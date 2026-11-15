"""Concurrency acceptance tests using local sockets and real C client threads."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time

from integration import ROOT, SMALL, Origin, OriginServer, free_port


def main():
    checks = 0
    with OriginServer(("127.0.0.1", 0), Origin) as origin, tempfile.TemporaryDirectory() as tmp:
        threading.Thread(target=origin.serve_forever, daemon=True).start()
        origin_port = origin.server_address[1]
        port = free_port()
        base = [str(ROOT / "proxy_client"), "127.0.0.1", str(port)]
        url = lambda path: f"http://127.0.0.1:{origin_port}{path}"
        held = []
        with tempfile.TemporaryFile(mode="w+b") as log:
            proxy = subprocess.Popen([str(ROOT / "proxy_server"), str(port)], stderr=log, stdout=log)
            try:
                for _ in range(100):
                    assert proxy.poll() is None, "Proxy exited"
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.03)
                else:
                    raise AssertionError("Proxy did not start")

                def client(args, success=True):
                    p = subprocess.run(base + args, cwd=tmp, capture_output=True, timeout=40)
                    assert (p.returncode == 0) == success, (p.stdout, p.stderr)
                    assert b"AddressSanitizer" not in p.stderr and b"runtime error:" not in p.stderr
                    return p

                def batch(paths, jobs=3, success=True):
                    source = Path(tmp) / "urls.txt"
                    source.write_text("# local test\n\n" + "\n".join(url(p) for p in paths) + "\n")
                    before = set((Path(tmp) / "downloads").glob("batch_*"))
                    p = client(["--batch", str(source), "--jobs", str(jobs)], success)
                    created = set((Path(tmp) / "downloads").glob("batch_*")) - before
                    assert len(created) == 1
                    return created.pop(), p

                directory, _ = batch([f"/unique/{i}" for i in range(12)])
                files = sorted(directory.glob("*.html"))
                assert len(files) == 12
                for i, file in enumerate(files):
                    assert file.read_bytes() == f"<html>/unique/{i}</html>".encode()
                checks += 1

                # Observe actual overlap and the configured client limit at the origin.
                with Origin.condition:
                    assert Origin.condition.wait_for(lambda: Origin.active == 0, timeout=5)
                    Origin.peak = 0
                directory, _ = batch(["/slow"] * 8, jobs=3)
                assert 2 <= Origin.peak <= 3, Origin.peak
                assert len(list(directory.glob("*.html"))) == 8
                assert all(p.read_bytes() == SMALL for p in directory.glob("*.html"))
                checks += 1

                # The same URL is assigned a distinct output before workers start.
                first, _ = batch(["/"] * 8, jobs=4)
                second, _ = batch(["/"] * 8, jobs=4)
                assert first != second and len(list(first.glob("*.html"))) == 8
                assert len(list(second.glob("*.html"))) == 8
                checks += 1

                directory, result = batch(["/", "/404", "/truncated", "/unique/good"], success=False)
                assert b"2 saved, 2 failed" in result.stdout
                assert len(list(directory.glob("*.html"))) == 2
                assert not list(directory.glob("*.part*"))
                checks += 1

                source = Path(tmp) / "invalid.txt"
                for contents in ["# empty\n", "ftp://example.com/\n", "http://bad host/\n", "http://x/\x00junk\n"]:
                    source.write_text(contents)
                    client(["--batch", str(source)], success=False)
                client(["--batch", str(source), "--jobs", "0"], success=False)
                client(["--batch", str(source), "--jobs", "17"], success=False)
                checks += 1

                # Keep one upstream blocked: a fast request must still finish.
                with ThreadPoolExecutor(max_workers=1) as pool:
                    slow = pool.submit(client, [url("/slowflag"), "--output", "slow.html"])
                    assert Origin.slow_started.wait(5)
                    try:
                        client([url("/"), "--output", "fast.html"])
                        assert not slow.done(), "Slow request unexpectedly released before fast request completed"
                    finally:
                        Origin.slow_release.set()
                    slow.result()
                checks += 1

                # Cumulative requests exceed the tutorial's fixed 400-entry arrays.
                for i in range(410):
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                        sock.sendall(f"GET {url('/')} HTTP/1.0\r\n\r\n".encode())
                        data = b""
                        while True:
                            block = sock.recv(8192)
                            if not block:
                                break
                            data += block
                        assert data.split(b"\r\n\r\n", 1)[1] == SMALL, i
                checks += 1

                # Fill all 32 server slots with genuinely active upstream requests.
                with Origin.condition:
                    assert Origin.condition.wait_for(lambda: Origin.active == 0, timeout=5)
                for _ in range(32):
                    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                    sock.sendall(f"GET {url('/hold')} HTTP/1.0\r\n\r\n".encode())
                    held.append(sock)
                with Origin.condition:
                    assert Origin.condition.wait_for(lambda: Origin.held == 32, timeout=5), Origin.held
                with socket.create_connection(("127.0.0.1", port), timeout=5) as extra:
                    extra.sendall(f"GET {url('/')} HTTP/1.0\r\n\r\n".encode())
                    assert b"503" in extra.recv(4096).split(b"\r\n")[0]
                assert len(os.listdir(f"/proc/{proxy.pid}/task")) <= 33
                Origin.hold_release.set()
                for sock in held:
                    while sock.recv(8192):
                        pass
                    sock.close()
                held.clear()
                checks += 1

                # Disconnect during a large response using TCP reset, then verify recovery.
                with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                    sock.sendall(f"GET {url('/large')} HTTP/1.0\r\n\r\n".encode())
                    sock.recv(128)
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                client([url("/"), "--output", "after_disconnect.html"])
                checks += 1

                # An idle partial request times out, releasing its detached worker.
                with socket.create_connection(("127.0.0.1", port), timeout=20) as idle:
                    idle.sendall(b"GET ")
                    assert idle.recv(1) == b""
                client([url("/"), "--output", "after_timeout.html"])
                assert not list(Path(tmp).rglob("*.part*"))
                assert proxy.poll() is None
                checks += 1
                print(f"PASS: {checks} concurrency scenarios (410 sequential requests, 32 server slots, bounded C batch workers)")
            finally:
                Origin.hold_release.set()
                Origin.slow_release.set()
                for sock in held:
                    sock.close()
                proxy.terminate()
                proxy.wait(timeout=5)
                origin.shutdown()
                log.seek(0)
                text = log.read().decode(errors="replace")
                assert "AddressSanitizer" not in text and "runtime error:" not in text, text
                assert "worker started" in text and "worker finished" in text


if __name__ == "__main__":
    main()
