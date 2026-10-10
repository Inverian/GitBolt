#!/usr/bin/env python3
"""E2E: Tools > Settings after a repository opens.

The Settings shortcut list comes from the menu actions MainWindow
collected at startup, File > Recent Repositories' entries among them.
Opening a repository rebuilds that submenu, deleting those entries, and
Settings then read the deleted actions: on macOS it crashed every time.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    # A fresh config: Recent holds its "(No recent repositories)"
    # placeholder at startup, and opening the repository replaces it.
    repo = make_repo(os.path.join(scratch, "repo"))
    # A crash writes ~/.gitbolt/crash.log, and every later launch opens
    # with a "Crash Detected" box: keep a failure out of the real home.
    home = os.environ.get("HOME")
    os.environ["HOME"] = os.path.join(scratch, "home")
    try:
        app = App(binary, repo, config_home=os.path.join(scratch, "config"))
    finally:
        if home is None:
            del os.environ["HOME"]
        else:
            os.environ["HOME"] = home
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen"), "repo open")
    recent = [a for a in app.cmd("list-actions")["actions"]
              if a.get("objectName", "").startswith("file.recent.")]
    if not recent:
        raise Failure("opening the repository didn't rebuild File > "
                      "Recent Repositories; the test proves nothing")

    for attempt in (1, 2):
        app.ok("trigger tools.settings")
        try:
            app.wait_until(lambda s: "Settings" in s.get("windows", []),
                           "Settings dialog visible", timeout=10)
        except (Failure, OSError, ValueError) as exc:
            if app.proc.poll() is not None:
                raise Failure(f"Settings (open #{attempt}) crashed GitBolt "
                              f"(rc={app.proc.returncode}){app.tail()}") \
                    from None
            raise exc
        app.ok("click Cancel")
        app.wait_until(lambda s: "Settings" not in s.get("windows", []),
                       "Settings dialog closed")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"quit rc={rc}{app.tail()}")


if __name__ == "__main__":
    run(test)
