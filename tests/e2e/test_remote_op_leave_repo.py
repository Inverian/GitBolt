#!/usr/bin/env python3
"""E2E: a remote op whose repository leaves the screen stays off it.

Close, or opening another repository, takes the repository off the
screen, but GitService keeps it open until another one replaces it: a
Fetch enabled on the home screen, or while the next repository loads,
fetches the one left behind. Two kinds of remote op outlived the switch
and did just that when they finished:

- an auto-fetch a Fetch click had revealed: it re-enabled Fetch and
  reported like a toolbar Fetch, so its "fetch failed: …" stuck on the
  home screen or over the next repository. It is now cancelled, and
  says so, once its repository is closed or replaced; but not when an
  open begins, as an open that fails, or that opens the same
  repository again, leaves it on screen;
- a toolbar Fetch, Pull or Push: it re-enabled its own action. It still
  does wherever the repository actions are on, as File > Home leaves
  them (the repository stays open there).

Each op here waits on a server that never answers (SilentServer) until
the test hangs up, which fails the fetch at a moment the test picks.
"""

from __future__ import annotations

import os

from e2elib import (App, Failure, SilentServer, auto_fetch_config,
                    check_for, git, home_screen, make_repo, on_screen, run,
                    stall_auto_fetch)

SHOWN = "Fetching from origin…"
CANCELLED = "Fetch cancelled."


def status(app: App) -> str:
    return app.state().get("statusMessage", "")


def warning_up(app: App) -> bool:
    """A message box is open (untitled on macOS, so not by its title)."""
    return any(w["class"] == "QMessageBox"
               for w in app.cmd("list-widgets")["windows"])


def reopen(app: App, work: str) -> None:
    name = os.path.basename(work)
    handoff = app.launch_second(work)
    if handoff.returncode != 0:
        raise Failure(f"hand-off of {name} failed: rc={handoff.returncode}")
    app.wait_until(on_screen(name), f"{name} on screen")
    app.wait_until(lambda s: app.action_enabled("act.fetch"),
                   f"Fetch enabled on {name}")


def reveal_auto_fetch(app: App, work: str) -> SilentServer:
    """A stalled auto-fetch, revealed by a Fetch click."""
    server = stall_auto_fetch(app, work)
    app.ok("trigger act.fetch")
    app.wait_until(lambda s: s.get("statusMessage") == SHOWN,
                   "the auto-fetch shown as a fetch")
    return server


def toolbar_fetch(app: App, work: str) -> SilentServer:
    """A toolbar Fetch, stalled."""
    server = SilentServer()
    git("remote", "set-url", "origin", server.url, cwd=work)
    app.ok("trigger act.fetch")
    if not server.connected.wait(20):
        raise Failure("the toolbar Fetch never reached the server")
    return server


def let_it_end(server: SilentServer) -> None:
    """The op at `server` ends: stopped already, or failing as the
    server hangs up."""
    if not server.closed.wait(2):
        server.hang_up()
    if not server.closed.wait(10):
        raise Failure("git never let go of the connection")


def fails_and_gives_fetch_back(app: App, server: SilentServer,
                               what: str) -> None:
    """The fetch at `server` is still the user's: it is still running,
    with Fetch off, until the server hangs up; then its failure is
    reported and Fetch is back on."""
    if server.closed.wait(1):
        raise Failure(f"{what}: the fetch was stopped")
    if app.action_enabled("act.fetch"):
        raise Failure(f"{what}: Fetch enabled while the fetch runs")
    server.hang_up()
    app.wait_until(lambda s: s.get("statusMessage", "")
                   .startswith("fetch failed"),
                   f"{what}: the fetch's failure")
    app.wait_until(lambda s: app.action_enabled("act.fetch"),
                   f"{what}: Fetch enabled once the fetch ended")


