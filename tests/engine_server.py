"""Local HTTP server for the download engine tests (tests/engine.cpp).

URLs are http://127.0.0.1:<port>/<mode>/<size>/file.bin, plus GET /stats and GET /reset.
Byte i of version v of a file is (i * 31 + 7 + v) & 0xFF. Modes:
  etag     strong ETag, no Last-Modified; honors Range; If-Range other than the ETag gives 200
  norange  ignores Range: always 200 with Content-Length
  nolength ignores Range: 200 without Content-Length, closes the connection at the end
  modified Last-Modified only; honors Range; If-Range other than the date gives 200 with the whole file
  weak     ETag W/"skein-weak" only; honors Range
  rangeprobe  Last-Modified only; bytes=0-0 gets 206, every other request gets 200 with the whole file
  changed  like modified, serving version 1 until 3 ranged requests were answered since /reset,
           then version 2 with its date
  ignore-ifrange  like changed, but never looks at If-Range: always 206 for a Range
  basic    needs Authorization: Basic for user / pass, else 401 with a Basic challenge; then like etag
  bearer   needs Authorization: Bearer tok, else 401 with a Bearer challenge; then like etag
  basic-secret / bearer-secret  like basic / bearer, for user / pass-SECRET-456 and Bearer tok-SECRET-123
  redirect-out   needs Bearer tok; 302 to mode etag on the other host name (127.0.0.1 <-> localhost)
  redirect-back  302 to mode bearer on the same host name
  redirect-loop  302 to itself (a relative Location)
  forbidden      403 always
The same port answers as 127.0.0.1 and localhost (and ::1 when it can be bound). /stats has
"auth": the Host header's name (without port) to the Authorization values of file requests since /reset,
and "anonymous": the mode to the number of file requests without Authorization since /reset.
"""
import base64
import json
import socket
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PIECE = 64 * 1024
PAUSE = 0.002
# Ranged answers are slower, so a 10 MiB download on 4 connections lasts several of the engine's 100 ms
# progress intervals: the tests that pause or copy the staging folder at a byte count need a report mid-transfer.
RANGED_PAUSE = 0.01
PATTERN = bytes((i * 31 + 7) & 0xFF for i in range(256))

VERSIONS = {
    1: ('"skein-test-1"', "Mon, 02 Nov 2026 10:00:00 GMT"),
    2: ('"skein-test-2"', "Mon, 02 Nov 2026 10:00:05 GMT"),
}


def content(offset, length, version=1):
    """Bytes offset .. offset + length - 1 of the file in the given version."""
    start = (offset + 0) % 256
    rotated = bytes((b + version) & 0xFF for b in PATTERN)
    reps = (start + length) // 256 + 2
    return (rotated * reps)[start:start + length]


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        with self.lock:
            self.requests = 0
            self.ranged = 0
            self.bytes = 0
            self.inflight = 0
            self.max_concurrent = 0
            self.auth = {}
            self.anonymous = {}

    def snapshot(self):
        with self.lock:
            return {"requests": self.requests, "ranged": self.ranged, "bytes": self.bytes,
                    "maxConcurrent": self.max_concurrent,
                    "auth": {host: list(values) for host, values in self.auth.items()},
                    "anonymous": dict(self.anonymous)}


