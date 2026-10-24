#!/usr/bin/env python3
"""The git GitBolt runs, and the note about it.

GitBolt runs the first git on the PATH its children get, the login
shell's entries first. Started from the Dock, it looked on launchd's
PATH alone, and ran Apple's /usr/bin/git (2.39.5, open to
CVE-2025-48384) while the user's shell ran a patched git from Homebrew.

Here the login shell puts a git of the test's first. It answers
--version with a version the test picks, notes every command, and runs
the real git for everything else. GitBolt must run it, to ask its
version and to fetch alike, and:

  - with a flawed version (on macOS Apple's 2.39.5; on Linux, whose
    distributions backport fixes and keep the version, one older than
    GitBolt's floor) show the note, which Dismiss hides;
  - launched again with that git, not show it again;
  - with another flawed version of it, show it again;
  - with a git that is fine, show nothing.
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

if sys.platform == "darwin":
    FLAWED = "git version 2.39.5 (Apple Git-154)"
    ALSO_FLAWED = "git version 2.39.3 (Apple Git-146)"
else:
    FLAWED = "git version 2.20.1"
    ALSO_FLAWED = "git version 2.17.1"
FINE = "git version 2.99.0"   # one from the future: nothing is known against it


def stand_in_git(tools: str, real_git: str, log: str, version_file: str) -> None:
    path = os.path.join(tools, "git")
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\n"
                 f"printf '%s\\n' \"$*\" >> '{log}'\n"
                 f"if [ \"$1\" = --version ]; then cat '{version_file}'; exit 0; fi\n"
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
    version_file = os.path.join(scratch, "version")
    stand_in_git(tools, real_git, log, version_file)
    config_home = os.path.join(scratch, "config")

    upstream = os.path.join(scratch, "upstream.git")
    git("init", "-q", "--bare", "-b", "main", upstream, cwd=scratch)
    work = make_repo(os.path.join(scratch, "work"))
    git("remote", "add", "origin", upstream, cwd=work)
    git("push", "-q", "origin", "main", cwd=work)

    def start(version: str, label: str) -> App:
        with open(version_file, "w") as fh:
            fh.write(version + "\n")
        asked = commands(log, "--version")
        app = launch(binary, work, home, config_home, label)
        apps.append(app)
        app.wait_bridge()
        wait_for(lambda: commands(log, "--version") > asked,
                 "GitBolt to ask the login shell's git its version", app)
        return app

    # A flawed git: the note, and the fetch runs that git too.
    app = start(FLAWED, "flawed git")
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
    app = start(FLAWED, "same git again")
    check_for(2, "the dismissed note came back",
              lambda: "shown" if notice_shown(app) else None)
    app.quit()

    # Another flawed version of it: a note of its own.
    app = start(ALSO_FLAWED, "another version")
    wait_for(lambda: notice_shown(app), "the note about another version", app)
    app.quit()

    # A git with nothing wrong: no note.
    app = start(FINE, "a fine git")
    check_for(2, "a note about a git with nothing wrong",
              lambda: "shown" if notice_shown(app) else None)
    app.quit()


if __name__ == "__main__":
    run(test)
