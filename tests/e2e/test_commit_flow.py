#!/usr/bin/env python3
"""E2E: selective staging + multi-line message + Commit & Push.

Covers, in one user-shaped flow, the review items that were only ever
verified by hand: the commit dialog's selection-gated Stage button,
multi-row selection, the multi-line message editor, and the
Commit & Push wiring all the way into a (local, bare) origin.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, git, make_repo, run

SUBJECT = "e2e: stage two of four and push"
BODY = "Rows 0 and 2 selected through the bridge."


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    origin = os.path.join(scratch, "origin.git")
    git("init", "-q", "--bare", "-b", "main", origin, cwd=scratch)
    git("remote", "add", "origin", origin, cwd=work)
    git("push", "-q", "-u", "origin", "main", cwd=work)
    for f in ("a.txt", "b.txt", "c.txt", "d.txt"):
        with open(os.path.join(work, f), "w") as fh:
            fh.write(f"new {f}\n")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 1,
                   "repo open with log loaded")
    head_before = app.state()["headOid"]

    # Open the commit dialog (slug carries a live count: commit-4).
    app.ok("trigger " + app.find_action_slug("commands/commit"))
    app.wait_until(
        lambda s: any("Commit" in w for w in s.get("windows", [])),
        "commit dialog visible")
    app.wait_view_rows("commit.unstagedList", 4)

    # Multi-row staging: a.txt + c.txt only.
    sel = app.ok("select-row commit.unstagedList 0,2")
    if sel["selectedRows"] != 2:
        raise Failure(f"multi-select reported {sel['selectedRows']} rows")
    app.ok("click commit.stageBtn")
    app.wait_view_rows("commit.stagedList", 2)
    app.wait_view_rows("commit.unstagedList", 2)

    app.ok(f"type commit.message {SUBJECT}\\n\\n{BODY}")
    app.ok("click commit.commitPushBtn")

    # The push is async; the bare origin receiving the commit is the
    # outcome that matters.
    app.wait_until(
        lambda s: s.get("headOid") and s["headOid"] != head_before,
        "HEAD to move after commit")
    head_after = app.state()["headOid"]

    deadline_state = app.wait_until(
        lambda s: _origin_head(origin) == head_after,
        "origin to receive the push", timeout=30)
    del deadline_state

    message = git("log", "-1", "--format=%B", cwd=work)
    if SUBJECT not in message or BODY not in message:
        raise Failure(f"multi-line message mangled: {message!r}")
    files = set(git("show", "--name-only", "--format=", "HEAD",
                    cwd=work).split())
    if files != {"a.txt", "c.txt"}:
        raise Failure(f"commit should contain exactly a.txt+c.txt: {files}")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


def _origin_head(origin: str) -> str:
    try:
        return git("rev-parse", "HEAD", cwd=origin)
    except RuntimeError:
        return ""


if __name__ == "__main__":
    run(test)
