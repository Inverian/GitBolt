#!/usr/bin/env python3
"""E2E: what a running local op keeps out, and where its outcome goes.

1. While a local op runs (Commands > Merge branches, held by its
   post-merge hook), nothing else starts git on the repository: the
   repository's plugins are off, the sidebar's New Branch is turned
   down with a note instead of creating the branch under the merge, and
   an open Commit dialog's Commit and staging are off. They all ran
   git or libgit2 alongside the merge.
2. A local op's outcome reaches the repository it ran on when that same
   repository is opened again meanwhile (a second launch handing its
   path over): a merge its hook turned down still says so. Any open
   used to drop it.
3. Plugins > Delete obsolete branches deletes nothing when another
   repository was opened while its picker or confirmation was up: the
   branches of those names there went, merged or not. It deletes with
   `git branch -d`, which keeps a branch that isn't merged.
4. The in-progress bar's Abort of a merge is a step like a rebase's:
   the bar is busy until `git merge --abort` is done (a second Abort
   ran a second git, which failed on index.lock and told the user to
   delete it), and then says "Merge aborted.".
"""

from __future__ import annotations

import os
import shlex
import time

from e2elib import App, Failure, git, make_repo, run


def commit(work: str, name: str, msg: str) -> None:
    with open(os.path.join(work, name), "w") as fh:
        fh.write(f"{msg}\n")
    git("add", name, cwd=work)
    git("commit", "-q", "-m", msg, cwd=work)


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


def boxes(app: App) -> list:
    return [w for w in app.cmd("list-widgets")["windows"]
            if w["class"] == "QMessageBox"]


def widget_enabled(app: App, name: str) -> bool:
    for group in app.cmd("list-widgets").values():
        if not isinstance(group, list):
            continue
        for w in group:
            if isinstance(w, dict) and w.get("objectName") == name and "enabled" in w:
                return bool(w["enabled"])
    raise Failure(f"{name} not reported by list-widgets")


def wait_for(path: str, what: str, timeout: float = 20) -> None:
    deadline = time.monotonic() + timeout
    while not os.path.exists(path):
        if time.monotonic() > deadline:
            raise Failure(f"{what} never happened")
        time.sleep(0.1)


def busy_gate(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "gate"))
    hold = os.path.join(scratch, "hold")
    started = os.path.join(scratch, "merge-hook-started")
    hooks = os.path.join(scratch, "gate-hooks")
    os.makedirs(hooks)
    with open(os.path.join(hooks, "post-merge"), "w") as fh:
        fh.write("#!/bin/sh\n"
                 f"touch {shlex.quote(started)}\n"
                 "n=0\n"
                 f"while [ -f {shlex.quote(hold)} ] && [ $n -lt 600 ]; do\n"
                 "    sleep 0.1; n=$((n + 1))\ndone\n")
    os.chmod(os.path.join(hooks, "post-merge"), 0o755)
    git("config", "core.hooksPath", hooks, cwd=work)
    git("checkout", "-q", "-b", "topic", cwd=work)
    commit(work, "t.txt", "topic")
    git("checkout", "-q", "main", cwd=work)
    with open(os.path.join(work, "c.txt"), "w") as fh:
        fh.write("c\n")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main",
                   "repo open on main")
    # The Commit dialog, open with a staged file, when the merge starts.
    app.ok("trigger act.commit")
    app.wait_view_rows("commit.unstagedList", 1)
    app.ok("click commit.stageAllBtn")
    app.wait_view_rows("commit.stagedList", 1)
    app.ok("type commit.message during-merge")
    if not app.action_enabled("plugins.gitflow"):
        raise Failure("GitFlow off before any op runs")

    open(hold, "w").close()
    merge(app, "topic")
    wait_for(started, "the merge's post-merge hook")
    for action in ("plugins.gitflow", "plugins.find-large-files",
                   "plugins.delete-obsolete-branches", "plugins.statistics"):
        if app.action_enabled(action):
            raise Failure(f"{action} enabled while git merges")
    if not app.action_enabled("plugins.plugin-manager"):
        raise Failure("Plugin Manager off while git merges")
    for name in ("commit.commitBtn", "commit.commitPushBtn", "commit.stageAllBtn",
                 "commit.unstageAllBtn"):
        if widget_enabled(app, name):
            raise Failure(f"{name} enabled while git merges")

    app.ok("trigger branchTree.newBranch")
    app.wait_until(lambda s: "New Branch" in s.get("windows", []), "the name box")
    app.ok("type QLineEdit:0 during-op")
    app.ok("click OK")
    app.wait_until(lambda s: "Another operation is still running"
                   in s.get("statusMessage", ""), "the refusal")
    if "during-op" in git("branch", "--format=%(refname:short)", cwd=work).split():
        raise Failure("the sidebar created a branch under the running merge")

    os.remove(hold)
    app.wait_until(lambda s: app.action_enabled("plugins.gitflow")
                   and widget_enabled(app, "commit.commitBtn"),
                   "the plugins and Commit back once the merge is in")
    if git("log", "-1", "--format=%s", cwd=work) == "during-merge":
        raise Failure("a commit ran under the merge")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def outcome_after_reopen(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "reopen"))
    hooks = os.path.join(scratch, "reopen-hooks")
    os.makedirs(hooks)
    started = os.path.join(scratch, "pre-merge-started")
    hook = os.path.join(hooks, "pre-merge-commit")
    with open(hook, "w") as fh:
        fh.write("#!/bin/sh\n"
                 f"touch {shlex.quote(started)}\n"
                 "sleep 3\necho 'hook says no' >&2\nexit 1\n")
    os.chmod(hook, 0o755)
    git("config", "core.hooksPath", hooks, cwd=work)
    git("checkout", "-q", "-b", "topic", cwd=work)
    commit(work, "t.txt", "topic")
    git("checkout", "-q", "main", cwd=work)
    commit(work, "m.txt", "main")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main",
                   "repo open on main")
    merge(app, "topic")
    wait_for(started, "the merge's pre-merge-commit hook")
    second = app.launch_second(work)
    if second.returncode != 0:
        raise Failure(f"the second launch exited with {second.returncode}")
    app.wait_until(lambda s: bool(boxes(app)), "the merge's failure", timeout=30)
    app.ok("click OK")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def obsolete_branches(binary: str, scratch: str, apps: list) -> None:
    a = make_repo(os.path.join(scratch, "repoA"))
    git("branch", "feature", cwd=a)                 # merged into A's HEAD
    b = make_repo(os.path.join(scratch, "repoB"))
    git("checkout", "-q", "-b", "feature", cwd=b)   # B's isn't merged
    commit(b, "work.txt", "unmerged work on feature")
    git("checkout", "-q", "main", cwd=b)
    git("branch", "done", cwd=b)                    # merged into B's HEAD

    app = App(binary, a)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 1, "A open")
    app.ok("trigger plugins.delete-obsolete-branches")
    app.wait_until(lambda s: "Delete Obsolete Branches" in s.get("windows", []),
                   "the picker")
    app.ok("select-row QListWidget:0 0")
    second = app.launch_second(b)
    if second.returncode != 0:
        raise Failure(f"the second launch exited with {second.returncode}")
    app.wait_until(lambda s: s.get("repoPath", "").rstrip("/").endswith("repoB")
                   and s.get("logRows", 0) >= 1, "B open under the picker")
    app.ok("click OK")
    app.wait_until(lambda s: bool(boxes(app)), "the confirmation")
    app.ok("click Yes")
    app.wait_until(lambda s: "no branch was deleted" in s.get("statusMessage", ""),
                   "the note that nothing was deleted")
    if not git("branch", "--list", "feature", cwd=b):
        raise Failure("B's unmerged feature branch was deleted")
    if not git("branch", "--list", "feature", cwd=a):
        raise Failure("A's feature branch was deleted after B was opened")

    # On B itself: the merged one goes; the unmerged one isn't listed.
    app.ok("trigger plugins.delete-obsolete-branches")
    app.wait_until(lambda s: "Delete Obsolete Branches" in s.get("windows", []),
                   "B's picker")
    app.ok("select-row QListWidget:0 0")
    app.ok("click OK")
    app.wait_until(lambda s: bool(boxes(app)), "the confirmation")
    app.ok("click Yes")
    deadline = time.monotonic() + 20
    while git("branch", "--list", "done", cwd=b):
        if time.monotonic() > deadline:
            raise Failure("B's merged branch was never deleted")
        time.sleep(0.2)
    if not git("branch", "--list", "feature", cwd=b):
        raise Failure("B's unmerged feature branch was deleted")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def bar_button(app: App, text: str):
    """Whether the in-progress bar's `text` button is enabled; None
    when it isn't shown."""
    for b in app.cmd("list-widgets")["buttons"]:
        if b.get("text") == text:
            return bool(b.get("enabled"))
    return None


