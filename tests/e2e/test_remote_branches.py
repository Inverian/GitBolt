#!/usr/bin/env python3
"""E2E: publishing a new branch and deleting it on the remote.

The toolbar Push on a branch that has never been pushed used to fail
("has no upstream branch"), so every first push needed shell git. It now
publishes the branch with --set-upstream. Commands > Delete remote
branch then removes it on the server (behind a confirmation) and drops
the local remote-tracking ref. Asserted against a local bare origin.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, git, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    origin = os.path.join(scratch, "origin.git")
    git("init", "-q", "--bare", origin, cwd=scratch)
    git("remote", "add", "origin", origin, cwd=work)
    # Nothing pushed yet: "feature" will be origin's only branch, so it
    # is also the only (default) entry in the delete picker below.
    git("checkout", "-q", "-b", "feature", cwd=work)
    with open(os.path.join(work, "f.txt"), "w") as fh:
        fh.write("feature\n")
    git("add", "f.txt", cwd=work)
    git("commit", "-q", "-m", "feature work", cwd=work)
    head = git("rev-parse", "HEAD", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "feature",
                   "repo open on feature")

    # 1. Toolbar Push publishes the branch and sets its upstream.
    app.ok("trigger act.push")
    app.wait_until(lambda s: _origin_rev(origin, "feature") == head,
                   "origin to receive the new branch", timeout=30)
    upstream = git("rev-parse", "--abbrev-ref", "feature@{upstream}", cwd=work)
    if upstream != "origin/feature":
        raise Failure(f"upstream not set by the first push: {upstream!r}")

    # 2. Commands > Delete remote branch, confirmed.
    app.ok("trigger commands.delete-remote-branch")
    app.wait_until(lambda s: "Delete Remote Branch" in s.get("windows", []),
                   "remote branch picker visible")
    app.ok("click OK")
    # The confirmation shares the picker's title, so wait for its Yes
    # button rather than the window title (which the closing picker
    # would also satisfy).
    app.wait_until(lambda s: any(b["text"] == "Yes" for b in
                                 app.cmd("list-widgets")["buttons"]),
                   "confirmation visible")
    app.ok("click Yes")
    app.wait_until(lambda s: _origin_rev(origin, "feature") == "",
                   "origin to drop the branch", timeout=30)
    app.wait_until(
        lambda s: _local_rev(work, "refs/remotes/origin/feature") == "",
        "local remote-tracking ref to go")
    if git("rev-parse", "HEAD", cwd=work) != head:
        raise Failure("deleting the remote branch touched the local one")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


def _origin_rev(origin: str, branch: str) -> str:
    return _local_rev(origin, f"refs/heads/{branch}")


def _local_rev(repo: str, ref: str) -> str:
    try:
        return git("rev-parse", "--verify", "-q", ref, cwd=repo)
    except RuntimeError:
        return ""


if __name__ == "__main__":
    run(test)
