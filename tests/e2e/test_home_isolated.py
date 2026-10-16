#!/usr/bin/env python3
"""E2E: an app the harness launches has a HOME of its own.

e2elib isolated each app's runtime and config dirs but passed the
test's HOME through, so a crash in any e2e test truncated the real
~/.gitbolt/crash.log (losing a report the user hadn't sent yet, and
showing them Crash Detected at their next launch), and the app's git
read the developer's ~/.gitconfig. The crash handler creates
~/.gitbolt at startup, which shows which home the app ran with.
"""

from __future__ import annotations

import os

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


if __name__ == "__main__":
    run(test)
