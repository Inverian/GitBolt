#!/usr/bin/env python3
"""E2E: Commands > Create tag and Commands > Delete tag.

Delete tag used to fail for every tag: Repository::tags() reported the
full ref ("refs/tags/v1.0"), the picker showed that, and git_tag_delete
wants the short name. Create tag parsed its target as a full hex id, so
the dialog's default (a branch name) became the null OID and failed.
Drives both dialogs through the bridge and asserts on-disk refs.
"""

from __future__ import annotations

import os

from e2elib import App, Failure, git, make_repo, run


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    head = git("rev-parse", "HEAD", cwd=work)
    # One of each kind, so the picker has something ahead of the tag
    # we delete: landing on v1.0 proves the selection took.
    git("tag", "v0.1", cwd=work)
    git("tag", "-a", "v1.0", "-m", "release 1.0", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main",
                   "repo open on main")

    # 1. Create tag with the dialog's default target: the current
    #    branch, by name.
    app.ok("trigger commands.create-tag")
    app.wait_until(lambda s: "Create Tag" in s.get("windows", []),
                   "create-tag dialog visible")
    app.ok("type tag.name v2.0")
    app.ok("click Create")
    app.wait_until(lambda s: _tag_commit(work, "v2.0") == head,
                   "v2.0 to be created on main's tip")

    # 2. Delete tag: the picker lists short names, sorted.
    app.ok("trigger commands.delete-tag")
    app.wait_until(lambda s: "Delete Tag" in s.get("windows", []),
                   "delete-tag picker visible")
    combos = app.cmd("list-widgets")["combos"]
    picker = next((c for c in combos
                   if c.get("objectName") != "toolbar.branchCombo"
                   and any(i.endswith("v1.0") for i in c["items"])), None)
    if picker is None:
        raise Failure(f"delete-tag picker not reported: {combos}")
    if picker["items"] != ["v0.1", "v1.0", "v2.0"]:
        raise Failure(f"picker should list short tag names: {picker['items']}")
    if picker["current"] == "v1.0":
        raise Failure("picker preselected 'v1.0'; test can't prove "
                      "the selection took")
    sel = app.ok(f"select-item QComboBox:{picker['index']} v1.0")
    if sel["text"] != "v1.0":
        raise Failure(f"select-item picked {sel['text']!r}")
    app.ok("click OK")
    # The confirmation shares the picker's title, so wait for its Yes
    # button rather than the window title.
    app.wait_until(lambda s: any(b["text"] == "Yes" for b in
                                 app.cmd("list-widgets")["buttons"]),
                   "confirmation visible")
    app.ok("click Yes")
    app.wait_until(lambda s: "v1.0" not in _tags(work),
                   "v1.0 to be deleted")
    if _tags(work) != ["v0.1", "v2.0"]:
        raise Failure(f"only v1.0 should be gone: {_tags(work)}")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


def _tags(repo: str) -> list[str]:
    return git("tag", "--list", cwd=repo).split()


def _tag_commit(repo: str, tag: str) -> str:
    try:
        return git("rev-parse", "--verify", "-q", f"refs/tags/{tag}^{{commit}}",
                   cwd=repo)
    except RuntimeError:
        return ""


if __name__ == "__main__":
    run(test)
