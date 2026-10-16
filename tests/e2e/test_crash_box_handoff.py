#!/usr/bin/env python3
"""E2E: a launch while Crash Detected is up is handed over, not dropped.

The box a crash leaves for the next launch used to come up before the
window and the hand-off listener. That instance held the single-
instance lock but answered nobody, so a second launch ("Open in
GitBolt" from Finder, say) waited its 10 s, said "not responding" and
dropped its path. The box now comes up over the window once the
listener is up, and a hand-off while it is up is answered at once and
acted on: the newer path wins over the first launch's own. OK then
clears the report. And as the box now comes up over the window, the
window can be closed under it: dismissing the box then ends the app.

A test session never shows the box (~/.gitbolt/crash.log is the
user's), so this one opts in with GITBOLT_TEST_CRASH_BOX=1, in a HOME
of its own with a crash.log planted in it.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time

import e2elib
from e2elib import App, Failure, check_for, make_repo, run

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       os.pardir, "fixtures", "crash", "macos.log")


def _on(name: str):
    return lambda s: s.get("repoPath", "").rstrip("/").endswith(name)


def _crash_box(app: App) -> bool:
    """Crash Detected is up. By its name: macOS message boxes have no
    title."""
    return any(w.get("objectName") == "app.crashDetected"
               for w in app.cmd("list-widgets")["windows"])


def _wait_for(app: App, pred, what: str, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while not pred(app):
        if time.monotonic() > deadline:
            raise Failure(f"{app.label}: timed out waiting for {what}")
        time.sleep(0.3)


def test(binary: str, scratch: str, apps: list) -> None:
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))
    home = os.path.join(scratch, "home")
    crash_log = os.path.join(home, ".gitbolt", "crash.log")
    os.makedirs(os.path.dirname(crash_log))
    shutil.copy(FIXTURE, crash_log)

    # e2elib builds each app's environment; the apps here get that home
    # and the opt-in, whatever HOME e2elib would give them.
    app_env = e2elib.app_env

    def crash_env(*args, **kwargs) -> dict:
        env = app_env(*args, **kwargs)
        env["HOME"] = home
        env["GITBOLT_TEST_CRASH_BOX"] = "1"
        return env

    def launch(label: str) -> App:
        e2elib.app_env = crash_env
        try:
            app = App(binary, repo_a, label=label)
        finally:
            e2elib.app_env = app_env
        apps.append(app)
        return app

    first = launch("instance showing Crash Detected")
    # The bridge answers while the box is up: it was created first.
    first.wait_bridge()
    _wait_for(first, _crash_box, "Crash Detected up")

    started = time.monotonic()
    second = first.launch_second(repo_b)
    took = time.monotonic() - started
    # Well under the 10 s after which a launch gives up on an owner that
    # doesn't listen.
    if second.returncode != 0 or took > 8:
        raise Failure("a launch while Crash Detected is up should be "
                      f"handed over at once, got rc={second.returncode} "
                      f"after {took:.1f} s: {second.stderr[-400:]}")
    # Opened behind the box, which stays up until the user is done
    # with it.
    first.wait_until(_on("repo-b"), "repo B open behind the box")
    if not _crash_box(first):
        raise Failure("the hand-off took Crash Detected down")
    if not os.path.exists(crash_log):
        raise Failure("crash.log went before the box was dismissed")

    first.ok("click OK")
    _wait_for(first, lambda app: not _crash_box(app),
              "Crash Detected dismissed")
    # The first launch's own repo A was opened first, so B stays.
    check_for(2, "after OK", lambda: None if _on("repo-b")(first.state())
              else f"repo B gave way: {first.state().get('repoPath')}")
    if os.path.exists(crash_log):
        raise Failure("OK left crash.log behind")

    rc = first.quit()
    if rc != 0:
        raise Failure(f"quit rc={rc}{first.tail()}")

    # The window closed while the box is up (Cmd+Q, the bridge's quit):
    # the box stays for the user, and dismissing it ends the app. The
    # last window closing quits only an app in its main event loop, which
    # this one isn't in yet: unless main() sees to it, the app runs on
    # with no window.
    shutil.copy(FIXTURE, crash_log)
    closing = launch("instance closed under Crash Detected")
    closing.wait_bridge()
    _wait_for(closing, _crash_box, "Crash Detected up")
    closing.ok("quit")
    _wait_for(closing, lambda app: [
        w.get("objectName") for w in app.cmd("list-widgets")["windows"]]
        == ["app.crashDetected"], "the window closed, the box still up")
    closing.ok("click OK")
    try:
        rc = closing.proc.wait(15)
    except subprocess.TimeoutExpired:
        raise Failure("dismissing Crash Detected after the window closed "
                      "left the app running") from None
    if rc != 0:
        raise Failure(f"rc={rc} after the window closed{closing.tail()}")


if __name__ == "__main__":
    run(test)
