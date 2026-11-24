"""Local HTTP server for the download engine tests (tests/engine.cpp).

URLs are http://127.0.0.1:<port>/<mode>/<size>/file.bin, plus GET /stats and GET /reset.
Byte i of version v of a file is (i * 31 + 7 + v) & 0xFF. Modes:
  etag     strong ETag, no Last-Modified; honors Range; If-Range other than the ETag gives 200
  norange  ignores Range: always 200 with Content-Length
  nolength ignores Range: 200 without Content-Length, closes the connection at the end
"""
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PIECE = 64 * 1024
PAUSE = 0.002
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

    def snapshot(self):
        with self.lock:
            return {"requests": self.requests, "ranged": self.ranged, "bytes": self.bytes,
                    "maxConcurrent": self.max_concurrent}


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
        match = re.fullmatch(r"/(etag|norange|nolength)/(\d+)/file\.bin", self.path)
        if not match:
            self.send_text(404, "not found", "text/plain")
            return
        mode, size, version = match.group(1), int(match.group(2)), 1
        etag, modified = VERSIONS[version]
        range_header = self.headers.get("Range")
        with STATS.lock:
            STATS.requests += 1
            if range_header and range_header != "bytes=0-0":
                STATS.ranged += 1
        first, last, status = 0, size - 1, 200
        # A range cannot be satisfied in an empty file; like many servers, answer 200 with no body.
        if mode == "etag" and range_header and size:
            parsed = re.fullmatch(r"bytes=(\d+)-(\d*)", range_header)
            if_range = self.headers.get("If-Range")
            if parsed and (if_range is None or if_range == etag):
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
        if mode == "etag":
            self.send_header("ETag", etag)
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
                    time.sleep(PAUSE)
        finally:
            if counted:
                with STATS.lock:
                    STATS.inflight -= 1


def start():
    """Starts the server on a thread; returns (server, base URL)."""
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"


if __name__ == "__main__":
    running, base = start()
    print(base, flush=True)
    threading.Event().wait()
