#!/usr/bin/env python3
"""E2E: a local op runs off the GUI thread.

Commands > Merge branches — like Undo last commit, Reset changes,
Clean, Bisect, the Reflog's Checkout and Reset and the sidebar's Merge
— ran git on the GUI thread, with a 30 s limit. A post-merge hook that
took a while froze the window (the bridge, which the GUI thread serves,
went silent) until the limit killed git, orphaning the hook, and
reported the merge, done by then, as failed, with an offer to resolve
conflicts it never had.

1. The merge now runs on a pool thread: the window answers at once,
   says "Merging topic…", and keeps the repository's actions off until
   git is done. Then the merge is in, with no warning, and they're back.
2. Quitting while it runs stops git and its hook, as Ctrl-C would, and
   GitBolt exits promptly.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import time

from e2elib import App, Failure, git, make_repo, run


def commit(work: str, name: str, msg: str) -> None:
    with open(os.path.join(work, name), "w") as fh:
        fh.write(f"{msg}\n")
    git("add", name, cwd=work)
    git("commit", "-q", "-m", msg, cwd=work)


def warning_up(app: App) -> bool:
    """A message box is open (untitled on macOS, so not by its title)."""
    return any(w["class"] == "QMessageBox"
               for w in app.cmd("list-widgets")["windows"])


def merge(app: App, branch: str) -> None:
    """Commands > Merge branches, `branch` picked."""
    app.ok("trigger commands.merge-branches")
    app.wait_until(lambda s: "Merge Branch" in s.get("windows", []),
                   "the merge picker")
    combos = app.cmd("list-widgets")["combos"]
    picker = next((c for c in combos if branch in c["items"]
                   and c.get("objectName") != "toolbar.branchCombo"), None)
    if picker is None:
        raise Failure(f"the merge picker's combo not reported: {combos}")
    app.ok(f"select-item QComboBox:{picker['index']} {branch}")
    app.ok("click OK")


def hook_pid(pid_file: str, timeout: float = 20) -> int:
    """The pid the post-merge hook noted once it started."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open(pid_file) as fh:
                pid = fh.read().strip()
            if pid:
                return int(pid)
        except OSError:
            pass
        time.sleep(0.1)
    raise Failure("the merge's post-merge hook never ran")


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    # A post-merge hook that notes its pid and waits while `hold`
    # exists (a minute at most). Hooks of our own, whatever
    # core.hooksPath says globally.
    hold = os.path.join(scratch, "hold")
    pid_file = os.path.join(scratch, "hook.pid")
    hooks = os.path.join(scratch, "hooks")
    os.makedirs(hooks)
    hook = os.path.join(hooks, "post-merge")
    with open(hook, "w") as fh:
        fh.write("#!/bin/sh\n"
                 f"echo $$ > {shlex.quote(pid_file)}\n"
                 "n=0\n"
                 f"while [ -f {shlex.quote(hold)} ] && [ $n -lt 600 ]; do\n"
                 "    sleep 0.1; n=$((n + 1))\ndone\n")
    os.chmod(hook, 0o755)
    git("config", "core.hooksPath", hooks, cwd=work)
    git("checkout", "-q", "-b", "topic", cwd=work)
    commit(work, "t.txt", "topic")
    topic = git("rev-parse", "HEAD", cwd=work)
    git("checkout", "-q", "-b", "later", cwd=work)
    commit(work, "u.txt", "later")
    git("checkout", "-q", "main", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main",
                   "repo open on main")

    # 1. A merge the hook holds up: the window answers meanwhile.
    open(hold, "w").close()
    merge(app, "topic")
    hook_pid(pid_file)
    start = time.monotonic()
    try:
        state = app.cmd("dump-state", timeout=1.0)
    except OSError:
        raise Failure("the window didn't answer while git merged: the "
                      "merge ran on the GUI thread") from None
    if time.monotonic() - start > 1.0:
        raise Failure("dump-state took over a second while git merged")
    if not state.get("statusMessage", "").startswith("Merging"):
        raise Failure(f"no word of the merge: {state.get('statusMessage')!r}")
    if not state.get("opIndicator", "").startswith("Merging topic"):
        raise Failure(f"the inline label: {state.get('opIndicator')!r}")
    if app.action_enabled("act.commit"):
        raise Failure("Commit enabled while git merges")

    os.remove(hold)
    app.wait_until(lambda s: s.get("headOid") == topic
                   and app.action_enabled("act.commit"),
                   "the merge in, and Commit back")
    time.sleep(1)       # a warning, had there been one, is up by now
    if warning_up(app):
        raise Failure("the merge, which went through, was reported "
                      "as failed")
    if app.state().get("opIndicator"):
        raise Failure("the inline label still says the merge runs")

    # 2. Quitting while a merge's hook runs: git and the hook are
    # stopped, and GitBolt goes.
    os.remove(pid_file)
    open(hold, "w").close()
    merge(app, "later")
    pid = hook_pid(pid_file)
    try:
        rc = app.quit(timeout=10)
    except subprocess.TimeoutExpired:
        raise Failure("GitBolt was still running 10 s after quit: it "
                      "waited for the merge's hook") from None
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc} after quitting during "
                      "a merge")
    deadline = time.monotonic() + 3
    while alive(pid):
        if time.monotonic() > deadline:
            raise Failure("the merge's hook ran on after GitBolt quit")
        time.sleep(0.1)
    if os.path.exists(os.path.join(work, ".git", "index.lock")):
        raise Failure("git left its index.lock behind")


if __name__ == "__main__":
    run(test)
