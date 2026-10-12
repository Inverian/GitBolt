#!/usr/bin/env python3
"""E2E: a test-bridge session never sends usage statistics.

dump-state's `telemetry` is the gate main() evaluated: "compiled-out"
in a build without endpoints (pull requests, forks, source builds), or
"inert:test-bridge" in the binary CI ships, which has them.
`telemetryIfConfigured` is the gate the same session would get with
endpoints compiled in, so a pull request's build shows the shipped one
would stay quiet too. Neither may ever be "on".

The session also leaves no trace: no telemetry queue next to
GitBolt.ini and no install ID in it, even after the Settings switch is
toggled and an action is triggered. (Opening Settings must never
create an ID; only a session that sends does.)

And every menu and toolbar action's objectName — what usage statistics
count — must be a plain identifier, so a name derived from a menu text
can't carry anything else. Recent Repositories entries are left out:
they all count as "file.recent".
"""

from __future__ import annotations

import os
import re

from e2elib import App, Failure, make_repo, run

NAME = re.compile(r"^[A-Za-z][A-Za-z0-9._-]{0,63}$")
INERT = {"compiled-out", "inert:test-bridge"}


def test(binary: str, scratch: str, apps: list) -> None:
    repo = make_repo(os.path.join(scratch, "repo"))
    config_home = os.path.join(scratch, "config")
    # A home of its own: OK in Settings writes the Git Config page's
    # name and email back with `git config --global`, and a crash would
    # write ~/.gitbolt/crash.log.
    home = os.environ.get("HOME")
    os.environ["HOME"] = os.path.join(scratch, "home")
    try:
        app = App(binary, repo, config_home=config_home)
    finally:
        if home is None:
            del os.environ["HOME"]
        else:
            os.environ["HOME"] = home
    apps.append(app)
    app.wait_bridge()
    state = app.wait_until(lambda s: s.get("repoOpen"), "repo open")

    gate = state.get("telemetry")
    if gate not in INERT:
        raise Failure(f"bridge session's telemetry gate is {gate!r}, "
                      f"expected one of {sorted(INERT)}")
    if_configured = state.get("telemetryIfConfigured")
    if if_configured != "inert:test-bridge":
        raise Failure("with endpoints compiled in, a bridge session's gate "
                      f"would be {if_configured!r}, not 'inert:test-bridge'")

    bad = []
    for action in app.cmd("list-actions")["actions"]:
        name = action.get("objectName", "")
        if name and not name.startswith("file.recent.") \
                and not NAME.match(name):
            bad.append(f"{name!r} ({action['slug']})")
    if bad:
        raise Failure("action objectNames usage statistics would count "
                      "must match " + NAME.pattern + ": " + ", ".join(bad))

    # Settings › General › Usage statistics: there, and toggled off,
    # when the build has endpoints; absent when it doesn't.
    app.ok("trigger tools.settings")
    app.wait_until(lambda s: "Settings" in s.get("windows", []),
                   "Settings dialog visible", timeout=10)
    buttons = {b.get("objectName") for b in app.cmd("list-widgets")["buttons"]}
    shown = "settings.usage-statistics" in buttons
    if shown != (gate != "compiled-out"):
        raise Failure(f"Usage statistics switch shown={shown} with the "
                      f"gate {gate!r}")
    if shown:
        app.ok("click settings.usage-statistics")
    app.ok("click OK")
    app.wait_until(lambda s: "Settings" not in s.get("windows", []),
                   "Settings dialog closed")

    app.ok("trigger act.refresh")
    if app.state().get("telemetry") not in INERT:
        raise Failure("the telemetry gate changed during the session")
    rc = app.quit()
    if rc != 0:
        raise Failure(f"quit rc={rc}{app.tail()}")

    # "telemetry", or "telemetry-local-<hash>" for a loopback stub build.
    ini_dir = os.path.join(config_home, "GitBolt")
    for entry in os.listdir(ini_dir):
        if entry.startswith("telemetry"):
            queue_dir = os.path.join(ini_dir, entry)
            raise Failure(f"a bridge session created {queue_dir}: "
                          f"{os.listdir(queue_dir)}")
    with open(os.path.join(ini_dir, "GitBolt.ini")) as fh:
        ini = fh.read()
    if "installId" in ini:
        raise Failure("a bridge session created an install ID in GitBolt.ini")
    if gate == "compiled-out" and "[telemetry]" in ini:
        raise Failure("Settings wrote a [telemetry] key in a build without "
                      "telemetry:\n" + ini)


if __name__ == "__main__":
    run(test)
