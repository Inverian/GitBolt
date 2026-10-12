#!/usr/bin/env python3
"""E2E: the Commit dialog commits through git, hooks and all.

GitBolt made every commit with libgit2, which runs no hooks: a
pre-commit that refuses (a secret scanner, a linter) was skipped
without a word, and so was a commit-msg hook (Gerrit's Change-Id).
Now a refusing pre-commit keeps HEAD where it is, the dialog stays
open, and the status bar says what the hook said; once the hook lets
it through, commit-msg's line is in the commit's message.

A Commit & Push whose commit ends while its own repository is being
opened again (a second launch handing it over) waits for that open:
its "Commit created" came over "Opening…", and the push was dropped
without a word, Push being off until the open was in.

And the hooks find the tools the user's login shell puts on PATH. A
GitBolt started from the Dock has launchd's PATH (/usr/bin:/bin:
/usr/sbin:/sbin), and a husky or lint-staged hook said "node: command
not found" to every commit. Here the app starts with that PATH, its
login shell's ~/.profile adds a tools dir, and the hook needs a tool
from there.
"""

from __future__ import annotations

import os
import stat
import threading
import time

import e2elib
from e2elib import App, Failure, git, make_repo, run

REFUSAL = "pre-commit: secrets found in b.txt"


def _hook(hooks: str, name: str, body: str) -> None:
    path = os.path.join(hooks, name)
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\n" + body)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _commit(app: App, message: str) -> None:
    """Commands > Commit, everything staged, `message`, Commit."""
    app.ok("trigger " + app.find_action_slug("commands/commit"))
    app.wait_until(
        lambda s: any("Commit" in w for w in s.get("windows", [])),
        "commit dialog visible")
    app.wait_view_rows("commit.unstagedList", 1)
    app.ok("click commit.stageAllBtn")
    app.wait_view_rows("commit.stagedList", 1)
    app.ok(f"type commit.message {message}")
    app.ok("click commit.commitBtn")


class Gate:
    """A FIFO the repository's config includes: while the gate is shut,
    reading the config (libgit2's open, any git run there) waits."""

    def __init__(self, repo: str, scratch: str) -> None:
        self.fifo = os.path.join(scratch, "gate.fifo")
        os.mkfifo(self.fifo)
        self._open = threading.Event()
        self._open.set()
        with open(os.path.join(repo, ".git", "config"), "a") as fh:
            fh.write(f"[include]\n\tpath = {self.fifo}\n")
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self) -> None:
        while True:
            self._open.wait()
            try:
                with open(self.fifo, "w"):
                    pass
            except OSError:
                return      # the scratch dir is gone
            time.sleep(0.005)

    def shut(self) -> None:
        self._open.clear()
        # A writer waiting for a reader gets one, and goes round to wait.
        fd = os.open(self.fifo, os.O_RDONLY | os.O_NONBLOCK)
        time.sleep(0.05)
        os.close(fd)

    def open(self) -> None:
        self._open.set()


def commit_and_push_across_reopen(binary: str, scratch: str, apps: list) -> None:
    origin = os.path.join(scratch, "origin.git")
    git("init", "-q", "--bare", "-b", "main", origin, cwd=scratch)
    work = make_repo(os.path.join(scratch, "pushed"))
    git("remote", "add", "origin", origin, cwd=work)
    git("push", "-q", "-u", "origin", "main", cwd=work)
    hooks = os.path.join(work, ".git", "hooks")
    os.makedirs(hooks, exist_ok=True)
    _hook(hooks, "pre-commit", "sleep 3\n")
    with open(os.path.join(work, "p.txt"), "w") as fh:
        fh.write("p\n")
    # Nothing after the commit that reads the config (which waits at the
    # gate): git commit's auto maintenance is a git of its own.
    git("config", "maintenance.auto", "false", cwd=work)
    git("config", "gc.auto", "0", cwd=work)
    gate = Gate(work, scratch)

    app = App(binary, work, label="Commit & Push across a reopen")
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 1,
                   "repo open with log loaded")
    head_before = app.state()["headOid"]
    app.ok("trigger " + app.find_action_slug("commands/commit"))
    app.wait_until(
        lambda s: any("Commit" in w for w in s.get("windows", [])),
        "commit dialog visible")
    app.wait_view_rows("commit.unstagedList", 1)
    app.ok("click commit.stageAllBtn")
    app.wait_view_rows("commit.stagedList", 1)
    app.ok("type commit.message commit-and-push")
    app.ok("click commit.commitPushBtn")
    time.sleep(1)
    # The same repository handed over while the hook runs; its open
    # waits at the gate until the commit is in.
    gate.shut()
    second = app.start_second(work)
    second.communicate(timeout=30)
    # Off the ref file: any git in the repository waits at the gate too.
    ref = os.path.join(work, ".git", "refs", "heads", "main")

    def head() -> str:
        with open(ref) as fh:
            return fh.read().strip()

    deadline = time.monotonic() + 20
    while head() == head_before:
        if time.monotonic() > deadline:
            raise Failure("the commit was never made")
        time.sleep(0.2)
    time.sleep(1.5)
    state = app.state()
    if not any("Opening" in w for w in state.get("windows", [])):
        raise Failure(f"the open landed before the gate opened: {state.get('windows')}")
    if "Commit created" in state.get("statusMessage", ""):
        raise Failure("the commit's outcome came over \"Opening…\"")
    gate.open()
    deadline = time.monotonic() + 30
    while git("rev-parse", "main", cwd=origin) != head():
        if time.monotonic() > deadline:
            raise Failure("the commit was never pushed: "
                          f"{app.state().get('statusMessage')!r}")
        time.sleep(0.3)
    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


