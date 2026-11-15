"""Controlled HTTP/HTTPS, redirect, certificate, and decoded-body acceptance tests."""
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import gzip
import os
import socket
import ssl
import subprocess
import tempfile
import threading
import time

from integration import ROOT, free_port

HTML = b"<!doctype html><html><body>Verified HTTPS through our C proxy.</body></html>\n"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def do_GET(self):
        try:
            assert self.headers.get("User-Agent") == "OSProjectProxy/1.0"
            path = self.path.split("?", 1)[0]
            location = None
            if path == "/relative":
                location = "/page?redirected=yes"
            elif path == "/to-https":
                location = self.server.https_url + "/page"
            elif path == "/to-http":
                location = self.server.http_url + "/page"
            elif path == "/loop":
                location = "/loop"
            elif path == "/file-redirect":
                location = "file:///etc/hosts"
            elif path == "/ftp-redirect":
                location = "ftp://127.0.0.1/file"
            elif path.startswith("/chain/"):
                remaining = int(path.rsplit("/", 1)[1])
                if remaining:
                    location = f"/chain/{remaining - 1}"
            if location:
                self.send_response(302)
                self.send_header("Location", location)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", "13")
                self.end_headers()
                self.wfile.write(b"redirect-body")
                return
            if path == "/timeout":
                self.server.timeout_release.wait(25)
            body = HTML
            if path == "/query":
                body = b"<html>" + self.path.encode() + b"</html>"
            if path == "/large":
                body = b"<html>" + b"secure data\n" * 20000 + b"</html>"
            if path == "/bomb":
                body = b"A" * (51 * 1024 * 1024)
            if path in ("/gzip", "/bomb"):
                body = gzip.compress(body)
            self.send_response(404 if path == "/missing" else 200)
            if path != "/missing-type":
                self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Connection", "close")
            self.close_connection = True
            if path == "/chunked":
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                for chunk in [body[:7], body[7:]]:
                    self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                self.wfile.write(b"0\r\n\r\n")
                return
            if path in ("/gzip", "/bomb"):
                self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(body) + 50 if path == "/truncated" else len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass


def certificates(directory):
    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=directory, check=True, capture_output=True)
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=OS Proxy Test CA", "-addext", "basicConstraints=critical,CA:TRUE",
            "-keyout", "ca.key", "-out", "ca.pem")
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost",
            "-keyout", "server.key", "-out", "server.csr")
    (directory / "extensions.cnf").write_text(
        "subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\nbasicConstraints=CA:FALSE\n")
    openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
            "-CAcreateserial", "-days", "1", "-extfile", "extensions.cnf", "-out", "server.pem")