def abort_is_busy(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "abort"), files=("f.txt",))
    git("checkout", "-q", "-b", "side", cwd=work)
    with open(os.path.join(work, "f.txt"), "w") as fh:
        fh.write("side\n")
    git("commit", "-qam", "side", cwd=work)
    git("checkout", "-q", "main", cwd=work)
    with open(os.path.join(work, "f.txt"), "w") as fh:
        fh.write("main\n")
    git("commit", "-qam", "main", cwd=work)
    try:
        git("merge", "-q", "side", cwd=work)
    except RuntimeError:
        pass                                        # the conflict
    # Putting f.txt back takes 3 s.
    git("config", "filter.slow.smudge", "sleep 3; cat", cwd=work)
    with open(os.path.join(work, ".git", "info", "attributes"), "w") as fh:
        fh.write("f.txt filter=slow\n")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoState") == "merge"
                   and bar_button(app, "Abort"), "the merge's bar")
    app.ok("click Abort")
    app.wait_until(lambda s: bool(boxes(app)), "the confirmation")
    app.ok("click Yes")
    app.wait_until(lambda s: bar_button(app, "Abort") is False,
                   "the bar busy while git aborts", timeout=5)
    if app.state().get("repoState") != "merge":
        raise Failure("the abort was over before the bar could be checked")
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and s.get("statusMessage") == "Merge aborted.", "the abort's outcome")
    if os.path.exists(os.path.join(work, ".git", "index.lock")):
        raise Failure("git left its index.lock behind")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def test(binary: str, scratch: str, apps: list) -> None:
    # git's own configuration only: a developer's core.hooksPath or
    # init.defaultBranch stays out of it.
    config = os.path.join(scratch, "gitconfig")
    with open(config, "w") as fh:
        fh.write("[init]\n\tdefaultBranch = main\n")
    os.environ["GIT_CONFIG_GLOBAL"] = config
    os.environ["GIT_CONFIG_NOSYSTEM"] = "1"
    busy_gate(binary, scratch, apps)
    outcome_after_reopen(binary, scratch, apps)
    obsolete_branches(binary, scratch, apps)
    abort_is_busy(binary, scratch, apps)


if __name__ == "__main__":
    run(test)
