"""Optional internet smoke test. Deterministic checks remain in make test."""
from pathlib import Path
import os
import socket
import subprocess
import tempfile
import time

from integration import ROOT, free_port


def main():
    with tempfile.TemporaryDirectory() as tmp, tempfile.TemporaryFile(mode="w+b") as log:
        port = free_port()
        env = os.environ.copy()
        env.pop("PROXY_CA_BUNDLE", None)
        proxy = subprocess.Popen([str(ROOT / "proxy_server"), str(port)], env=env, stdout=log, stderr=log)
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
            for i, url in enumerate(["http://example.com/", "https://example.com/"]):
                output = Path(tmp) / f"page_{i}.html"
                result = subprocess.run([str(ROOT / "proxy_client"), "127.0.0.1", str(port), url,
                                         "--output", str(output)], capture_output=True, timeout=35)
                print(result.stdout.decode(), end="", flush=True)
                assert result.returncode == 0, result.stderr.decode()
                assert output.stat().st_size > 0
            print("PASS: public HTTP and HTTPS pages fetched through the C proxy")
        finally:
            proxy.terminate()
            proxy.wait(timeout=5)
            log.seek(0)
            text = log.read().decode(errors="replace")
            assert "AddressSanitizer" not in text and "runtime error:" not in text, text


if __name__ == "__main__":
    main()