def main():
    checks = 0
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        certificates(directory)
        http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        https = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(directory / "server.pem", directory / "server.key")
        https.socket = context.wrap_socket(https.socket, server_side=True)
        http_url = f"http://127.0.0.1:{http.server_port}"
        https_url = f"https://localhost:{https.server_port}"
        for server in (http, https):
            server.http_url, server.https_url = http_url, https_url
            server.timeout_release = threading.Event()
            threading.Thread(target=server.serve_forever, daemon=True).start()
        proxies, logs = [], []
        try:
            def start_proxy(trusted):
                port = free_port()
                env = os.environ.copy()
                env.pop("PROXY_CA_BUNDLE", None)
                if trusted:
                    env["PROXY_CA_BUNDLE"] = str(directory / "ca.pem")
                log = tempfile.TemporaryFile(mode="w+b")
                logs.append(log)
                process = subprocess.Popen([str(ROOT / "proxy_server"), str(port)], env=env, stdout=log, stderr=log)
                proxies.append(process)
                for _ in range(100):
                    assert process.poll() is None, "Proxy exited"
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            return port
                    except OSError:
                        time.sleep(0.03)
                raise AssertionError("Proxy did not start")

            trusted = start_proxy(True)
            untrusted = start_proxy(False)
            def download(url, success=True, contains=None, expected=HTML, port=trusted):
                nonlocal checks
                output = directory / f"result_{checks}.html"
                result = subprocess.run([str(ROOT / "proxy_client"), "127.0.0.1", str(port), url,
                                         "--output", str(output)], cwd=tmp, capture_output=True, timeout=35)
                assert (result.returncode == 0) == success, (url, result.stdout, result.stderr)
                assert b"AddressSanitizer" not in result.stderr and b"runtime error:" not in result.stderr
                if success:
                    assert output.read_bytes() == expected
                else:
                    assert not output.exists()
                if contains:
                    assert contains in result.stdout + result.stderr, (url, result.stdout, result.stderr)
                assert not list(directory.glob("*.part*"))
                checks += 1
                return result

            download(http_url + "/page")
            download(https_url + "/page", contains=b"0 redirects")
            download(https_url + "/page", success=False, port=untrusted, contains=b"certificate verification failed")
            download(f"https://127.0.0.1:{https.server_port}/page", success=False, contains=b"certificate verification failed")
            download(http_url + "/to-https", contains=b"1 redirects")
            download(https_url + "/to-http", contains=b"1 redirects")
            download(https_url + "/relative", contains=b"/page?redirected=yes (1 redirects)")
            download(https_url + "/chain/5", contains=b"5 redirects")
            download(https_url + "/chain/6", success=False, contains=b"limit of 5 redirects")
            download(https_url + "/loop", success=False, contains=b"limit of 5 redirects")
            download(https_url + "/file-redirect", success=False, contains=b"HTTP and HTTPS")
            download(https_url + "/ftp-redirect", success=False, contains=b"HTTP and HTTPS")
            download("file:///etc/hosts", success=False, contains=b"Only http:// and https://")
            download(https_url + "/query?q=a%20b&n=2#ignored", expected=b"<html>/query?q=a%20b&n=2</html>")
            download(https_url + "/gzip")
            download(https_url + "/chunked")
            download(https_url + "/large", expected=b"<html>" + b"secure data\n" * 20000 + b"</html>")
            download(https_url + "/bomb", success=False, contains=b"50 MiB")
            download(https_url + "/truncated", success=False, contains=b"incomplete")
            download(https_url + "/missing", success=False, contains=b"HTTP 404")
            download(https_url + "/missing-type", success=False, contains=b"Expected HTML")

            # Mixed HTTP/HTTPS batch, including redirects, must keep result files distinct.
            urls = directory / "urls.txt"
            urls.write_text("\n".join([http_url + "/page", https_url + "/gzip",
                                       http_url + "/to-https", https_url + "/relative"]))
            result = subprocess.run([str(ROOT / "proxy_client"), "127.0.0.1", str(trusted), "--batch",
                                     str(urls), "--jobs", "4"], cwd=tmp, capture_output=True, timeout=35)
            assert result.returncode == 0, (result.stdout, result.stderr)
            files = list((directory / "downloads").rglob("*.html"))
            assert len(files) == 4 and all(f.read_bytes() == HTML for f in files)
            checks += 1

            # A real stalled upstream returns 504 before the client's receive deadline.
            download(http_url + "/timeout", success=False, contains=b"HTTP 504 Gateway Timeout")
            http.timeout_release.set()
            download(https_url + "/page")
            print(f"PASS: {checks} HTTPS/redirect scenarios (verified certificates, gzip, size cap, timeout, mixed batch)")
        finally:
            for server in (http, https):
                server.timeout_release.set()
                server.shutdown()
                server.server_close()
            for process in proxies:
                process.terminate()
                process.wait(timeout=5)
            for log in logs:
                log.seek(0)
                text = log.read().decode(errors="replace")
                assert "AddressSanitizer" not in text and "runtime error:" not in text, text
                log.close()


if __name__ == "__main__":
    main()
