#!/usr/bin/env python3
"""E2E: an auto-fetch nobody wants any more stops, and holds nothing up.

An auto-fetch is disowned when its result stops being wanted: another
repository is opened, or Plugins → Periodic background fetch is
switched off. It used to run on all the same, against a stalled server
until git's 2-minute timeout, and every Fetch, Pull and Push meanwhile
was turned away as "Another remote operation is still running…", with
nothing on screen to say which. Disowning it now stops its git, and a
remote op clicked while it winds down is held for that moment, then
starts; unless the repository is closed first, which drops it.

That moment lasts a poll of git's process or so. A click sent on its
own may come after it, and then nothing is held, so the tests queue the
click behind the switch-off in one bridge write (App.batch): it always
lands while the stopped fetch winds down.

The stalled auto-fetches talk to a server that never answers
(SilentServer); the user's fetches then go to a real (bare) remote, so
they complete.
"""

from __future__ import annotations

import os
import time

from e2elib import (App, Failure, SilentServer, auto_fetch_config,
                    check_for, git, home_screen, make_repo, on_screen, run,
                    stall_auto_fetch)

TURNED_AWAY = "Another remote operation is still running…"
SWITCH = "trigger plugins.periodic-background-fetch"


def bare_remote(work: str, scratch: str, name: str) -> str:
    bare = os.path.join(scratch, name)
    git("clone", "-q", "--bare", work, bare, cwd=scratch)
    return bare


def wait_fetch_completes(app: App, what: str, timeout: float = 15.0) -> None:
    """Poll until the user's Fetch reports success; fail if the click was
    turned away instead (that note stays up for 3 s, so polling sees it)."""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        last = app.state().get("statusMessage", "")
        if last == TURNED_AWAY:
            raise Failure(f"{what}: the Fetch was turned away while the "
                          "disowned auto-fetch ran on")
        if last == "Fetch complete.":
            return
        time.sleep(0.2)
    raise Failure(f"{what}: the Fetch never completed; status {last!r}")


def periodic_fetch_on(app: App) -> None:
    """Switch periodic fetch back on; its interval dialog's OK keeps the
    saved hour."""
    app.ok(SWITCH)
    app.wait_until(lambda s: "Periodic Fetch" in s["windows"],
                   "the interval dialog")
    app.ok("click OK")
    app.wait_until(lambda s: "Periodic Fetch" not in s["windows"],
                   "the interval dialog to close")


def test(binary: str, scratch: str, apps: list) -> None:
    work_a = make_repo(os.path.join(scratch, "work-a"))
    git("remote", "add", "origin", os.path.join(scratch, "missing.git"),
        cwd=work_a)
    work_b = make_repo(os.path.join(scratch, "work-b"))
    upstream_b = bare_remote(work_b, scratch, "upstream-b.git")
    git("remote", "add", "origin", upstream_b, cwd=work_b)

    app = App(binary, work_a, config_home=auto_fetch_config(scratch))
    apps.append(app)
    app.wait_bridge()
    app.wait_until(on_screen("work-a"), "work-a open")

    # 1. Another repository opened during an auto-fetch: the auto-fetch
    # of work-a stops, and work-b's Fetch runs at once.
    server = stall_auto_fetch(app, work_a)
    second = app.launch_second(work_b)
    if second.returncode != 0:
        raise Failure(f"hand-off of work-b failed: rc={second.returncode}")
    app.wait_until(on_screen("work-b"), "work-b open")
    app.wait_until(lambda s: app.action_enabled("act.fetch"),
                   "Fetch enabled on work-b")
    app.ok("trigger act.fetch")
    wait_fetch_completes(app, "after opening work-b")
    if not server.closed.wait(5):
        raise Failure("work-a's disowned auto-fetch still held its "
                      "connection after work-b was opened")

    # 2. Periodic fetch switched off during an auto-fetch, and a Fetch
    # clicked while the stopped one winds down: the Fetch is held for
    # it, not turned away, and runs once it has ended. origin moves
    # back to the real remote once the auto-fetch is connected, so the
    # user's fetch goes there.
    app.wait_until(lambda s: s.get("statusMessage") != "Fetch complete.",
                   "the first Fetch's note to clear")
    server = stall_auto_fetch(app, work_b)
    git("remote", "set-url", "origin", upstream_b, cwd=work_b)
    if server.closed.is_set():
        raise Failure("the auto-fetch ended before it was switched off")
    app.batch(SWITCH, "trigger act.fetch")
    wait_fetch_completes(app, "after switching periodic fetch off")
    if not server.closed.wait(5):
        raise Failure("the auto-fetch still held its connection after "
                      "periodic fetch was switched off")

    # 3. The same, with the repository closed right after the click:
    # the held Fetch goes with it. Fetch stays off on the home screen,
    # nothing is reported there, and the server the held Fetch would
    # have fetched from never hears from git.
    periodic_fetch_on(app)
    server = stall_auto_fetch(app, work_b)
    held_for = SilentServer()
    git("remote", "set-url", "origin", held_for.url, cwd=work_b)
    app.batch(SWITCH, "trigger act.fetch", "trigger repository.close")
    app.wait_until(home_screen, "the home screen")
    if not server.closed.wait(5):
        raise Failure("the auto-fetch still held its connection after "
                      "periodic fetch was switched off")

    def home_screen_problem() -> str:
        if app.action_enabled("act.fetch"):
            return "Fetch enabled on the home screen"
        msg = app.state().get("statusMessage", "")
        if msg.lower().startswith("fetch") or msg == TURNED_AWAY:
            return f"the status bar says {msg!r}"
        if held_for.connected.is_set():
            return "the held Fetch ran after the repository was closed"
        return ""
    check_for(3, "a held Fetch, then Close", home_screen_problem)

    rc = app.quit(timeout=15)
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc}")


if __name__ == "__main__":
    run(test)
