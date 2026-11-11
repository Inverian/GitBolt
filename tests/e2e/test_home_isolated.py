#!/usr/bin/env python3
"""E2E: an app the harness launches has a HOME of its own.

e2elib isolated each app's runtime and config dirs but passed the
test's HOME through, so a crash in any e2e test truncated the real
~/.gitbolt/crash.log (losing a report the user hadn't sent yet, and
showing them Crash Detected at their next launch), and the app's git
read the developer's ~/.gitconfig. The crash handler creates
~/.gitbolt at startup, which shows which home the app ran with.

The log file likewise: a test session writes GitBolt-test.log, in the
app's own HOME (macOS) or XDG_STATE_HOME (Linux), never the user's
GitBolt.log or anything under the test's HOME.
"""

from __future__ import annotations

import os
import sys

from e2elib import App, Failure, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    repo = make_repo(os.path.join(scratch, "repo"))
    # Stands in for the user's real home: what the test process (and
    # so far every app it launched) has as HOME.
    user_home = os.path.join(scratch, "user-home")
    os.makedirs(user_home)
    home = os.environ.get("HOME")
    os.environ["HOME"] = user_home
    try:
        app = App(binary, repo)
    finally:
        if home is None:
            del os.environ["HOME"]
        else:
            os.environ["HOME"] = home
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen"), "repo open")

    app_home = app.env.get("HOME")
    if not app_home or os.path.realpath(app_home) == os.path.realpath(user_home):
        raise Failure(f"the app inherited the test's HOME ({app_home})")
    if os.path.exists(os.path.join(user_home, ".gitbolt")):
        raise Failure("the app created ~/.gitbolt in the test's HOME: a "
                      "crash would overwrite the user's crash.log")
    if not os.path.isdir(os.path.join(app_home, ".gitbolt")):
        raise Failure(f"no .gitbolt in the app's HOME ({app_home}): the "
                      "app didn't run with it, so this proves nothing")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"quit rc={rc}{app.tail()}")

    log_dir = (os.path.join(app_home, "Library", "Logs", "GitBolt")
               if sys.platform == "darwin"
               else os.path.join(app.env["XDG_STATE_HOME"], "GitBolt"))
    log = os.path.join(log_dir, "GitBolt-test.log")
    if not os.path.isfile(log):
        raise Failure(f"no {log}{app.tail()}")
    with open(log, encoding="utf-8") as fh:
        text = fh.read()
    if " started: " not in text or "test session" not in text:
        raise Failure(f"no session line in {log}:\n{text}")
    if os.path.exists(os.path.join(log_dir, "GitBolt.log")):
        raise Failure("a test session wrote the user's GitBolt.log")
    for leak in (("Library", "Logs", "GitBolt"), (".local", "state", "GitBolt")):
        if os.path.exists(os.path.join(user_home, *leak)):
            raise Failure(f"the app wrote {os.path.join(*leak)} in the test's HOME")


if __name__ == "__main__":
    if sys.platform == "win32":
        print("SKIP: on Windows QDir::homePath() is the profile folder whatever "
              "HOME says, so ~/.gitbolt can't move (a test session writes "
              "crash-test.log there, never crash.log), and the log goes to the "
              "real %LOCALAPPDATA%")
        sys.exit(77)
    run(test)
