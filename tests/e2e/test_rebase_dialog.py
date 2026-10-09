#!/usr/bin/env python3
"""E2E: Commands > Rebase, and carrying a stopped rebase on.

The dialog's plan reached git as GIT_SEQUENCE_EDITOR, the todo text
where git expects a command, so git failed on every rebase and GitBolt
still said "Rebase complete.". Then, once rebases ran, one that stopped
could only be carried on from the dialog that started it, and only if
it was still open. Now:

1. a clean rebase from the dialog lands, its commits in order;
2. a conflicting one stops mid-rebase and offers the conflict resolver;
   the repository view's in-progress bar holds Continue back until the
   conflict is resolved, then finishes the rebase without running
   core.editor (GitBolt has no terminal for an editor; `git rebase
   --continue` used to open one for the commit message);
3. the plan's operations: a reword gets the message typed for it, an
   `edit` stops the rebase until the bar's Continue, and a squash
   folds a commit into the one below it;
4. the bar's Abort (confirmed) puts the branch back.
"""

from __future__ import annotations

import os
import shlex

from e2elib import App, Failure, git, make_repo, run


def commit(work: str, name: str, content: str, msg: str) -> None:
    with open(os.path.join(work, name), "w") as fh:
        fh.write(content)
    git("add", name, cwd=work)
    git("commit", "-q", "-m", msg, cwd=work)


def subjects(work: str) -> list[str]:
    return git("log", "--format=%s", cwd=work).splitlines()


def open_dialog(app: App, target: str) -> None:
    """Commands > Rebase, with `target` picked in the dialog."""
    app.ok("trigger commands.rebase")
    app.wait_until(lambda s: "Interactive Rebase" in s.get("windows", []),
                   "rebase dialog visible")
    combos = app.cmd("list-widgets")["combos"]
    combo = next((c for c in combos if "(select branch)" in c["items"]),
                 None)
    if combo is None:
        raise Failure(f"rebase dialog combo not reported: {combos}")
    sel = app.ok(f"select-item QComboBox:{combo['index']} {target}")
    if sel["text"] != target:
        raise Failure(f"select-item picked {sel['text']!r}")


def start_rebase(app: App) -> None:
    app.ok("click Rebase")
    app.wait_until(lambda s: "Interactive Rebase" not in s.get("windows", []),
                   "rebase dialog closed")


def buttons(app: App) -> dict[str, bool]:
    """Visible buttons: text -> enabled."""
    return {b["text"]: b["enabled"] for b in app.cmd("list-widgets")["buttons"]}


def answer(app: App, button: str) -> None:
    """Click `button` of a question box once it's up."""
    app.wait_until(lambda s: button in buttons(app), f"a {button} button")
    app.ok(f"click {button}")


