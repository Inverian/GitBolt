#!/usr/bin/env python3
"""E2E: the periodic background fetch runs off the GUI thread, quietly.

Plugins → Periodic background fetch used to run `git fetch` on the GUI
thread at every tick: the window froze for the whole fetch (minutes
against a stalled server), and quitting during one waited out git's
2-minute timeout. A tick is now a remote op like the toolbar Fetch, on
a pool thread that quit stops. It stays quiet: no "Fetching…"
narration, no disabled Fetch button, and a failure is a passing note,
not the sticky failure of something the user did. A user's Fetch
meanwhile shows the running one instead of being turned away.

The bridge's fire-timer stands in for the interval, which the UI only
takes in whole minutes. Remotes are a server that never answers
(SilentServer) and a path with no repository (fails at once).
"""

from __future__ import annotations

import os
import subprocess
import time

from e2elib import (AUTO_FETCH_TIMER, App, Failure, auto_fetch_config, git,
                    make_repo, run, stall_auto_fetch)


def status(state: dict) -> str:
    return state.get("statusMessage", "")


def wait_action_enabled(app: App, object_name: str,
                        timeout: float = 15.0) -> None:
    """Poll: an op's failure reaches the status bar a moment before its
    finished handler re-enables actions (both are queued)."""
    deadline = time.monotonic() + timeout
    while not app.action_enabled(object_name):
        if time.monotonic() > deadline:
            raise Failure(f"{object_name} stayed disabled")
        time.sleep(0.2)


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    git("remote", "add", "origin", os.path.join(scratch, "missing.git"),
        cwd=work)

    app = App(binary, work, config_home=auto_fetch_config(scratch))
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen"), "repo open")

    # 1. A stalled auto-fetch leaves the window live, and quiet.
    server = stall_auto_fetch(app, work)
    try:
        state = app.cmd("dump-state", timeout=5)
    except OSError:
        raise Failure("the window froze during the auto-fetch: no answer "
                      "to dump-state within 5 s") from None
    if status(state).startswith("Fetching"):
        raise Failure(f"the auto-fetch narrated itself: {status(state)!r}")
    if not app.action_enabled("act.fetch"):
        raise Failure("the auto-fetch disabled the Fetch button")

    # 2. A Fetch click meanwhile shows the running fetch; a Pull waits.
    app.ok("trigger act.fetch")
    app.wait_until(lambda s: status(s) == "Fetching from origin…",
                   "the auto-fetch shown as a fetch")
    if app.action_enabled("act.fetch"):
        raise Failure("Fetch still enabled while the shown fetch runs")
    app.ok("trigger act.pull")
    app.wait_until(
        lambda s: status(s) == "Another remote operation is still running…",
        "the pull turned away")

    # Shown, it reports like a toolbar Fetch: its failure sticks.
    server.hang_up()
    app.wait_until(lambda s: status(s).startswith("fetch failed:"),
                   "the shown fetch's failure")
    wait_action_enabled(app, "act.fetch")

    # 3. A failing auto-fetch is a passing note, not the sticky failure.
    git("remote", "set-url", "origin", os.path.join(scratch, "missing.git"),
        cwd=work)
    app.ok(f"fire-timer {AUTO_FETCH_TIMER}")
    app.wait_until(lambda s: status(s).startswith("Auto-fetch failed:"),
                   "the auto-fetch failure note")

    # 4. Quitting during an auto-fetch stops git and exits at once.
    server = stall_auto_fetch(app, work)
    try:
        rc = app.quit(timeout=15)
    except subprocess.TimeoutExpired:
        raise Failure("GitBolt was still running 15 s after quit: it "
                      "waited for the stalled auto-fetch") from None
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc} after quitting during "
                      "an auto-fetch")
    if not server.closed.wait(5):
        raise Failure("git still held the connection after GitBolt quit")


if __name__ == "__main__":
    run(test)