def test(binary: str, scratch: str, apps: list) -> None:
    work_a = make_repo(os.path.join(scratch, "work-a"))
    git("remote", "add", "origin", os.path.join(scratch, "missing.git"),
        cwd=work_a)
    work_b = make_repo(os.path.join(scratch, "work-b"))
    not_a_repo = os.path.join(scratch, "not-a-repo")
    os.makedirs(not_a_repo)

    app = App(binary, work_a, config_home=auto_fetch_config(scratch))
    apps.append(app)
    app.wait_bridge()
    app.wait_until(on_screen("work-a"), "work-a on screen")

    # 1. A revealed auto-fetch, then Close: it is cancelled, saying so,
    # and nothing else of it reaches the home screen.
    server = reveal_auto_fetch(app, work_a)
    app.ok("trigger repository.close")
    app.wait_until(home_screen, "the home screen")
    app.wait_until(lambda s: s.get("statusMessage") == CANCELLED,
                   "the fetch's cancellation note")
    if not server.closed.wait(5):
        raise Failure("the revealed fetch ran on after Close")

    def home_screen_problem() -> str:
        if app.action_enabled("act.fetch"):
            return "Fetch enabled on the home screen"
        msg = status(app)
        if msg.startswith("fetch failed") or msg in (SHOWN, "Fetch complete."):
            return f"the fetch reported there: {msg!r}"
        return ""
    check_for(2, "revealed auto-fetch, then Close", home_screen_problem)

    # 2. A toolbar Fetch, then Close. Its outcome is still the user's
    # to see; but Fetch must stay off with no repository on screen.
    reopen(app, work_a)
    server = toolbar_fetch(app, work_a)
    app.ok("trigger repository.close")
    app.wait_until(home_screen, "the home screen")
    let_it_end(server)
    check_for(2, "toolbar Fetch, then Close",
              lambda: app.action_enabled("act.fetch")
              and "Fetch enabled on the home screen")

    # 3. A toolbar Fetch, then File > Home: the repository stays open,
    # and its actions on, so Fetch comes back when the fetch ends.
    reopen(app, work_a)
    server = toolbar_fetch(app, work_a)
    app.ok("trigger file.home")
    app.wait_until(
        lambda s: any(b["text"] == "Clear Recent"
                      for b in app.cmd("list-widgets")["buttons"]),
        "the dashboard")
    fails_and_gives_fetch_back(app, server, "toolbar Fetch, then Home")

    # 4. A revealed auto-fetch, then another repository: it is
    # cancelled once that one is open, and work-a's outcome must not
    # land on work-b's screen.
    reopen(app, work_a)
    server = reveal_auto_fetch(app, work_a)
    reopen(app, work_b)
    app.wait_until(lambda s: s.get("statusMessage") == CANCELLED,
                   "the fetch's cancellation note on work-b")
    if not server.closed.wait(5):
        raise Failure("work-a's revealed fetch ran on after work-b opened")
    check_for(2, "revealed auto-fetch, then work-b",
              lambda: status(app).startswith("fetch failed")
              and f"work-a's fetch reported on work-b: {status(app)!r}")

    # 5. A revealed auto-fetch, then its own repository opened again:
    # the fetch is still the one on screen, so it runs on.
    reopen(app, work_a)
    server = reveal_auto_fetch(app, work_a)
    handoff = app.launch_second(work_a)
    if handoff.returncode != 0:
        raise Failure(f"hand-off of work-a failed: rc={handoff.returncode}")
    state = app.wait_until(
        lambda s: s.get("statusMessage", "").startswith("Opened:")
        or s.get("statusMessage") == CANCELLED,
        "work-a opened again")
    if state["statusMessage"] == CANCELLED:
        raise Failure("opening work-a again cancelled its own fetch")
    fails_and_gives_fetch_back(app, server, "work-a opened again")

    # 6. A revealed auto-fetch, then an open that fails: work-a stays,
    # and so does its fetch.
    server = reveal_auto_fetch(app, work_a)
    handoff = app.launch_second(not_a_repo)
    if handoff.returncode != 0:
        raise Failure(f"hand-off of not-a-repo failed: "
                      f"rc={handoff.returncode}")
    app.wait_until(lambda s: warning_up(app), "the failed open's warning")
    app.ok("click OK")
    app.wait_until(lambda s: not warning_up(app) and on_screen("work-a")(s),
                   "work-a back on screen")
    fails_and_gives_fetch_back(app, server, "an open that failed")

    rc = app.quit(timeout=15)
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc}")


if __name__ == "__main__":
    run(test)
