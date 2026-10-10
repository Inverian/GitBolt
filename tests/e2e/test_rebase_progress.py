#!/usr/bin/env python3
"""E2E: a rebase around a merge, while git runs, and across a repository
switch.

1. A plan that no longer fits HEAD — another branch checked out at the
   same commit, a commit made since — is turned down with the dialog
   still open and the plan in it; picking the target again lists the
   new commit. (It used to run on the HEAD of the moment, dropping what
   the plan didn't list, or rewriting the other branch.) Then a branch
   that had main merged into it rebases onto main. The plan leaves the
   merge out, as git's own todo list does; it used to list it, and git,
   which can't pick a merge, failed on it at every Continue and Skip.
2. While the dialog's `git rebase` runs (held up here by a post-commit
   hook), the in-progress bar that the watcher's refresh brings up
   offers nothing. Its Continue, Skip and Abort used to be live, each a
   second git on top of the running one. Nor does the dialog, which
   stays usable, start another rebase meanwhile: it says so, and stays
   open.
3. A Continue still running when another repository is opened leaves
   that one alone. Its bar is live (it stayed disabled until the step
   finished), and the step's outcome, a conflict here, isn't reported
   there (GitBolt offered to resolve that repository's conflicts).
4. A Continue still running when its own repository is opened again
   (handed over by a second launch, as Recent or the dashboard would)
   keeps that repository's bar quiet, and its outcome is reported. It
   used to be dropped as another repository's, the bar turned back on
   under the running git.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import time

from e2elib import App, Failure, git, make_repo, run


def commit(work: str, name: str, content: str, msg: str) -> None:
    with open(os.path.join(work, name), "w") as fh:
        fh.write(content)
    git("add", name, cwd=work)
    git("commit", "-q", "-m", msg, cwd=work)


def subjects(work: str) -> list[str]:
    return git("log", "--format=%s", cwd=work).splitlines()


def pick_target(app: App, target: str) -> None:
    """Pick `target` in the open Rebase dialog's branch list."""
    combos = app.cmd("list-widgets")["combos"]
    combo = next((c for c in combos if "(select branch)" in c["items"]),
                 None)
    if combo is None:
        raise Failure(f"rebase dialog combo not reported: {combos}")
    sel = app.ok(f"select-item QComboBox:{combo['index']} {target}")
    if sel["text"] != target:
        raise Failure(f"select-item picked {sel['text']!r}")


def open_dialog(app: App, target: str) -> None:
    """Commands > Rebase, with `target` picked in the dialog."""
    app.ok("trigger commands.rebase")
    app.wait_until(lambda s: "Interactive Rebase" in s.get("windows", []),
                   "rebase dialog visible")
    pick_target(app, target)


def start_rebase(app: App) -> None:
    app.ok("click Rebase")
    app.wait_until(lambda s: "Interactive Rebase" not in s.get("windows", []),
                   "rebase dialog closed")


def buttons(app: App) -> dict[str, bool]:
    """Visible buttons: text -> enabled."""
    return {b["text"]: b["enabled"] for b in app.cmd("list-widgets")["buttons"]}


def turned_down(app: App, what: str) -> None:
    """Click Rebase, and see it turned down: a warning (its OK) over the
    dialog, which stays open once it's dismissed."""
    app.ok("click Rebase")
    app.wait_until(lambda s: "OK" in buttons(app), what)
    app.ok("click OK")
    app.wait_until(lambda s: "OK" not in buttons(app)
                   and "Interactive Rebase" in s.get("windows", []),
                   "the dialog still open")


def refs(work: str) -> str:
    return git("for-each-ref", "--format=%(refname) %(objectname)", cwd=work)


def refused(app: App, work: str) -> None:
    """turned_down(), and git left alone."""
    before = refs(work)
    turned_down(app, "the rebase turned down")
    time.sleep(1)       # a rebase, had one started, would have begun
    if app.state().get("repoState") != "none" or refs(work) != before:
        raise Failure("a rebase ran though it was turned down")


