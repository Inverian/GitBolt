#!/usr/bin/env python3
"""E2E: a remote op reports its own outcome.

1. Whether the running remote op had failed was a window-wide flag
   that any failed fetch, pull, push or checkout set. A checkout that
   failed while a Fetch ran (Commands > Check out branch, a dirty a.txt
   in the way) made the Fetch, which went through, end in a bare red ✗,
   without "Fetch complete.". Each remote op now says how it went
   itself. The Fetch is slowed down on the "server" side: origin's
   upload-pack waits a few seconds before it answers.
2. Create tag's "Push tag after creation" pushed on the GUI thread,
   whatever else ran, and its failure set that flag too. The push is a
   remote op of its own now: turned down while another one runs (the
   tag is created all the same), and narrated like any other.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, SilentServer, git, make_repo, run


def create_tag(app: App, name: str) -> None:
    """Commands > Create tag `name` on the current branch, pushed."""
    app.ok("trigger commands.create-tag")
    app.wait_until(lambda s: "Create Tag" in s.get("windows", []),
                   "the create-tag dialog")
    app.ok(f"type tag.name {name}")
    app.ok("click tag.push")
    app.ok("click Create")
    app.wait_until(lambda s: "Create Tag" not in s.get("windows", []),
                   "the create-tag dialog closed")


def tag_exists(repo: str, name: str) -> bool:
    try:
        git("rev-parse", "--verify", "-q", f"refs/tags/{name}", cwd=repo)
    except RuntimeError:
        return False
    return True


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    git("checkout", "-q", "-b", "other", cwd=work)
    with open(os.path.join(work, "a.txt"), "w") as fh:
        fh.write("other\n")
    git("commit", "-q", "-am", "other changes a.txt", cwd=work)
    git("checkout", "-q", "main", cwd=work)
    origin = os.path.join(scratch, "origin.git")
    git("clone", "-q", "--bare", work, origin, cwd=scratch)
    git("remote", "add", "origin", origin, cwd=work)
    git("config", "remote.origin.uploadpack", "sleep 4; git-upload-pack",
        cwd=work)
    # A dirty a.txt, which checking out `other` would overwrite.
    with open(os.path.join(work, "a.txt"), "w") as fh:
        fh.write("dirty\n")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main",
                   "repo open on main")

    app.ok("trigger act.fetch")
    app.wait_until(lambda s: s.get("statusMessage", "").startswith("Fetching"),
                   "the fetch running")

    # A checkout that fails meanwhile.
    app.ok("trigger commands.checkout-branch")
    app.wait_until(lambda s: "Checkout Branch" in s.get("windows", []),
                   "the checkout picker")
    combos = app.cmd("list-widgets")["combos"]
    picker = next((c for c in combos if "other" in c["items"]
                   and c.get("objectName") != "toolbar.branchCombo"), None)
    if picker is None:
        raise Failure(f"the checkout picker's combo not reported: {combos}")
    app.ok(f"select-item QComboBox:{picker['index']} other")
    app.ok("click OK")
    app.wait_until(lambda s: s.get("statusMessage", "").startswith("checkout failed"),
                   "the checkout's failure")
    if app.state().get("branch") != "main":
        raise Failure("the checkout went through; the test proves nothing")

    # The fetch goes through, and says so.
    state = app.wait_until(
        lambda s: s.get("statusMessage") == "Fetch complete."
        or s.get("opIndicator", "").startswith("✗"),
        "the fetch's outcome", timeout=30)
    if not state.get("opIndicator", "").startswith("✓"):
        raise Failure(f"the fetch, which went through, was reported as "
                      f"{state.get('opIndicator')!r} "
                      f"({state.get('statusMessage')!r})")
    git("rev-parse", "--verify", "-q", "refs/remotes/origin/other", cwd=work)

    # 2. Create tag with "Push tag after creation" while a fetch waits
    # on a server that never answers: the tag is created, and its push
    # is turned down with the usual note. (It ran right away, on the
    # GUI thread, alongside the fetch.) Pushes still go to origin.git.
    server = SilentServer()
    git("remote", "set-url", "origin", server.url, cwd=work)
    git("remote", "set-url", "--push", "origin", origin, cwd=work)
    app.ok("trigger act.fetch")
    if not server.connected.wait(20):
        raise Failure("the fetch never reached the server")
    create_tag(app, "v1")
    app.wait_until(lambda s: s.get("statusMessage")
                   == "Another remote operation is still running…",
                   "the tag's push turned down")
    if not tag_exists(work, "v1"):
        raise Failure("the tag wasn't created")
    if tag_exists(origin, "v1"):
        raise Failure("the tag was pushed alongside the running fetch")

    # 3. On its own, the tag's push goes through as a remote op.
    server.hang_up()
    app.wait_until(lambda s: s.get("opIndicator", "").startswith("✗"),
                   "the stalled fetch's failure")
    create_tag(app, "v2")
    app.wait_until(lambda s: s.get("statusMessage") == "Tag v2 pushed.",
                   "the tag pushed")
    if not tag_exists(origin, "v2"):
        raise Failure("the tag isn't on origin")

    rc = app.quit(timeout=15)
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc}")


if __name__ == "__main__":
    run(test)
