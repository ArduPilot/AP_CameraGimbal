#!/usr/bin/env python3
"""Runs tests/test_dropbox against a fake Dropbox (OAuth and the files API) on
loopback TLS. It keeps files and upload sessions in memory and models what a
client has to cope with: 4 MiB hash blocks, incorrect_offset replies, a chunk
that is applied but never acknowledged, and 429 rate limits. The test steers
failures through GET /__control?... and reads the stored files with
GET /__control?state=1. Usage: test_dropbox.py <path to the test_dropbox binary>"""
import hashlib
import http.server
import json
import os
import sys
import tempfile
import threading
import urllib.parse

import test_gdrive as base

lock = threading.Lock()
state = {}
BLOCK = 4 * 1024 * 1024


def reset():
    state.clear()
    state.update(files={}, sessions={}, counter=0, token_n=0, valid_token=None, expire_token=False, error500=0,
                 ratelimit=0, drop_before=0, drop_after=0, corrupt=False, noscope=False)


reset()


def content_hash(data):
    blocks = b"".join(hashlib.sha256(data[i:i + BLOCK]).digest() for i in range(0, len(data), BLOCK))
    return hashlib.sha256(blocks).hexdigest()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, obj=None, raw=None, headers=()):
        body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        if state["error500"] > 0:
            state["error500"] -= 1
            self.reply(500, {"error": "server"})
            return False
        if state["ratelimit"] > 0:
            state["ratelimit"] -= 1
            self.reply(429, {"error_summary": "too_many_requests/.", "error": {".tag": "too_many_requests"}},
                       headers=[("Retry-After", "1")])
            return False
        if state["noscope"]:
            # What Dropbox says to a token that lacks a permission: a plain-text 400.
            self.reply(400, raw=b"Error in call to API function \"files/get_metadata\": Your app (ID: 1) is not permitted to access "
                       b"this endpoint because it does not have the required scope 'files.metadata.read'. The owner of the app can "
                       b"enable the scope for the app using the Permissions tab on the App Console.")
            return False
        if state["expire_token"]:
            state["expire_token"] = False
            state["valid_token"] = None
        if self.headers.get("Authorization") != "Bearer " + str(state["valid_token"]):
            self.reply(401, {"error_summary": "expired_access_token/.", "error": {".tag": "expired_access_token"}})
            return False
        return True

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        with lock:
            if url.path != "/__control":
                return self.reply(404, {})
            if "state" in query:
                lines = ["%s|%d|%s" % (shown, len(data), digest) for shown, data, digest in state["files"].values()]
                body = "\n".join(sorted(lines)).encode()
                return self.reply(200, raw=body)
            if "reset" in query:
                reset()
            for key in ("expire_token", "corrupt", "noscope"):
                if key in query:
                    state[key] = True
            for key in ("error500", "ratelimit", "drop_before", "drop_after"):
                if key in query:
                    state[key] = int(query[key][0])
            self.reply(200, {})

    def api_arg(self):
        return json.loads(self.headers.get("Dropbox-API-Arg", "{}"))

    def metadata(self, shown, data, digest):
        if state["corrupt"]:
            state["corrupt"] = False
            digest = "0" * 64
        return {".tag": "file", "name": shown.rsplit("/", 1)[-1], "path_display": shown, "size": len(data), "content_hash": digest}

    def store(self, path, data):
        digest = content_hash(bytes(data))
        state["files"][path.lower()] = (path, bytes(data), digest)
        return self.metadata(path, data, digest)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        url = urllib.parse.urlparse(self.path)
        with lock:
            drop_before = url.path.endswith("append_v2") and state["drop_before"] > 0 and length > 0
            if drop_before:
                state["drop_before"] -= 1
        if drop_before:
            self.rfile.read(length)  # take the data, apply nothing, and vanish
            self.close_connection = True
            self.connection.shutdown(2)
            return
        data = self.rfile.read(length)
        with lock:
            if url.path == "/oauth2/token":
                form = urllib.parse.parse_qs(data.decode())
                if form.get("grant_type") != ["refresh_token"] or form.get("refresh_token") != ["refresh-tok"] or \
                        form.get("client_id") != ["appkey"] or form.get("client_secret") != ["app/secret+="]:
                    return self.reply(400, {"error": "invalid_grant", "error_description": "refresh token is malformed"})
                state["token_n"] += 1
                state["valid_token"] = ("sl.u.token%d" % state["token_n"]) + "x" * 1300  # as long as the real ones
                return self.reply(200, {"access_token": state["valid_token"], "token_type": "bearer", "expires_in": 14400})
            if not self.authorized():
                return
            if url.path == "/2/files/get_metadata":
                path = json.loads(data)["path"]
                if path.endswith((" ", ".")):  # Dropbox refuses these names
                    return self.reply(409, {"error_summary": "path/malformed_path/.", "error": {".tag": "path",
                                                                                         "path": {".tag": "malformed_path"}}})
                entry = state["files"].get(path.lower())
                if not entry:
                    return self.reply(409, {"error_summary": "path/not_found/.", "error": {".tag": "path", "path": {".tag": "not_found"}}})
                return self.reply(200, self.metadata(*entry))
            if url.path == "/2/files/upload":
                path = self.api_arg()["path"]
                return self.reply(200, self.store(path, data))
            if url.path == "/2/files/upload_session/start":
                state["counter"] += 1
                sid = "session%d" % state["counter"]
                state["sessions"][sid] = bytearray(data)
                return self.reply(200, {"session_id": sid})
            if url.path in ("/2/files/upload_session/append_v2", "/2/files/upload_session/finish"):
                arg = self.api_arg()
                cursor = arg["cursor"]
                session = state["sessions"].get(cursor["session_id"])
                if session is None:
                    return self.reply(409, {"error_summary": "not_found/.", "error": {".tag": "lookup_failed",
                                                                                       "lookup_failed": {".tag": "not_found"}}})
                if cursor["offset"] != len(session):
                    return self.reply(409, {"error_summary": "incorrect_offset/.", "error": {
                        ".tag": "incorrect_offset", "correct_offset": len(session)}})
                session.extend(data)
                if url.path.endswith("append_v2"):
                    if state["drop_after"] > 0 and length > 0:
                        state["drop_after"] -= 1  # applied, but the reply is lost
                        self.close_connection = True
                        self.connection.shutdown(2)
                        return
                    return self.reply(200, raw=b"null")
                meta = self.store(arg["commit"]["path"], session)
                del state["sessions"][cursor["session_id"]]
                return self.reply(200, meta)
            self.reply(404, {"error": "no such path"})


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        server, cert = base.start_server(directory, Handler)
        import subprocess
        result = subprocess.run([binary, str(server.server_address[1]), cert, directory], timeout=300)
        server.shutdown()
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