def checkout(app: App, work: str, branch: str) -> None:
    git("checkout", "-q", branch, cwd=work)
    app.wait_until(lambda s: s.get("branch") == branch, f"GitBolt on {branch}")


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    # Every commit waits while `hold` exists (a minute at most). Hooks
    # of our own, whatever core.hooksPath says globally.
    hold = os.path.join(scratch, "hold")
    hooks = os.path.join(scratch, "hooks")
    os.makedirs(hooks)
    hook = os.path.join(hooks, "post-commit")
    with open(hook, "w") as fh:
        fh.write("#!/bin/sh\nn=0\n"
                 f"while [ -f {shlex.quote(hold)} ] && [ $n -lt 600 ]; do\n"
                 "    sleep 0.1; n=$((n + 1))\ndone\n")
    os.chmod(hook, 0o755)
    git("config", "core.hooksPath", hooks, cwd=work)

    # feature: f1, main merged in, f2. slow: one commit. stack: three,
    # the last clashing with main's latest.
    git("checkout", "-q", "-b", "feature", cwd=work)
    commit(work, "f1.txt", "1\n", "feature 1")
    git("checkout", "-q", "main", cwd=work)
    commit(work, "m1.txt", "1\n", "main 1")
    git("checkout", "-q", "feature", cwd=work)
    git("merge", "-q", "--no-ff", "--no-edit", "main", cwd=work)
    commit(work, "f2.txt", "2\n", "feature 2")
    git("checkout", "-q", "-b", "slow", "main~1", cwd=work)
    commit(work, "s1.txt", "1\n", "slow 1")
    git("checkout", "-q", "-b", "stack", "main~1", cwd=work)
    commit(work, "t1.txt", "1\n", "stack 1")
    commit(work, "t2.txt", "2\n", "stack 2")
    commit(work, "a.txt", "stack\n", "stack 3")
    git("checkout", "-q", "main", cwd=work)
    commit(work, "a.txt", "main\n", "main 2")
    git("checkout", "-q", "feature", cwd=work)

    # Another repository, mid-merge with a conflict.
    other = make_repo(os.path.join(scratch, "other"))
    git("checkout", "-q", "-b", "x", cwd=other)
    commit(other, "a.txt", "x\n", "x")
    git("checkout", "-q", "main", cwd=other)
    commit(other, "a.txt", "main\n", "main")
    if subprocess.run(["git", "merge", "-q", "x"], cwd=other,
                      capture_output=True).returncode == 0:
        raise Failure("the other repository's merge didn't conflict")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "feature",
                   "repo open on feature")

    # 1. Turned down while the plan doesn't fit HEAD, the plan kept.
    open_dialog(app, "main")
    app.wait_view_rows("rebase.planList", 2)
    #    Another branch, at the same commit.
    git("checkout", "-q", "-b", "elsewhere", cwd=work)
    refused(app, work)
    app.wait_view_rows("rebase.planList", 2)
    git("checkout", "-q", "feature", cwd=work)
    git("branch", "-q", "-D", "elsewhere", cwd=work)
    #    A commit since: picking the same target again lists it.
    commit(work, "f3.txt", "3\n", "feature 3")
    refused(app, work)
    pick_target(app, "main")
    app.wait_view_rows("rebase.planList", 3)
    #    The merge stays out of the plan, and the rebase goes through.
    start_rebase(app)
    want = ["feature 3", "feature 2", "feature 1", "main 2", "main 1", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want,
                   f"feature rebased onto main in a line ({want})")

    # 2. Nothing on offer while git rebases.
    checkout(app, work, "slow")
    open(hold, "w").close()
    open_dialog(app, "main")
    start_rebase(app)
    app.wait_until(lambda s: "Abort" in buttons(app),
                   "the in-progress bar, while git rebases")
    live = sorted(b for b, enabled in buttons(app).items()
                  if b in ("Continue", "Skip", "Abort") and enabled)
    if live:
        raise Failure(f"the bar offers {live} while git is still rebasing")
    if app.state().get("repoState") != "rebase":
        raise Failure("the rebase didn't wait for the hook")
    #    The dialog, modeless, can still start another: it's turned
    #    down, staying open, and the bar stays quiet. (git would have
    #    refused it, and its failure re-enabled the bar.)
    open_dialog(app, "main")
    turned_down(app, "the second rebase turned down")
    live = sorted(b for b, enabled in buttons(app).items()
                  if b in ("Continue", "Skip", "Abort") and enabled)
    if live:
        raise Failure(f"the bar offers {live} while git is still rebasing")
    app.ok("click Cancel")
    app.wait_until(lambda s: "Interactive Rebase" not in s.get("windows", []),
                   "the dialog closed")
    os.remove(hold)
    want = ["slow 1", "main 2", "main 1", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want,
                   f"slow rebased onto main ({want})")
    app.wait_until(lambda s: "Abort" not in buttons(app), "the bar gone")

    # 3. Stop at "stack 1", then Continue: "stack 2" waits on the hold,
    #    and "stack 3" will stop on a conflict.
    checkout(app, work, "stack")
    open_dialog(app, "main")
    app.ok("select-row rebase.planList 2")
    app.ok("click Edit")
    start_rebase(app)
    app.wait_until(lambda s: s.get("repoState") == "rebase"
                   and buttons(app).get("Continue") is True,
                   "the rebase stopped at the edit")
    open(hold, "w").close()
    app.ok("click Continue")
    app.wait_until(lambda s: buttons(app).get("Continue") is False,
                   "Continue running")

    second = app.launch_second(other)
    if second.returncode != 0:
        raise Failure(f"hand-off of the other repository failed: "
                      f"rc={second.returncode} {second.stderr[-400:]!r}")
    app.wait_until(lambda s: s.get("repoPath", "").rstrip("/").endswith("other")
                   and s.get("repoState") == "merge",
                   "the other repository open")
    app.wait_until(lambda s: buttons(app).get("Abort") is True
                   and buttons(app).get("Resolve Conflicts") is True,
                   "the other repository's bar, live")

    os.remove(hold)
    deadline = time.monotonic() + 30
    while git("diff", "--name-only", "--diff-filter=U", cwd=work) != "a.txt":
        if time.monotonic() > deadline:
            raise Failure("the first repository's rebase never stopped "
                          "on its conflict")
        time.sleep(0.2)
    time.sleep(2)       # the step's outcome, had it been sent, is in
    # (The offer is a question box: Yes / No.)
    if "No" in buttons(app):
        raise Failure("the first repository's conflict was offered for "
                      "resolving in the other one")
    note = app.state().get("statusMessage", "")
    if "rebase" in note.lower():
        raise Failure(f"the first repository's step reported here: {note!r}")
    if buttons(app).get("Abort") is not True:
        raise Failure(f"the other repository's bar went dead: {buttons(app)}")

    # 4. Back on the first repository, its conflict resolved: Continue
    #    ("stack 3" waits on the hold), and the same repository handed
    #    over again meanwhile.
    def on_work(s: dict) -> bool:
        return s.get("repoPath", "").rstrip("/").endswith("work")

    second = app.launch_second(work)
    if second.returncode != 0:
        raise Failure(f"hand-off of the first repository failed: "
                      f"rc={second.returncode} {second.stderr[-400:]!r}")
    app.wait_until(lambda s: on_work(s) and s.get("repoState") == "rebase"
                   and buttons(app).get("Resolve Conflicts") is True,
                   "the first repository open, stopped on its conflict")
    with open(os.path.join(work, "a.txt"), "w") as fh:
        fh.write("both\n")
    git("add", "a.txt", cwd=work)
    app.wait_until(lambda s: buttons(app).get("Continue") is True,
                   "Continue on offer")
    open(hold, "w").close()
    app.ok("click Continue")
    app.wait_until(lambda s: buttons(app).get("Continue") is False,
                   "Continue running")

    second = app.launch_second(work)
    if second.returncode != 0:
        raise Failure(f"second hand-off of the first repository failed: "
                      f"rc={second.returncode} {second.stderr[-400:]!r}")
    app.wait_until(lambda s: on_work(s)
                   and s.get("statusMessage", "").startswith("Opened"),
                   "the first repository opened again")
    app.wait_until(lambda s: "Abort" in buttons(app),
                   "its bar, after the status refresh")
    for _ in range(5):
        live = sorted(b for b, enabled in buttons(app).items()
                      if b in ("Continue", "Skip", "Abort") and enabled)
        if live:
            raise Failure(f"opened again, the bar offers {live} while git "
                          "is still rebasing")
        time.sleep(0.3)
    if subjects(work)[0] != "stack 3" or app.state().get("repoState") != "rebase":
        raise Failure("the rebase didn't wait for the hook")

    os.remove(hold)
    app.wait_until(lambda s: s.get("statusMessage") == "Rebase complete.",
                   "the step's outcome, reported")
    want = ["stack 3", "stack 2", "stack 1", "main 2", "main 1", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want
                   and "Abort" not in buttons(app),
                   f"stack rebased onto main ({want}), the bar gone")


if __name__ == "__main__":
    run(test)