def login_path(binary: str, scratch: str, apps: list) -> None:
    home = os.path.join(scratch, "login-home")
    tools = os.path.join(home, "tools")
    os.makedirs(tools)
    _hook(tools, "gbtool", "exit 0\n")
    with open(os.path.join(home, ".profile"), "w") as fh:
        fh.write('PATH="$HOME/tools:$PATH"; export PATH\n')

    work = make_repo(os.path.join(scratch, "login"))
    hooks = os.path.join(work, ".git", "hooks")
    os.makedirs(hooks, exist_ok=True)
    _hook(hooks, "pre-commit",
          "gbtool || { echo 'pre-commit: no gbtool on PATH' >&2; exit 1; }\n")
    with open(os.path.join(work, "c.txt"), "w") as fh:
        fh.write("c\n")

    app_env = e2elib.app_env

    def launchd_env(*args, **kwargs) -> dict:
        env = app_env(*args, **kwargs)
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        # A login shell that reads ~/.profile: bash, where there is one.
        env["SHELL"] = "/bin/bash" if os.path.exists("/bin/bash") else "/bin/sh"
        env.pop("ENV", None)
        return env

    e2elib.app_env = launchd_env
    try:
        app = App(binary, work, home=home, label="launched with launchd's PATH")
    finally:
        e2elib.app_env = app_env
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 1,
                   "repo open with log loaded")
    head_before = app.state()["headOid"]
    _commit(app, "needs-gbtool")
    state = app.wait_until(
        lambda s: (s.get("headOid") and s["headOid"] != head_before)
        or "Commit failed" in s.get("statusMessage", ""),
        "the commit's outcome")
    if state["headOid"] == head_before:
        raise Failure("the hook didn't find the login shell's tool: "
                      f"{state.get('statusMessage')!r}")
    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


def test(binary: str, scratch: str, apps: list) -> None:
    # git's global configuration: none, so a developer's core.hooksPath
    # or commit.gpgSign stays out of it (the app inherits this).
    global_config = os.path.join(scratch, "gitconfig")
    open(global_config, "w").close()
    os.environ["GIT_CONFIG_GLOBAL"] = global_config
    os.environ["GIT_CONFIG_NOSYSTEM"] = "1"

    login_path(binary, scratch, apps)
    commit_and_push_across_reopen(binary, scratch, apps)

    work = make_repo(os.path.join(scratch, "work"))
    hooks = os.path.join(work, ".git", "hooks")
    os.makedirs(hooks, exist_ok=True)
    # Hooks run at the top of the work tree.
    _hook(hooks, "pre-commit",
          f"if [ -f .git/BLOCK ]; then echo '{REFUSAL}' >&2; exit 1; fi\n")
    _hook(hooks, "commit-msg", "echo 'Change-Id: I123' >> \"$1\"\n")
    block = os.path.join(work, ".git", "BLOCK")
    open(block, "w").close()
    with open(os.path.join(work, "b.txt"), "w") as fh:
        fh.write("b\n")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 1,
                   "repo open with log loaded")
    head_before = app.state()["headOid"]

    _commit(app, "hooked")

    app.wait_until(lambda s: REFUSAL in s.get("statusMessage", ""),
                   "the hook's refusal on the status bar")
    state = app.state()
    if state["headOid"] != head_before:
        raise Failure("a commit the pre-commit hook refused was made")
    if not any("Commit" in w for w in state.get("windows", [])):
        raise Failure("the commit dialog closed on a refused commit")

    os.remove(block)
    app.ok("click commit.commitBtn")
    app.wait_until(
        lambda s: s.get("headOid") and s["headOid"] != head_before,
        "HEAD to move once the hook lets the commit through")
    message = git("log", "-1", "--format=%B", cwd=work)
    if message != "hooked\nChange-Id: I123":
        raise Failure(f"commit-msg hook's line missing: {message!r}")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


if __name__ == "__main__":
    run(test)
