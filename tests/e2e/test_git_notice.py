#!/usr/bin/env python3
"""The git GitBolt runs, and the note about it.

Started as the Dock starts it (launchd's PATH), GitBolt must run the
login shell's git: here a stand-in that answers --version with a flawed
version, notes every command and runs the real git for the rest. It
must ask that git its version and fetch with it, show the note, which
Dismiss hides, and not show it again on the next launch. What it says
about other gits is TestGitNotice's.
"""
from __future__ import annotations

import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import e2elib  # noqa: E402
from e2elib import App, Failure, check_for, git, make_repo, run  # noqa: E402

DISMISS = "gitNotice.dismiss"

# On Linux, whose distributions backport fixes and keep the version, only
# a git older than GitBolt's floor gets a note.
FLAWED = ("git version 2.39.5 (Apple Git-154)" if sys.platform == "darwin"
          else "git version 2.20.1")


def stand_in_git(tools: str, real_git: str, log: str) -> None:
    path = os.path.join(tools, "git")
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\n"
                 f"printf '%s\\n' \"$*\" >> '{log}'\n"
                 f"if [ \"$1\" = --version ]; then echo '{FLAWED}'; exit 0; fi\n"
                 f"exec '{real_git}' \"$@\"\n")
    os.chmod(path, 0o755)


def commands(log: str, word: str) -> int:
    """How many commands the stand-in ran with `word` among their
    arguments (a fetch comes after -c fetch.prune=true)."""
    if not os.path.exists(log):
        return 0
    with open(log) as fh:
        return sum(1 for line in fh if word in line.split())


def notice_shown(app: App) -> bool:
    return any(b.get("objectName") == DISMISS
               for b in app.cmd("list-widgets")["buttons"])


def launch(binary: str, work: str, home: str, config_home: str,
           label: str) -> App:
    """GitBolt as the Dock starts it: launchd's PATH, and a login shell
    (bash, where there is one) that reads ~/.profile."""
    app_env = e2elib.app_env

    def launchd_env(*args, **kwargs) -> dict:
        env = app_env(*args, **kwargs)
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        env["SHELL"] = "/bin/bash" if os.path.exists("/bin/bash") else "/bin/sh"
        env.pop("ENV", None)
        return env

    e2elib.app_env = launchd_env
    try:
        return App(binary, work, home=home, config_home=config_home, label=label)
    finally:
        e2elib.app_env = app_env


def wait_for(pred, what: str, app: App, timeout: float = 20.0) -> None:
    deadline = time.monotonic() + timeout
    while not pred():
        if time.monotonic() > deadline:
            raise Failure(f"{app.label}: timed out waiting for {what}{app.tail()}")
        time.sleep(0.2)


def test(binary: str, scratch: str, apps: list) -> None:
    real_git = shutil.which("git")
    if not real_git:
        raise Failure("no git on the test's PATH")
    home = os.path.join(scratch, "home")
    tools = os.path.join(home, "tools")
    os.makedirs(tools)
    with open(os.path.join(home, ".profile"), "w") as fh:
        fh.write('PATH="$HOME/tools:$PATH"; export PATH\n')
    log = os.path.join(scratch, "git.log")
    stand_in_git(tools, real_git, log)
    config_home = os.path.join(scratch, "config")

    upstream = os.path.join(scratch, "upstream.git")
    git("init", "-q", "--bare", "-b", "main", upstream, cwd=scratch)
    work = make_repo(os.path.join(scratch, "work"))
    git("remote", "add", "origin", upstream, cwd=work)
    git("push", "-q", "origin", "main", cwd=work)

    def start(label: str) -> App:
        asked = commands(log, "--version")
        app = launch(binary, work, home, config_home, label)
        apps.append(app)
        app.wait_bridge()
        wait_for(lambda: commands(log, "--version") > asked,
                 "GitBolt to ask the login shell's git its version", app)
        return app

    # A flawed git: the note, and the fetch runs that git too.
    app = start("flawed git")
    wait_for(lambda: notice_shown(app), "the note about the git", app)
    # The repository open and Fetch on first: the trigger does nothing
    # before.
    app.wait_until(lambda s: s.get("repoOpen"), "the repository to open")
    wait_for(lambda: app.action_enabled("act.fetch"), "Fetch to be enabled", app)
    fetches = commands(log, "fetch")
    app.ok("trigger act.fetch")
    wait_for(lambda: commands(log, "fetch") > fetches,
             "the fetch to run the login shell's git", app)
    app.ok(f"click {DISMISS}")
    wait_for(lambda: not notice_shown(app), "Dismiss to hide the note", app)
    if app.quit() != 0:
        raise Failure(f"app exited non-zero after quit{app.tail()}")

    # The same git again: dismissed for good.
    app = start("same git again")
    check_for(2, "the dismissed note came back",
              lambda: "shown" if notice_shown(app) else None)
    app.quit()


if __name__ == "__main__":
    if sys.platform == "win32":
        print("SKIP: the login shell's git under launchd's PATH has no Windows "
              "counterpart (TestGitNotice covers Windows' git versions)")
        sys.exit(77)
    run(test)
