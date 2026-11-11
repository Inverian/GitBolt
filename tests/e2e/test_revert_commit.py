#!/usr/bin/env python3
"""E2E: Commands > Revert commit.

GitBolt could carry on or abort a revert started in a terminal, but
couldn't start one. Now the dialog takes a commit (its field filled
with the one selected in the log), and Revert stages the inverse of
its changes with git's REVERT_HEAD and message, as a cherry-pick stages
a pick:

1. a clean revert is staged, the bar says a revert is in progress, and
   the Commit dialog's commit concludes it with git's message;
2. a revert that conflicts with a later change stops on the conflict
   (offering the resolver), and the bar's Abort puts everything back.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, git, make_repo, run


def commit(work: str, name: str, content: str, msg: str) -> str:
    with open(os.path.join(work, name), "w") as fh:
        fh.write(content)
    git("add", name, cwd=work)
    git("commit", "-q", "-m", msg, cwd=work)
    return git("rev-parse", "HEAD", cwd=work)


def buttons(app: App) -> dict[str, bool]:
    """Visible buttons: text -> enabled."""
    return {b["text"]: b["enabled"] for b in app.cmd("list-widgets")["buttons"]}


def answer(app: App, button: str) -> None:
    """Click `button` of a question box once it's up."""
    app.wait_until(lambda s: button in buttons(app), f"a {button} button")
    app.ok(f"click {button}")


def revert(app: App, commit_id: str) -> None:
    """Commands > Revert commit, `commit_id` typed in, Revert clicked."""
    app.ok("trigger commands.revert-commit")
    app.wait_until(lambda s: "Revert Commit" in s.get("windows", []),
                   "revert dialog visible")
    app.ok(f"type revert.commit {commit_id}")
    # The preview resolves the commit, which turns Revert on.
    app.wait_until(lambda s: buttons(app).get("Revert") is True,
                   "the dialog's Revert button on")
    app.ok("click Revert")
    app.wait_until(lambda s: "Revert Commit" not in s.get("windows", []),
                   "revert dialog closed")


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    git("config", "core.autocrlf", "false", cwd=work)
    two = commit(work, "a.txt", "two\n", "two")
    commit(work, "b.txt", "b\n", "add b")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 3,
                   "repo open with log loaded")
    head = app.state()["headOid"]

    # 1. A clean revert: staged, not committed, with git's state.
    revert(app, two[:10])
    app.wait_until(lambda s: s.get("repoState") == "revert", "a revert in progress")
    status = git("status", "--porcelain", cwd=work)
    if status != "M  a.txt":
        raise Failure(f"the revert should stage a.txt's old content: {status!r}")
    with open(os.path.join(work, "a.txt")) as fh:
        if fh.read() != "a.txt\n":
            raise Failure("a.txt wasn't put back")
    with open(os.path.join(work, ".git", "REVERT_HEAD")) as fh:
        if fh.read().strip() != two:
            raise Failure("REVERT_HEAD doesn't name the reverted commit")
    if app.state()["headOid"] != head:
        raise Failure("the revert committed by itself")
    # The bar's Commit… (list-widgets reports texts without the "…").
    app.wait_until(lambda s: buttons(app).get("Commit") is True,
                   "the bar's Commit…")

    # It opens the Commit dialog, git's message filled in.
    app.ok("click Commit…")
    app.wait_until(lambda s: any("Commit" in w for w in s.get("windows", []))
                   and buttons(app).get("Commit") is True,
                   "commit dialog ready")
    app.ok("click commit.commitBtn")
    app.wait_until(lambda s: s.get("headOid") != head and s.get("repoState") == "none",
                   "the revert committed")
    message = git("log", "-1", "--format=%B", cwd=work).strip()
    if message != f'Revert "two"\n\nThis reverts commit {two}.':
        raise Failure(f"the revert commit has another message: {message!r}")
    # base, two, add b, the revert: all that's left since base is b.txt.
    if git("diff", "--name-only", "HEAD~3", "HEAD", cwd=work) != "b.txt":
        raise Failure("the revert commit didn't undo 'two' alone")
    if git("status", "--porcelain", cwd=work) != "":
        raise Failure("changes left after the revert commit")

    # 2. A revert that a later change conflicts with stops on it.
    three = commit(work, "a.txt", "three\n", "three")
    commit(work, "a.txt", "four\n", "four")
    app.wait_until(lambda s: s.get("headOid") == git("rev-parse", "HEAD", cwd=work),
                   "GitBolt sees the new commits")
    head = app.state()["headOid"]
    revert(app, three)
    answer(app, "No")       # the offer to open the conflict resolver
    app.wait_until(lambda s: s.get("repoState") == "revert", "the revert stopped")
    status = git("status", "--porcelain", cwd=work)
    if status != "UU a.txt":
        raise Failure(f"the conflict isn't in the index: {status!r}")
    app.wait_until(lambda s: buttons(app).get("Abort") is True, "the bar's Abort")
    app.ok("click Abort")
    answer(app, "Yes")
    app.wait_until(lambda s: s.get("repoState") == "none", "the revert aborted")
    if git("rev-parse", "HEAD", cwd=work) != head:
        raise Failure("Abort moved the branch")
    if git("status", "--porcelain", cwd=work) != "":
        raise Failure("Abort left changes behind")
    with open(os.path.join(work, "a.txt")) as fh:
        if fh.read() != "four\n":
            raise Failure("Abort didn't put a.txt back")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


if __name__ == "__main__":
    run(test)
