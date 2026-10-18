#!/usr/bin/env python3
"""A bare repository open in GitBolt: its git commands run in it.

A bare repository has no work tree, and git ran with no working
directory at all, so in GitBolt's own: started from a terminal inside
another repository, Fetch fetched into that one and Push pushed its
unpublished commits to its remote. Now they run in the bare
repository's git dir.

Opened from a folder inside it (refs/heads), as Finder's Open in
GitBolt or a Dock drop can, the bare repository is found too. git is
told which repository it is (GIT_DIR): with safe.bareRepository set
to "explicit", git won't use a bare repository it finds by itself.
Pull and Commit, which need a work tree, are off.

And git never takes the repository from GitBolt's own environment:
started from a shell that exports GIT_DIR and GIT_WORK_TREE (a bare
dotfiles setup), every command went to that repository instead of the
open one.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import App, Failure, git, make_repo, run  # noqa: E402


def refs(repo: str) -> list[str]:
    return git("for-each-ref", "--format=%(refname) %(objectname)", cwd=repo).splitlines()


def run_op(app: App, action: str, what: str) -> None:
    """Trigger a toolbar op and wait for its own outcome, which must be
    a success: "✓ Push complete." (the label of the op before it may
    still be up when the trigger returns)."""
    app.ok(f"trigger {action}")
    deadline = time.monotonic() + 30
    label = ""
    while time.monotonic() < deadline:
        label = (app.state().get("opIndicator") or "").strip()
        if label.startswith(("✓", "✗")) and what.lower() in label.lower():
            if not label.startswith("✓"):
                raise Failure(f"{what} failed: {label!r}")
            return
        time.sleep(0.3)
    raise Failure(f"{what} never finished: {label!r}")


def test(binary: str, scratch: str, apps: list) -> None:
    # Where GitBolt is started from: a clone with a commit not yet
    # pushed, and a remote that must not get it.
    upstream = os.path.join(scratch, "upstream.git")
    git("init", "-q", "--bare", "-b", "main", upstream, cwd=scratch)
    here = make_repo(os.path.join(scratch, "here"))
    git("remote", "add", "origin", upstream, cwd=here)
    git("push", "-q", "-u", "origin", "main", cwd=here)
    with open(os.path.join(here, "a.txt"), "a") as fh:
        fh.write("not to be pushed\n")
    git("commit", "-q", "-am", "local only", cwd=here)
    upstream_before = refs(upstream)
    here_before = refs(here)

    # The bare repository, with a remote of its own.
    other = make_repo(os.path.join(scratch, "other"))
    bare = os.path.join(scratch, "b.git")
    git("clone", "-q", "--bare", other, bare, cwd=scratch)

    # The app's own home, where git's hardening refuses a bare
    # repository that isn't named outright.
    home = os.path.join(scratch, "home")
    os.makedirs(home)
    with open(os.path.join(home, ".gitconfig"), "w") as fh:
        fh.write("[safe]\n\tbareRepository = explicit\n")

    app = App(binary, repo=os.path.join(bare, "refs", "heads"), cwd=here, home=home)
    apps.append(app)
    app.wait_bridge()
    # dump-state's repoPath is the work tree: none here. The title says.
    app.wait_until(lambda s: s.get("repoOpen")
                   and any(t.startswith("b.git") for t in s["windows"]), "b.git open")

    for name in ("act.pull", "act.commit"):
        if app.action_enabled(name):
            raise Failure(f"{name} is on in a bare repository")
    if not app.action_enabled("act.push"):
        raise Failure("act.push is off in a bare repository")

    run_op(app, "act.fetch", "Fetch")
    run_op(app, "act.push", "Push")
    time.sleep(1)

    if refs(upstream) != upstream_before:
        raise Failure(f"the starting folder's remote changed: {upstream_before} -> {refs(upstream)}")
    if refs(here) != here_before:
        raise Failure(f"the starting folder's repository changed: {here_before} -> {refs(here)}")
    if not os.path.exists(os.path.join(bare, "FETCH_HEAD")):
        raise Failure("Fetch did not run in the bare repository (no FETCH_HEAD there)")
    # Push published main from the bare repository to its own remote.
    if git("config", "branch.main.remote", cwd=bare) != "origin":
        raise Failure("Push did not run in the bare repository")
    app.quit()

    # GIT_DIR and GIT_WORK_TREE in GitBolt's environment, pointing at
    # `here`, while a plain repository with a remote of its own is open.
    upstream2 = os.path.join(scratch, "upstream2.git")
    git("init", "-q", "--bare", "-b", "main", upstream2, cwd=scratch)
    plain = make_repo(os.path.join(scratch, "plain"))
    git("remote", "add", "origin", upstream2, cwd=plain)
    git("push", "-q", "-u", "origin", "main", cwd=plain)
    with open(os.path.join(plain, "a.txt"), "a") as fh:
        fh.write("to be pushed\n")
    git("commit", "-q", "-am", "plain's own", cwd=plain)
    app2 = App(binary, repo=plain, label="exported GIT_DIR",
               extra_env={"GIT_DIR": os.path.join(here, ".git"), "GIT_WORK_TREE": here})
    apps.append(app2)
    app2.wait_bridge()
    app2.wait_until(lambda s: s.get("repoOpen")
                    and any(t.startswith("plain") for t in s["windows"]), "plain open")
    run_op(app2, "act.push", "Push")
    time.sleep(1)
    if refs(upstream) != upstream_before:
        raise Failure(f"pushed to the exported GIT_DIR's remote: {upstream_before} -> {refs(upstream)}")
    if git("rev-parse", "main", cwd=upstream2) != git("rev-parse", "HEAD", cwd=plain):
        raise Failure("the open repository's commit was not pushed to its own remote")


run(test)
