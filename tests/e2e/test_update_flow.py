#!/usr/bin/env python3
"""E2E: the update check against a stub feed, through the real app.

The app runs as version 0.1.0 (GITBOLT_TEST_APP_VERSION) with its feed on
a loopback stub (update_stub.py) offering v0.1.1. The build-tree app is
no release package, so it can't install it (update/Package.h: Unknown).

1. Nothing asks by itself: the daily check isn't scheduled in a test
   session. Fired through the bridge (fire-timer updateCheckTimer), it
   asks once and the notice bar says "GitBolt 0.1.1 is available.";
   fired again, it doesn't ask (one check a day). × dismisses the bar.
2. Help › Check for Updates asks again (a manual check always does) and
   the dialog shows the release, why this copy can't install it, and
   Open Release Page, Skip This Version and Later, no Install and
   Restart.
3. Skip This Version closes it and is saved, with the last check, in
   GitBolt.ini.
"""

from __future__ import annotations

import configparser
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import App, Failure, run  # noqa: E402
from update_stub import UpdateFeed  # noqa: E402

TIMER = "updateCheckTimer"
NOTICE = "GitBolt 0.1.1 is available."


def test(binary: str, scratch: str, apps: list) -> None:
    stub = UpdateFeed("0.1.1")
    for name in ("GitBolt-0.1.1-Darwin-arm64.dmg", "GitBolt-0.1.1-Windows-AMD64.exe",
                 "gitbolt_0.1.1_amd64.deb", "GitBolt-0.1.1-x86_64.AppImage"):
        stub.add_asset(name, f"not really {name}".encode())
    stub.add_checksums()
    config_home = os.path.join(scratch, "config")
    try:
        app = App(binary, config_home=config_home,
                  extra_env={"GITBOLT_TEST_UPDATE_FEED": stub.feed_url,
                             "GITBOLT_TEST_APP_VERSION": "0.1.0"})
        apps.append(app)
        app.wait_bridge()
        state = app.state()
        if state.get("updates") != "test-feed":
            raise Failure(f"updates gate {state.get('updates')!r}, expected 'test-feed'")
        if state.get("updateTimerActive") is not False:
            raise Failure("the daily check is scheduled in a test session")

        # 1. The automatic check, fired: the bar, no dialog.
        app.ok(f"fire-timer {TIMER}")
        state = app.wait_until(lambda s: s.get("updateNotice") == NOTICE,
                               "the update notice bar")
        if "updateDialog" in state:
            raise Failure(f"an automatic find opened the dialog: {state['updateDialog']}")
        if stub.feed_hits() != 1:
            raise Failure(f"{stub.feed_hits()} feed requests for one check")
        app.ok(f"fire-timer {TIMER}")
        time.sleep(1.5)
        if stub.feed_hits() != 1:
            raise Failure("the automatic check asked again within the day")
        app.ok("click updateNotice.dismiss")
        app.wait_until(lambda s: "updateNotice" not in s, "the bar dismissed")

        # 2. A manual check asks again, and says why this copy can't
        #    install.
        app.ok("trigger help.check-for-updates")
        state = app.wait_until(lambda s: s.get("updateDialog") == "not-installable",
                               "the dialog's not-installable state")
        text = state.get("updateDialogText", "")
        for expected in ("GitBolt 0.1.1 is available", "You have 0.1.0.",
                         "wasn't installed from a GitBolt release package"):
            if expected not in text:
                raise Failure(f"the dialog doesn't say {expected!r}: {text!r}")
        if stub.feed_hits() != 2:
            raise Failure(f"the manual check made {stub.feed_hits() - 1} requests, not 1")
        buttons = {b.get("objectName") for b in app.cmd("list-widgets")["buttons"]
                   if b.get("objectName", "").startswith("updateDialog.")}
        expected = {"updateDialog.openReleasePage", "updateDialog.skip", "updateDialog.later"}
        if buttons != expected:
            raise Failure(f"the dialog's buttons are {sorted(buttons)}, expected {sorted(expected)}")
        if stub.hits("/download/"):
            raise Failure("something was downloaded")

        # 3. Skip This Version.
        app.ok("click updateDialog.skip")
        app.wait_until(lambda s: "updateDialog" not in s, "the dialog closed")
        rc = app.quit()
        if rc != 0:
            raise Failure(f"quit rc={rc}{app.tail()}")

        ini = configparser.ConfigParser(interpolation=None)
        ini.read(os.path.join(config_home, "GitBolt", "GitBolt.ini"))
        if not ini.has_section("updates"):
            raise Failure("no [updates] section in GitBolt.ini")
        if ini.get("updates", "skippedVersion", fallback=None) != "0.1.1":
            raise Failure(f"skippedVersion is {ini.get('updates', 'skippedVersion', fallback=None)!r}")
        last = ini.get("updates", "lastCheck", fallback="")
        if not last.endswith("Z"):
            raise Failure(f"lastCheck {last!r} isn't a UTC time")
    finally:
        stub.close()


if __name__ == "__main__":
    run(test)
