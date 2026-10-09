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

from e2elib import App, Failure, SilentServer, git, make_repo, run

TIMER = "periodicFetchTimer"


def action_enabled(app: App, object_name: str) -> bool:
    for a in app.cmd("list-actions")["actions"]:
        if a.get("objectName") == object_name:
            return a["enabled"]
    raise Failure(f"no action named {object_name}")


def status(state: dict) -> str:
    return state.get("statusMessage", "")


def wait_action_enabled(app: App, object_name: str,
                        timeout: float = 15.0) -> None:
    """Poll: an op's failure reaches the status bar a moment before its
    finished handler re-enables actions (both are queued)."""
    deadline = time.monotonic() + timeout
    while not action_enabled(app, object_name):
        if time.monotonic() > deadline:
            raise Failure(f"{object_name} stayed disabled")
        time.sleep(0.2)


def stall_on(app: App, work: str) -> SilentServer:
    """Point origin at a fresh silent server and fire an auto-fetch at
    it; returns once git is connected and waiting. Fires again while
    ticks are skipped: the last auto-fetch's finished handler may still
    be queued behind the failure note the test waited for."""
    server = SilentServer()
    git("remote", "set-url", "origin", server.url, cwd=work)
    deadline = time.monotonic() + 20
    while not server.connected.is_set():
        if time.monotonic() > deadline:
            raise Failure("the auto-fetch never reached the server")
        app.ok(f"fire-timer {TIMER}")
        server.connected.wait(1)
    return server


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    git("remote", "add", "origin", os.path.join(scratch, "missing.git"),
        cwd=work)

    # Periodic fetch on, as the Plugins menu saves it. The hour-long
    # interval keeps the real timer out of the way.
    config_home = os.path.join(scratch, "config")
    ini_dir = os.path.join(config_home, "GitBolt")
    os.makedirs(ini_dir)
    with open(os.path.join(ini_dir, "GitBolt.ini"), "w") as fh:
        fh.write("[plugins]\n"
                 "periodicFetch\\enabled=true\n"
                 "periodicFetch\\intervalMinutes=60\n")

    app = App(binary, work, config_home=config_home)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen"), "repo open")

    # 1. A stalled auto-fetch leaves the window live, and quiet.
    server = stall_on(app, work)
    try:
        state = app.cmd("dump-state", timeout=5)
    except OSError:
        raise Failure("the window froze during the auto-fetch: no answer "
                      "to dump-state within 5 s") from None
    if status(state).startswith("Fetching"):
        raise Failure(f"the auto-fetch narrated itself: {status(state)!r}")
    if not action_enabled(app, "act.fetch"):
        raise Failure("the auto-fetch disabled the Fetch button")

    # 2. A Fetch click meanwhile shows the running fetch; a Pull waits.
    app.ok("trigger act.fetch")
    app.wait_until(lambda s: status(s) == "Fetching from origin…",
                   "the auto-fetch shown as a fetch")
    if action_enabled(app, "act.fetch"):
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
    app.ok(f"fire-timer {TIMER}")
    app.wait_until(lambda s: status(s).startswith("Auto-fetch failed:"),
                   "the auto-fetch failure note")

    # 4. Quitting during an auto-fetch stops git and exits at once.
    server = stall_on(app, work)
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
