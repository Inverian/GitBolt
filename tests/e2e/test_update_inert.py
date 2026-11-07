#!/usr/bin/env python3
"""E2E: a test session never reaches the network for updates.

Every e2e launch is a test session (the bridge, an instance name, the
offscreen platform), which never uses the official feed, whatever the
build (src/ui/UpdateSetup.h). Three launches:

1. No GITBOLT_TEST_UPDATE_FEED: in-app updates are absent. dump-state's
   `updates` is "off:test-session"; there is no updateCheckTimer to
   fire; Settings' Updates page (GitBolt's seventh, row 0/6) stays
   hidden, and selecting its row shows no switch. Help › Check for
   Updates would open the releases page in the browser, so it isn't
   triggered here (tests/ui/TestUpdateController.cpp catches that URL).
2. A test feed that isn't plain http to 127.0.0.1: GitHub's own feed,
   then localhost. Refused ("off:test-feed-refused"), absent as above.
3. A loopback stub feed, with GITBOLT_UPDATE_CHECK=0: on against the
   stub ("test-feed"), yet nothing is scheduled: the timer exists but
   doesn't run, and the stub hears nothing while the app sits there.
   The Updates page is shown, its switch disabled by the variable.
"""

from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import App, Failure, run  # noqa: E402
from update_stub import UpdateFeed  # noqa: E402

UPDATES_ROW = "0/6"
SWITCH = "settings.check-for-updates"


def settings_switch(app: App) -> dict | None:
    """The Updates page's switch as list-widgets has it, after selecting
    the page's row in Settings; None when it isn't shown."""
    app.ok("trigger tools.settings")
    app.wait_until(lambda s: "Settings" in s.get("windows", []),
                   "Settings dialog visible", timeout=10)
    app.ok(f"select-row settings.nav {UPDATES_ROW}")
    found = [b for b in app.cmd("list-widgets")["buttons"]
             if b.get("objectName") == SWITCH]
    app.ok("click Cancel")
    app.wait_until(lambda s: "Settings" not in s.get("windows", []),
                   "Settings dialog closed")
    return found[0] if found else None


def expect_absent(app: App, gate: str) -> None:
    state = app.state()
    if state.get("updates") != gate:
        raise Failure(f"updates gate {state.get('updates')!r}, expected {gate!r}")
    if "updateTimerActive" in state:
        raise Failure("an update check timer exists with updates off")
    reply = app.cmd("fire-timer updateCheckTimer")
    if reply.get("ok"):
        raise Failure(f"fire-timer updateCheckTimer worked with updates off: {reply}")
    if settings_switch(app) is not None:
        raise Failure("Settings shows the Updates page with updates off")


def test(binary: str, scratch: str, apps: list) -> None:
    # 1. No test feed.
    app = App(binary, label="no feed")
    apps.append(app)
    app.wait_bridge()
    expect_absent(app, "off:test-session")
    app.quit()

    # 2. Refused feeds: GitHub's own (a test session never asks it), and
    #    a name rather than a loopback address.
    for feed in ("https://api.github.com/repos/Inverian/GitBolt/releases/latest",
                 "http://localhost:9/repos/Inverian/GitBolt/releases/latest"):
        app = App(binary, label=f"feed {feed}",
                  extra_env={"GITBOLT_TEST_UPDATE_FEED": feed,
                             "GITBOLT_TEST_APP_VERSION": "0.1.0"})
        apps.append(app)
        app.wait_bridge()
        expect_absent(app, "off:test-feed-refused")
        app.quit()
        if "GITBOLT_TEST_UPDATE_FEED must be" not in app.tail(40):
            raise Failure(f"no warning about the refused feed {feed}:{app.tail()}")

    # 3. A loopback feed: on, nothing scheduled.
    stub = UpdateFeed("0.1.1")
    stub.add_asset("GitBolt-0.1.1-Darwin-arm64.dmg", b"never downloaded")
    stub.add_checksums()
    try:
        app = App(binary, label="loopback feed",
                  extra_env={"GITBOLT_TEST_UPDATE_FEED": stub.feed_url,
                             "GITBOLT_TEST_APP_VERSION": "0.1.0",
                             "GITBOLT_UPDATE_CHECK": "0"})
        apps.append(app)
        app.wait_bridge()
        state = app.state()
        if state.get("updates") != "test-feed":
            raise Failure(f"updates gate {state.get('updates')!r} with a loopback feed")
        if state.get("updateTimerActive") is not False:
            raise Failure(f"the update check timer runs in a test session: {state}")
        switch = settings_switch(app)
        if switch is None:
            raise Failure("no Updates page with the test feed on")
        if switch.get("enabled", True):
            raise Failure("GITBOLT_UPDATE_CHECK=0 left the Updates switch enabled")
        time.sleep(3)
        if stub.requests:
            raise Failure(f"the app asked the stub by itself: {stub.requests}")
        if app.state().get("updateNotice"):
            raise Failure("an update notice without a check")
        rc = app.quit()
        if rc != 0:
            raise Failure(f"quit rc={rc}{app.tail()}")
    finally:
        stub.close()


if __name__ == "__main__":
    run(test)
