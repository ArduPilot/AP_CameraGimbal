#!/usr/bin/env python3
"""Runs tests/test_https against a loopback TLS server with a self-signed
certificate. Usage: test_https.py <path to the test_https binary>"""
import http.server
import os
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time

connections = 0


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        if status != 204:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_PUT(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.reply(200, ("%d:%d" % (len(data), sum(data))).encode())

    def do_GET(self):
        path = self.path
        if path == "/json":
            self.reply(200, b'{"ok":true}')
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"hello", b" world"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\nX-Trailer: 1\r\n\r\n")
        elif path == "/count":
            self.reply(200, str(connections).encode())
        elif path == "/nobody":
            self.reply(204)
        elif path == "/resume":
            self.reply(308, b"", [("Range", "bytes=0-99"), ("Location", "https://example.test/next"), ("Retry-After", "7")])
        elif path == "/big":
            self.reply(200, b"x" * 5000)
        elif path == "/close":
            self.reply(200, b"bye", [("Connection", "close")])
            self.close_connection = True
        elif path == "/silentclose":
            self.reply(200, b"bye")
            self.close_connection = True
        elif path == "/eof":
            self.send_response(200)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"until close")
            self.close_connection = True
        elif path == "/eofsame":
            # headers and body in one TLS record: the body is already buffered when the headers have been read
            self.wfile.write(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\narrived with the headers")
            self.close_connection = True
        elif path == "/slow":
            time.sleep(5)
            self.reply(200, b"late")
        else:
            self.reply(404, b"not found")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def get_request(self):
        global connections
        sock, address = super().get_request()
        try:
            sock = self.context.wrap_socket(sock, server_side=True)
        except (ssl.SSLError, OSError):
            sock.close()
            raise OSError("handshake failed")
        connections += 1
        return sock, address


def make_cert(directory, name):
    key, cert = os.path.join(directory, name + ".key"), os.path.join(directory, name + ".pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                    "-nodes", "-days", "2", "-subj", "/CN=localhost", "-keyout", key, "-out", cert,
                    "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                   check=True, capture_output=True)
    return key, cert


def main():
    global connections
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        key, cert = make_cert(directory, "server")
        _, other = make_cert(directory, "other")
        server = Server(("127.0.0.1", 0), Handler)
        server.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server.context.load_cert_chain(cert, key)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        result = subprocess.run([binary, str(server.server_address[1]), cert, other], timeout=120)
        server.shutdown()
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