def test(binary: str, scratch: str, apps: list) -> None:
    # An editor exported where the tests run (some IDEs and agents set
    # one) would outrank core.editor below and hide the bug.
    os.environ.pop("GIT_EDITOR", None)
    work = make_repo(os.path.join(scratch, "work"))
    marker = os.path.join(scratch, "editor-ran")
    git("config", "core.editor", f"touch {shlex.quote(marker)}; false",
        cwd=work)

    git("checkout", "-q", "-b", "feature", cwd=work)
    commit(work, "f1.txt", "1\n", "feature 1")
    commit(work, "f2.txt", "2\n", "feature 2")
    for branch in ("clash", "clash2"):
        git("checkout", "-q", "-b", branch, "main", cwd=work)
        commit(work, "a.txt", f"{branch}\n", branch)
    git("checkout", "-q", "main", cwd=work)
    commit(work, "a.txt", "main\n", "main change")
    git("checkout", "-q", "feature", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "feature",
                   "repo open on feature")

    # 1. A clean rebase.
    open_dialog(app, "main")
    start_rebase(app)
    want = ["feature 2", "feature 1", "main change", "base"]
    app.wait_until(lambda s: subjects(work) == want,
                   f"feature rebased onto main ({want})")
    app.wait_until(lambda s: s.get("repoState") == "none", "rebase finished")
    if "Continue" in buttons(app):
        raise Failure("the in-progress bar is up with no rebase going")

    # 2. A conflicting one: the dialog closes, the bar carries it on.
    git("checkout", "-q", "clash", cwd=work)
    app.wait_until(lambda s: s.get("branch") == "clash", "GitBolt on clash")
    open_dialog(app, "main")
    start_rebase(app)
    answer(app, "No")       # the offer to open the conflict resolver
    if app.state().get("repoState") != "rebase":
        raise Failure("the conflict didn't leave the rebase in progress")
    app.wait_until(lambda s: buttons(app).get("Resolve Conflicts") is True,
                   "the bar's Resolve Conflicts")
    if buttons(app).get("Continue") is not False:
        raise Failure(f"Continue offered over a conflict: {buttons(app)}")

    with open(os.path.join(work, "a.txt"), "w") as fh:
        fh.write("resolved\n")
    git("add", "a.txt", cwd=work)
    app.wait_until(lambda s: buttons(app).get("Continue") is True,
                   "Continue, once the conflict is resolved")
    app.ok("click Continue")
    want = ["clash", "main change", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want,
                   f"rebase continued to the end ({want})")
    if os.path.exists(marker):
        raise Failure("Continue ran core.editor")

    # 3. Reword the newest commit and stop at the one under it.
    git("checkout", "-q", "feature", cwd=work)
    app.wait_until(lambda s: s.get("branch") == "feature", "GitBolt on feature")
    open_dialog(app, "main")
    app.ok("select-row rebase.planList 0")
    app.ok("click Reword")
    app.wait_until(lambda s: "Reword Commit" in s.get("windows", []),
                   "the reword prompt")
    app.ok("type QPlainTextEdit:0 feature two\\n\\nnow with a body")
    app.ok("click OK")
    app.wait_until(lambda s: "Reword Commit" not in s.get("windows", []),
                   "the reword prompt closed")
    app.ok("select-row rebase.planList 1")
    app.ok("click Edit")
    start_rebase(app)
    app.wait_until(lambda s: s.get("repoState") == "rebase"
                   and buttons(app).get("Continue") is True,
                   "the rebase stopped at the edit, Continue on offer")
    if subjects(work)[0] != "feature 1":
        raise Failure(f"stopped somewhere else: {subjects(work)}")
    app.ok("click Continue")
    want = ["feature two", "feature 1", "main change", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want,
                   f"the reword landed ({want})")
    body = git("log", "-1", "--format=%b", cwd=work)
    if body != "now with a body":
        raise Failure(f"reworded body is {body!r}")

    #    …then squash it into "feature 1".
    open_dialog(app, "main")
    app.ok("select-row rebase.planList 0")
    app.ok("click Squash")
    start_rebase(app)
    want = ["feature 1", "main change", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want,
                   f"the squash landed ({want})")
    message = git("log", "-1", "--format=%B", cwd=work)
    if "feature two" not in message or not os.path.exists(
            os.path.join(work, "f2.txt")):
        raise Failure(f"the squash lost a commit: {message!r}")
    if os.path.exists(marker):
        raise Failure("a rebase step ran core.editor")

    # 4. Abort from the bar: the branch goes back.
    git("checkout", "-q", "clash2", cwd=work)
    app.wait_until(lambda s: s.get("branch") == "clash2", "GitBolt on clash2")
    before = git("rev-parse", "HEAD", cwd=work)
    open_dialog(app, "main")
    start_rebase(app)
    answer(app, "No")
    app.wait_until(lambda s: buttons(app).get("Abort") is True,
                   "the bar's Abort")
    app.ok("click Abort")
    answer(app, "Yes")
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and s.get("branch") == "clash2",
                   "the rebase aborted")
    if git("rev-parse", "HEAD", cwd=work) != before:
        raise Failure("Abort didn't put the branch back")
    app.wait_until(lambda s: "Abort" not in buttons(app), "the bar gone")


if __name__ == "__main__":
    run(test)
