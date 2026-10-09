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
import subprocess

from e2elib import App, Failure, SilentServer, git, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    server = SilentServer()
    work = make_repo(os.path.join(scratch, "work"))
    git("remote", "add", "origin", server.url, cwd=work)

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
