#!/usr/bin/env python3
"""E2E: Commands > Rebase, which never ran a rebase.

The dialog's plan reached git as GIT_SEQUENCE_EDITOR, the todo text
where git expects a command, so git failed on every rebase and GitBolt
still said "Rebase complete.". Now:

1. a clean rebase from the dialog lands, its commits in order;
2. a conflicting one stops mid-rebase and offers the conflict resolver,
   and once the conflict is resolved the dialog's Continue finishes it
   without running core.editor (GitBolt has no terminal for an editor;
   `git rebase --continue` used to open one for the commit message).
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


def rebase_onto(app: App, target: str, button: str) -> None:
    """Commands > Rebase, pick `target` in the dialog, press `button`."""
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
    app.ok(f"click {button}")


def has_button(app: App, text: str) -> bool:
    return any(b["text"] == text for b in app.cmd("list-widgets")["buttons"])


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
    git("checkout", "-q", "-b", "clash", "main", cwd=work)
    commit(work, "a.txt", "clash\n", "clash")
    git("checkout", "-q", "main", cwd=work)
    commit(work, "a.txt", "main\n", "main change")
    git("checkout", "-q", "feature", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "feature",
                   "repo open on feature")

    # 1. A clean rebase, with the dialog's own Rebase button.
    rebase_onto(app, "main", "Rebase")
    want = ["feature 2", "feature 1", "main change", "base"]
    app.wait_until(lambda s: subjects(work) == want,
                   f"feature rebased onto main ({want})")
    app.wait_until(lambda s: s.get("repoState") == "none", "rebase finished")

    # 2. A conflicting one, from the embedded list's Start Rebase, which
    #    keeps the dialog (and its Continue button) open.
    git("checkout", "-q", "clash", cwd=work)
    app.wait_until(lambda s: s.get("branch") == "clash", "GitBolt on clash")
    rebase_onto(app, "main", "Start Rebase")
    app.wait_until(lambda s: has_button(app, "No"),
                   "the offer to open the conflict resolver")
    if app.state().get("repoState") != "rebase":
        raise Failure("the conflict didn't leave the rebase in progress")
    app.ok("click No")

    with open(os.path.join(work, "a.txt"), "w") as fh:
        fh.write("resolved\n")
    git("add", "a.txt", cwd=work)
    app.ok("click Continue")
    want = ["clash", "main change", "base"]
    app.wait_until(lambda s: s.get("repoState") == "none"
                   and subjects(work) == want,
                   f"rebase continued to the end ({want})")
    if os.path.exists(marker):
        raise Failure("Continue ran core.editor")


if __name__ == "__main__":
    run(test)
