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
4. Downloads go in a folder of the session's own in its TMPDIR
   (gitbolt-updates-XXXXXX, 0700), which dump-state's updateDir names.
   A copy started with GITBOLT_TEST_UPDATE_DIR set to it (as the install
   helper relaunches one) goes on using it, and reads the helper's
   install-result there once: a failure opens the dialog on why, with
   install.log's path, and Try Again checks again; ok says "GitBolt was
   updated to <v>." in the status bar. Either way the file is gone
   afterwards.
"""

from __future__ import annotations

import configparser
import os
import stat
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
                 "gitbolt_0.1.1_amd64.deb"):
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
        update_dir = state.get("updateDir", "")
        check_private_dir(update_dir, app.runtime_dir)

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

        # 4. What the install helper leaves for the copy it relaunches.
        result = os.path.join(update_dir, "install-result")
        log = os.path.join(update_dir, "install.log")
        with open(result, "w") as fh:
            fh.write("failed 0.1.1 the installer exited with code 2\n")
        with open(log, "w") as fh:
            fh.write("".join(f"10:00:{i:02} step {i}\n" for i in range(1, 16)))
        relaunched = relaunch(binary, app, stub, update_dir, apps, "failed install")
        state = relaunched.wait_until(lambda s: s.get("updateDialog") == "failed",
                                      "the dialog on the failed install")
        text = state.get("updateDialogText", "")
        for expected in ("Couldn't install GitBolt 0.1.1",
                         "Installing GitBolt 0.1.1 failed after GitBolt quit: "
                         "the installer exited with code 2.",
                         log):
            if expected not in text:
                raise Failure(f"the dialog doesn't say {expected!r}: {text!r}")
        if os.path.exists(result):
            raise Failure("install-result is still there after it was read")
        hits = stub.feed_hits()
        relaunched.ok("click updateDialog.retry")
        relaunched.wait_until(lambda s: s.get("updateDialog") == "not-installable",
                              "Try Again's check")
        if stub.feed_hits() != hits + 1:
            raise Failure("Try Again didn't check again")
        rc = relaunched.quit()
        if rc != 0:
            raise Failure(f"quit rc={rc}{relaunched.tail()}")

        with open(result, "w") as fh:
            fh.write("ok 0.1.1\n")
        relaunched = relaunch(binary, app, stub, update_dir, apps, "updated")
        relaunched.wait_until(lambda s: s.get("statusMessage") == "GitBolt was updated to 0.1.1.",
                              "the status bar's word on the update")
        state = relaunched.state()
        if "updateDialog" in state:
            raise Failure(f"an update that went in opened the dialog: {state['updateDialog']}")
        if os.path.exists(result):
            raise Failure("install-result is still there after it was read")
        rc = relaunched.quit()
        if rc != 0:
            raise Failure(f"quit rc={rc}{relaunched.tail()}")
    finally:
        stub.close()


def check_private_dir(path: str, runtime_dir: str) -> None:
    """A test session's downloads folder: its own, in its TMPDIR, that
    only this user may enter."""
    if not path:
        raise Failure("dump-state has no updateDir")
    if os.path.realpath(os.path.dirname(path)) != os.path.realpath(runtime_dir):
        raise Failure(f"updateDir {path!r} isn't in the session's TMPDIR {runtime_dir!r}")
    if not os.path.basename(path).startswith("gitbolt-updates-"):
        raise Failure(f"updateDir {path!r} isn't a gitbolt-updates-XXXXXX folder")
    st = os.lstat(path)
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.getuid() or st.st_mode & 0o077:
        raise Failure(f"updateDir {path!r} isn't private: mode {oct(st.st_mode)}")


def relaunch(binary: str, old: App, stub: UpdateFeed, update_dir: str, apps: list,
             label: str) -> App:
    """A copy started as the install helper starts one: the old session's
    TMPDIR and environment, GITBOLT_TEST_UPDATE_DIR included."""
    app = App(binary, runtime_dir=old.runtime_dir, label=label,
              extra_env={"GITBOLT_TEST_UPDATE_FEED": stub.feed_url,
                         "GITBOLT_TEST_APP_VERSION": "0.1.0",
                         "GITBOLT_TEST_UPDATE_DIR": update_dir})
    apps.append(app)
    app.wait_bridge()
    state = app.state()
    if state.get("updateDir") != update_dir:
        raise Failure(f"the relaunched copy downloads into {state.get('updateDir')!r}, "
                      f"not {update_dir!r}")
    return app


if __name__ == "__main__":
    run(test)
