"""Local end-to-end tests: real TCP origin -> proxy -> client -> saved HTML."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import socket
import socketserver
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
SMALL = b"<!doctype html><html><body>Hello proxy!</body></html>\n"
LARGE = b"<html>" + (b"hello\x00world\n" * 20000) + b"</html>"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Origin(socketserver.BaseRequestHandler):
    condition = threading.Condition()
    active = 0
    peak = 0
    held = 0
    hold_release = threading.Event()
    slow_started = threading.Event()
    slow_release = threading.Event()

    def handle(self):
        with Origin.condition:
            Origin.active += 1
            Origin.peak = max(Origin.peak, Origin.active)
        try:
            self.request.settimeout(5)
            request = b""
            while b"\r\n\r\n" not in request:
                block = self.request.recv(8192)
                if not block:
                    return
                request += block
            # This also verifies the client is using our proxy, not fetching directly.
            assert b"User-Agent: OSProjectProxy/1.0" in request
            path = request.split(b" ")[1]
            if path == b"/hold":
                with Origin.condition:
                    Origin.held += 1
                    Origin.condition.notify_all()
                Origin.hold_release.wait(10)
            if path == b"/slowflag":
                Origin.slow_started.set()
                Origin.slow_release.wait(10)
            body = LARGE if path == b"/large" else SMALL
            if path.startswith(b"/unique/"):
                body = b"<html>" + path + b"</html>"
            if path == b"/slow":
                time.sleep(0.4)
            if path == b"/chunked":
                chunks = [SMALL[:13], SMALL[13:]]
                payload = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n"
                payload += b"".join(f"{len(c):x};demo=yes\r\n".encode() + c + b"\r\n" for c in chunks)
                payload += b"0\r\nX-Test: complete\r\n\r\n"
            elif path == b"/close":
                payload = b"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n" + body
            elif path == b"/truncated":
                payload = b"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: 999\r\n\r\nshort"
            elif path == b"/badchunk":
                payload = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc"
            elif path == b"/conflict":
                payload = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 3\r\nContent-Length: 9\r\n\r\nabc"
            elif path == b"/both":
                payload = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
            elif path == b"/huge":
                payload = b"HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: 999999999\r\n\r\n"
            elif path == b"/longheader":
                payload = b"HTTP/1.0 200 OK\r\nX-Long: " + b"a" * 70000 + b"\r\n\r\n"
            elif path == b"/empty":
                payload = b""
            else:
                status = b"404 Not Found" if path == b"/404" else b"302 Found" if path == b"/redirect" else b"200 OK"
                mime = b"application/octet-stream" if path == b"/binary" else b"text/html; charset=utf-8"
                encoding = b"Content-Encoding: gzip\r\n" if path == b"/gzip" else b""
                payload = (b"HTTP/1.0 " + status + b"\r\nContent-Type: " + mime + b"\r\n"
                           + encoding + f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
            if path == b"/split":
                # Deliberately fragment the header terminator and body across writes.
                for byte in payload[:100]:
                    self.request.sendall(bytes([byte]))
                    time.sleep(0.001)
                self.request.sendall(payload[100:])
            else:
                self.request.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with Origin.condition:
                Origin.active -= 1
                Origin.condition.notify_all()


class OriginServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 64


def main():
    count = 0
    with OriginServer(("127.0.0.1", 0), Origin) as origin, tempfile.TemporaryDirectory() as tmp:
        threading.Thread(target=origin.serve_forever, daemon=True).start()
        port = free_port()
        origin_port = origin.server_address[1]
        with tempfile.TemporaryFile(mode="w+b") as log:
            proxy = subprocess.Popen([str(ROOT / "proxy_server"), str(port)], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    if proxy.poll() is not None:
                        log.seek(0)
                        raise AssertionError(log.read().decode())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.03)
                else:
                    raise AssertionError("Proxy did not start")

                def run(path, filename=None, success=True, url=None):
                    command = [str(ROOT / "proxy_client"), "127.0.0.1", str(port),
                               url or f"http://127.0.0.1:{origin_port}{path}"]
                    if filename is not None:
                        command += ["--output", filename]
                    result = subprocess.run(command, cwd=tmp, capture_output=True, timeout=25)
                    assert (result.returncode == 0) == success, (command, result.stdout, result.stderr)
                    assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
                    return result

                for path, body in [("/", SMALL), ("/large", LARGE), ("/split", SMALL),
                                   ("/chunked", SMALL), ("/close", SMALL), ("/?q=one#fragment", SMALL)]:
                    name = f"success_{count}.html"
                    run(path, name)
                    assert (Path(tmp) / name).read_bytes() == body
                    count += 1

                run("/", "existing.html")
                run("/large", "existing.html", success=False)
                assert (Path(tmp) / "existing.html").read_bytes() == SMALL
                count += 1

                run("/same.html?one=1")
                run("/same.html?two=2")
                downloads = list((Path(tmp) / "downloads").glob("*.html"))
                assert len(downloads) == 2 and all(p.read_bytes() == SMALL for p in downloads)
                count += 1

                for path in ["/truncated", "/badchunk", "/conflict", "/both", "/huge", "/longheader",
                             "/empty", "/404", "/redirect", "/binary", "/gzip"]:
                    failed = run(path, "failed.html", success=False)
                    if path == "/redirect":
                        assert b"Redirect following" in failed.stderr
                    elif path == "/404":
                        assert b"HTTP 404" in failed.stderr and b"Redirect" not in failed.stderr
                    assert not (Path(tmp) / "failed.html").exists()
                    assert not list(Path(tmp).rglob("*.part*"))
                    count += 1

                run("/", "missing/fail.html", success=False)
                run("/", success=False, url="https://example.com/")
                run("/", success=False, url="http://bad host/")
                dead_port = free_port()
                failed = run("/", success=False, url=f"http://127.0.0.1:{dead_port}/")
                assert b"Bad Gateway" in failed.stderr and b"Redirect" not in failed.stderr
                count += 4

                def fetch_unique(i):
                    path = f"/unique/{i}"
                    name = f"parallel_{i}.html"
                    run(path, name)
                    assert (Path(tmp) / name).read_bytes() == b"<html>" + path.encode() + b"</html>"

                with ThreadPoolExecutor(max_workers=8) as pool:
                    list(pool.map(fetch_unique, range(16)))
                count += 1

                # Invalid and oversized raw requests must not crash the server.
                for request, status in [(b"GET / HTTP/1.0\r\n\r\n", b"400"),
                                        (b"POST http://example.com/ HTTP/1.0\r\n\r\n", b"405"),
                                        (b"GET http://example.com/ HTTP/1.0\r\nX: " + b"a" * 17000, b"431"),
                                        (b"GET http://example.com/ HTTP/1.0\r\nX: \x00\r\n\r\n", b"400")]:
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                        sock.sendall(request)
                        assert status in sock.recv(4096).split(b"\r\n")[0]
                    count += 1
                run("/", "after_errors.html")
                assert proxy.poll() is None
                count += 1
                print(f"PASS: {count} end-to-end scenarios (including 16 concurrent downloads)")
            finally:
                proxy.terminate()
                proxy.wait(timeout=5)
                log.seek(0)
                server_log = log.read().decode(errors="replace")
                assert "AddressSanitizer" not in server_log and "runtime error:" not in server_log, server_log
                origin.shutdown()


if __name__ == "__main__":
    main()
