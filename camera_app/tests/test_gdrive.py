#!/usr/bin/env python3
"""Runs tests/test_gdrive against a fake Google Drive and OAuth server on
loopback TLS. The server keeps folders and files in memory; the test steers
failures through GET /__control?... and reads the stored files with
GET /__control?state. Usage: test_gdrive.py <path to the test_gdrive binary>"""
import hashlib
import http.server
import json
import os
import re
import socketserver
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

lock = threading.Lock()
state = {}


def reset():
    state.clear()
    state.update(folders={"root": ("", None)}, files={}, sessions={}, counter=0, token_n=0, valid_token=None,
                 expire_token=False, error500=0, drop_chunk=0, corrupt=False, stall=0, slow=False)


reset()


def new_id(prefix):
    state["counter"] += 1
    return "%s%d" % (prefix, state["counter"])


def tree_path(folder_id):
    parts = []
    while folder_id and folder_id != "root":
        name, parent = state["folders"][folder_id]
        parts.append(name)
        folder_id = parent
    return "/".join(reversed(parts))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, obj=None, headers=()):
        body = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def authorized(self):
        """Common checks for API calls; True if the call may go on."""
        if state["error500"] > 0:
            state["error500"] -= 1
            self.reply(500, {"error": "backend"})
            return False
        if state["expire_token"]:
            state["expire_token"] = False
            state["valid_token"] = None
        if self.headers.get("Authorization") != "Bearer " + str(state["valid_token"]):
            self.reply(401, {"error": {"code": 401}})
            return False
        return True

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        with lock:
            if url.path == "/__control":
                return self.control(query)
            if not self.authorized():
                return
            if url.path == "/drive/v3/files":
                return self.search(query["q"][0])
            self.reply(404, {"error": "no such path"})

    def control(self, query):
        if "state" in query:
            lines = []
            for fid, (name, parent, data, md5) in state["files"].items():
                lines.append("%s/%s|%d|%s" % (tree_path(parent), name, len(data), md5))
            body = "\n".join(sorted(lines)).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if "reset" in query:
            reset()
        for key in ("expire_token", "corrupt", "slow"):
            if key in query:
                state[key] = True
        for key in ("error500", "drop_chunk", "stall"):
            if key in query:
                state[key] = int(query[key][0])
        self.reply(200, {})

    def search(self, q):
        name = re.search(r"name = '((?:[^'\\]|\\.)*)'", q).group(1)
        name = re.sub(r"\\(.)", r"\1", name)
        parent = re.search(r"'([^']+)' in parents", q).group(1)
        want_folder = "mimeType = 'application/vnd.google-apps.folder'" in q
        found = []
        if want_folder:
            for fid, (n, p) in state["folders"].items():
                if n == name and p == parent:
                    found.append({"id": fid, "name": n})
        else:
            for fid, (n, p, data, md5) in state["files"].items():
                if n == name and p == parent:
                    found.append({"id": fid, "name": n, "size": str(len(data)), "md5Checksum": md5})
        self.reply(200, {"files": found})

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        data = self.body()
        with lock:
            if url.path == "/token":
                form = urllib.parse.parse_qs(data.decode())
                if form["client_id"][0] == "bad" or form["grant_type"][0] != "refresh_token" or \
                        form["refresh_token"][0] != "1//refresh" or form["client_secret"][0] != "secret/+=":
                    return self.reply(400, {"error": "invalid_grant", "error_description": "Token has been expired or revoked."})
                state["token_n"] += 1
                state["valid_token"] = "token%d" % state["token_n"]
                return self.reply(200, {"access_token": state["valid_token"], "expires_in": 3599, "token_type": "Bearer"})
            if not self.authorized():
                return
            meta = json.loads(data or b"{}")
            if url.path == "/drive/v3/files":
                if meta.get("mimeType") == "application/vnd.google-apps.folder":
                    fid = new_id("folder")
                    state["folders"][fid] = (meta["name"], meta["parents"][0])
                    return self.reply(200, {"id": fid})
                fid = new_id("file")
                state["files"][fid] = (meta["name"], meta["parents"][0], b"", hashlib.md5(b"").hexdigest())
                return self.reply(200, {"id": fid, "size": "0", "md5Checksum": hashlib.md5(b"").hexdigest()})
            if url.path == "/upload/drive/v3/files" and query.get("uploadType") == ["resumable"]:
                return self.start_session(None, meta)
            self.reply(404, {"error": "no such path"})

    def do_PATCH(self):
        url = urllib.parse.urlparse(self.path)
        data = self.body()
        with lock:
            if not self.authorized():
                return
            m = re.fullmatch(r"/upload/drive/v3/files/([^/]+)", url.path)
            if m and m.group(1) in state["files"]:
                return self.start_session(m.group(1), json.loads(data or b"{}"))
            self.reply(404, {"error": "no such file"})

    def start_session(self, existing, meta):
        sid = new_id("session")
        total = int(self.headers["X-Upload-Content-Length"])
        state["sessions"][sid] = dict(existing=existing, name=meta.get("name"), parent=(meta.get("parents") or [None])[0],
                                      total=total, data=bytearray())
        self.reply(200, None, [("Location", "https://localhost:%d/upload/drive/v3/files?uploadType=resumable&upload_id=%s" %
                                (self.server.server_address[1], sid))])

    def do_PUT(self):
        url = urllib.parse.urlparse(self.path)
        sid = urllib.parse.parse_qs(url.query).get("upload_id", [""])[0]
        length = int(self.headers.get("Content-Length", 0))
        deadline = time.monotonic() + 30
        while state["slow"] and length and time.monotonic() < deadline:
            time.sleep(0.05)  # a slow link: hold the data back until released
        with lock:
            session = state["sessions"].get(sid)
            if state["drop_chunk"] > 0 and length > 0 and session is not None:
                # Take half the chunk, then lose the connection.
                half = self.rfile.read(length // 2)
                session["data"].extend(half[: len(half) // 262144 * 262144])
                state["drop_chunk"] -= 1
                self.close_connection = True
                self.connection.shutdown(2)
                return
        data = self.rfile.read(length)
        with lock:
            if session is None:
                return self.reply(404, {"error": "no session"})
            if state["stall"] > 0 and length > 0:
                state["stall"] -= 1  # take nothing, say nothing was kept
                return self.reply(308, None)
            m = re.fullmatch(r"bytes (?:(\d+)-(\d+)|\*)/(\d+)", self.headers["Content-Range"])
            if length:
                start = int(m.group(1))
                if start != len(session["data"]):
                    return self.reply(400, {"error": "bad offset %d, have %d" % (start, len(session["data"]))})
                session["data"].extend(data)
            if len(session["data"]) < session["total"]:
                headers = [("Range", "bytes=0-%d" % (len(session["data"]) - 1))] if session["data"] else []
                return self.reply(308, None, headers)
            md5 = hashlib.md5(bytes(session["data"])).hexdigest()
            if state["corrupt"]:
                state["corrupt"] = False
                md5 = "0" * 32
            fid = session["existing"] or new_id("file")
            if session["existing"]:
                name, parent = state["files"][fid][:2]
            else:
                name, parent = session["name"], session["parent"]
            state["files"][fid] = (name, parent, bytes(session["data"]), md5)
            del state["sessions"][sid]
            self.reply(200, {"id": fid, "size": str(len(state["files"][fid][2])), "md5Checksum": md5})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # a client that hangs up mid-request (an upload being stopped) is expected

    def get_request(self):
        sock, address = super().get_request()
        try:
            return self.context.wrap_socket(sock, server_side=True), address
        except (ssl.SSLError, OSError):
            sock.close()
            raise OSError("handshake failed")


def start_server(directory, handler=None):
    """Starts a fake server (the fake Drive unless `handler` is given) on a free loopback
    port. Returns (server, CA file). The certificate is for "localhost"."""
    key, cert = os.path.join(directory, "k.key"), os.path.join(directory, "c.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                    "-days", "2", "-subj", "/CN=localhost", "-keyout", key, "-out", cert,
                    "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"], check=True, capture_output=True)
    server = Server(("127.0.0.1", 0), handler or Handler)
    server.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server.context.load_cert_chain(cert, key)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, cert


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        server, cert = start_server(directory)
        result = subprocess.run([binary, str(server.server_address[1]), cert, directory], timeout=300)
        server.shutdown()
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
