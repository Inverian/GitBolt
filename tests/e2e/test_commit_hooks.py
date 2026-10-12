#!/usr/bin/env python3
"""E2E: the Commit dialog commits through git, hooks and all.

GitBolt made every commit with libgit2, which runs no hooks: a
pre-commit that refuses (a secret scanner, a linter) was skipped
without a word, and so was a commit-msg hook (Gerrit's Change-Id).
Now a refusing pre-commit keeps HEAD where it is, the dialog stays
open, and the status bar says what the hook said; once the hook lets
it through, commit-msg's line is in the commit's message.
"""

from __future__ import annotations

import os
import stat

from e2elib import App, Failure, git, make_repo, run

REFUSAL = "pre-commit: secrets found in b.txt"


def _hook(hooks: str, name: str, body: str) -> None:
    path = os.path.join(hooks, name)
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\n" + body)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def test(binary: str, scratch: str, apps: list) -> None:
    # git's global configuration: none, so a developer's core.hooksPath
    # or commit.gpgSign stays out of it (the app inherits this).
    global_config = os.path.join(scratch, "gitconfig")
    open(global_config, "w").close()
    os.environ["GIT_CONFIG_GLOBAL"] = global_config
    os.environ["GIT_CONFIG_NOSYSTEM"] = "1"

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

    app.ok("trigger " + app.find_action_slug("commands/commit"))
    app.wait_until(
        lambda s: any("Commit" in w for w in s.get("windows", [])),
        "commit dialog visible")
    app.wait_view_rows("commit.unstagedList", 1)
    app.ok("click commit.stageAllBtn")
    app.wait_view_rows("commit.stagedList", 1)
    app.ok("type commit.message hooked")
    app.ok("click commit.commitBtn")

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
