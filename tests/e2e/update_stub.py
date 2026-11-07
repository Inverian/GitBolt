"""A loopback stand-in for GitHub's releases API and its asset downloads,
for the in-app update e2e tests (test_update_*.py).

GitBolt asks it only because the test points GITBOLT_TEST_UPDATE_FEED at
it, which the app reads in a test session alone, and only plain http to
127.0.0.1 (src/ui/UpdateSetup.h): no test ever reaches api.github.com.

`UpdateFeed` answers
  - GET /repos/Inverian/GitBolt/releases/latest: a releases/latest
    answer for `tag` (draft and prerelease false, html_url under
    Inverian/GitBolt's releases), listing every asset added;
  - GET /download/<tag>/<name>: that asset's bytes, from memory or
    streamed from a file, optionally slowed down (`throttle`).
Every request is recorded in `requests`, so a test can prove that
nothing asked, or that something asked exactly once.
"""

from __future__ import annotations

import hashlib
import http.server
import json
import os
import sys
import threading
import time

REPO = "Inverian/GitBolt"
FEED_PATH = f"/repos/{REPO}/releases/latest"
CHECKSUMS = "SHA256SUMS.txt"

NOTES = """## [{version}] - 2026-11-01

### Added
- **Something new**: the notes are GitHub's markdown, shown as written:
  <b>not bold</b>, [not a link](https://example.invalid/), `code`.

### Fixed
- **Something old**: fixed.
"""


class _Asset:
    def __init__(self, name: str, data: bytes | None, path: str | None):
        self.name = name
        self.data = data
        self.path = path
        if data is not None:
            self.size = len(data)
            self.sha256 = hashlib.sha256(data).hexdigest()
        else:
            self.size = os.path.getsize(path)
            digest = hashlib.sha256()
            with open(path, "rb") as fh:
                for block in iter(lambda: fh.read(1 << 20), b""):
                    digest.update(block)
            self.sha256 = digest.hexdigest()


class UpdateFeed:
    """The feed and its downloads on 127.0.0.1, port chosen by the OS."""

    def __init__(self, version: str = "0.1.1") -> None:
        self.version = version
        self.notes = NOTES.format(version=version)
        self.assets: dict[str, _Asset] = {}
        # Change these between requests to change the answers.
        self.feed_status = 200
        self.feed_delay = 0.0               # seconds before the feed answers
        self.throttle: tuple[int, float] | None = None   # (bytes, seconds) per chunk of a download
        self.with_digests = True            # the API's "digest" for each asset
        self.requests: list[tuple[str, str]] = []
        self._lock = threading.Lock()
        feed = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                with feed._lock:
                    feed.requests.append(("GET", self.path))
                if self.path == FEED_PATH:
                    feed._answer_feed(self)
                elif self.path.startswith("/download/"):
                    feed._answer_download(self)
                else:
                    self._send(404, b"not found", "text/plain")

            def _send(self, status: int, body: bytes, kind: str) -> None:
                self.send_response(status)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args) -> None:
                pass    # keep the test's output to its verdict

        class Server(http.server.ThreadingHTTPServer):
            daemon_threads = True

            def handle_error(self, request, client_address) -> None:
                # The app cancelled a download, or quit: not the test's
                # business. Anything else is printed as usual.
                if isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
                    return
                super().handle_error(request, client_address)

        self.httpd = Server(("127.0.0.1", 0), Handler)
        self.origin = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        self.feed_url = self.origin + FEED_PATH
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    # ---- what it serves -----------------------------------------------------

    @property
    def tag(self) -> str:
        return f"v{self.version}"

    def add_asset(self, name: str, data: bytes | None = None,
                  path: str | None = None) -> None:
        """An installer (or any file) of the release: `data`, or the
        file at `path`, streamed when asked for."""
        self.assets[name] = _Asset(name, data, path)

    def add_checksums(self) -> None:
        """SHA256SUMS.txt over every asset so far, as the release job's
        `sha256sum -- *` writes it."""
        lines = "".join(f"{a.sha256}  {a.name}\n"
                        for a in sorted(self.assets.values(), key=lambda a: a.name)
                        if a.name != CHECKSUMS)
        self.add_asset(CHECKSUMS, lines.encode())

    def release(self) -> dict:
        assets = []
        for asset in self.assets.values():
            entry = {
                "name": asset.name,
                "browser_download_url": f"{self.origin}/download/{self.tag}/{asset.name}",
                "size": asset.size,
            }
            if self.with_digests:
                entry["digest"] = f"sha256:{asset.sha256}"
            assets.append(entry)
        return {
            "tag_name": self.tag,
            "name": f"GitBolt {self.version}",
            "draft": False,
            "prerelease": False,
            "html_url": f"https://github.com/{REPO}/releases/tag/{self.tag}",
            "body": self.notes,
            "assets": assets,
        }

    def hits(self, prefix: str) -> int:
        """Requests whose path starts with `prefix`."""
        with self._lock:
            return sum(1 for _, path in self.requests if path.startswith(prefix))

    def feed_hits(self) -> int:
        return self.hits(FEED_PATH)

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()

    # ---- answers ------------------------------------------------------------

    def _answer_feed(self, handler) -> None:
        if self.feed_delay:
            time.sleep(self.feed_delay)
        if self.feed_status != 200:
            body = json.dumps({"message": "Server Error"}).encode()
        else:
            body = json.dumps(self.release()).encode()
        handler.send_response(self.feed_status)
        handler.send_header("Content-Type", "application/json; charset=utf-8")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def _answer_download(self, handler) -> None:
        parts = handler.path.split("/")
        # /download/<tag>/<name>
        asset = self.assets.get(parts[-1]) if len(parts) == 4 and parts[2] == self.tag else None
        if asset is None:
            handler._send(404, b"not found", "text/plain")
            return
        handler.send_response(200)
        handler.send_header("Content-Type", "application/octet-stream")
        handler.send_header("Content-Length", str(asset.size))
        handler.end_headers()
        chunk, pause = self.throttle or (1 << 20, 0.0)
        try:
            if asset.data is not None:
                for start in range(0, asset.size, chunk):
                    handler.wfile.write(asset.data[start:start + chunk])
                    if pause:
                        time.sleep(pause)
            else:
                with open(asset.path, "rb") as fh:
                    for block in iter(lambda: fh.read(chunk), b""):
                        handler.wfile.write(block)
                        if pause:
                            time.sleep(pause)
        except (BrokenPipeError, ConnectionResetError):
            pass    # the app cancelled
