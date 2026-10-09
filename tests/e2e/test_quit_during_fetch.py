#!/usr/bin/env python3
"""E2E: quitting while a fetch waits on a server that never answers.

Fetch, pull and push run on a pool thread, inside the window's
GitService. Quitting mid-op used to tear the window and its service
down under the running job: GitBolt lingered, windowless, until git's
2-minute timeout, and the job then returned into the freed service.
Quit now stops git and its process tree and exits at once.

The "server" accepts git:// connections and never says a word, so the
fetch is stuck in git's first read for as long as the test likes.
"""

from __future__ import annotations

import os
import socket
import subprocess
import threading

from e2elib import App, Failure, git, make_repo, run


class SilentServer:
    """Listens on 127.0.0.1, accepts one connection and never answers.
    `closed` is set once the peer has hung up: every process holding
    the connection is gone."""

    def __init__(self) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(1)
        self.port = self.sock.getsockname()[1]
        self.connected = threading.Event()
        self.closed = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        conn, _ = self.sock.accept()
        self.connected.set()
        with conn:
            while conn.recv(4096):      # git's request, never answered
                pass
        self.closed.set()


def test(binary: str, scratch: str, apps: list) -> None:
    server = SilentServer()
    work = make_repo(os.path.join(scratch, "work"))
    git("remote", "add", "origin",
        f"git://127.0.0.1:{server.port}/repo.git", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen"), "repo open")

    app.ok("trigger act.fetch")
    if not server.connected.wait(20):
        raise Failure("the fetch never reached the server")

    try:
        rc = app.quit(timeout=15)
    except subprocess.TimeoutExpired:
        raise Failure("GitBolt was still running 15 s after quit: it "
                      "waited for the stalled fetch") from None
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc} after quitting during "
                      "a fetch")
    if not server.closed.wait(5):
        raise Failure("git still held the connection after GitBolt quit")


if __name__ == "__main__":
    run(test)