STATS = Stats()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def send_text(self, status, body, kind):
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        try:
            self.serve()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            self.close_connection = True

    def serve(self):
        if self.path == "/stats":
            self.send_text(200, json.dumps(STATS.snapshot()), "application/json")
            return
        if self.path == "/reset":
            STATS.reset()
            self.send_text(200, "ok", "text/plain")
            return
        match = re.fullmatch(r"/(etag|norange|nolength|modified|weak|rangeprobe|changed|ignore-ifrange|basic|bearer|basic-secret|bearer-secret"
                             r"|redirect-out|redirect-back|redirect-loop|forbidden)/(\d+)/file\.bin", self.path)
        if not match:
            self.send_text(404, "not found", "text/plain")
            return
        mode, size = match.group(1), int(match.group(2))
        range_header = self.headers.get("Range")
        authorization = self.headers.get("Authorization")
        # The Host header's name and port: "127.0.0.1:5000", "localhost:5000" or "[::1]:5000".
        host, _, port = self.headers.get("Host", "").rpartition(":")
        host = host.strip("[]")
        with STATS.lock:
            STATS.requests += 1
            if authorization is not None:
                STATS.auth.setdefault(host, []).append(authorization)
            else:
                STATS.anonymous[mode] = STATS.anonymous.get(mode, 0) + 1
        if mode == "forbidden":
            self.send_text(403, "forbidden", "text/plain")
            return
        if mode in ("basic", "bearer", "basic-secret", "bearer-secret", "redirect-out"):
            wanted = {"basic": "Basic " + base64.b64encode(b"user:pass").decode(),
                      "basic-secret": "Basic " + base64.b64encode(b"user:pass-SECRET-456").decode(),
                      "bearer-secret": "Bearer tok-SECRET-123"}.get(mode, "Bearer tok")
            if authorization != wanted:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="test"' if mode.startswith("basic") else "Bearer")
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", "12")
                self.end_headers()
                self.wfile.write(b"unauthorized")
                return
        if mode.startswith("redirect-"):
            if mode == "redirect-out":
                other = "localhost" if host == "127.0.0.1" else "127.0.0.1"
                location = f"http://{other}:{port}/etag/{size}/file.bin"
            elif mode == "redirect-back":
                location = f"http://{host}:{port}/bearer/{size}/file.bin"
            else:
                location = "file.bin"
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if mode in ("basic", "bearer", "basic-secret", "bearer-secret"):
            mode = "etag"
        with STATS.lock:
            # The changed modes switch to version 2 once 3 ranged requests were answered since /reset.
            version = 2 if mode in ("changed", "ignore-ifrange") and STATS.ranged >= 3 else 1
            if range_header and range_header != "bytes=0-0":
                STATS.ranged += 1
        etag, modified = VERSIONS[version]
        if mode == "weak":
            etag = 'W/"skein-weak"'
        # The validator the server compares If-Range with.
        validator = etag if mode == "etag" else modified
        first, last, status = 0, size - 1, 200
        # A range cannot be satisfied in an empty file; like many servers, answer 200 with no body.
        honors_range = mode in ("etag", "modified", "weak", "changed", "ignore-ifrange") or (mode == "rangeprobe" and range_header == "bytes=0-0")
        if honors_range and range_header and size:
            parsed = re.fullmatch(r"bytes=(\d+)-(\d*)", range_header)
            if_range = self.headers.get("If-Range")
            if parsed and (if_range is None or mode == "ignore-ifrange" or if_range == validator):
                first = int(parsed.group(1))
                last = min(int(parsed.group(2)) if parsed.group(2) else size - 1, size - 1)
                if first >= size or first > last:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                status = 206
        length = last - first + 1 if size else 0
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        if mode in ("etag", "weak"):
            self.send_header("ETag", etag)
        if mode in ("modified", "rangeprobe", "changed", "ignore-ifrange"):
            self.send_header("Last-Modified", modified)
        if honors_range:
            self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
        if mode == "nolength":
            self.send_header("Connection", "close")
            self.close_connection = True
        else:
            self.send_header("Content-Length", str(length))
        self.end_headers()
        counted = status == 206 and range_header != "bytes=0-0"
        if counted:
            with STATS.lock:
                STATS.inflight += 1
                STATS.max_concurrent = max(STATS.max_concurrent, STATS.inflight)
        try:
            sent = 0
            while sent < length:
                piece = min(PIECE, length - sent)
                self.wfile.write(content(first + sent, piece, version))
                with STATS.lock:
                    STATS.bytes += piece
                sent += piece
                if sent < length:
                    time.sleep(RANGED_PAUSE if status == 206 else PAUSE)
        finally:
            if counted:
                with STATS.lock:
                    STATS.inflight -= 1


def start():
    """Starts the server on a thread; returns (server, base URL)."""
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    # "localhost" may resolve to ::1 first; answering there too spares each request a refused connection.
    try:
        class Server6(ThreadingHTTPServer):
            address_family = socket.AF_INET6
        server6 = Server6(("::1", port), Handler)
        server6.daemon_threads = True
        threading.Thread(target=server6.serve_forever, daemon=True).start()
    except OSError:
        pass
    return server, f"http://127.0.0.1:{port}"


if __name__ == "__main__":
    running, base = start()
    print(base, flush=True)
    threading.Event().wait()
