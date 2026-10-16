#!/usr/bin/env python3
"""E2E: what a question or a modeless window does when things change.

1. Repository > Sparse Working Copy > Edit patterns applies nothing when
   another repository was opened (a second launch handing its path over)
   while the editor was up: `sparse-checkout set` ran on that one, and
   its other files went.
2. Commands > Clean working directory cleans nothing when another
   repository was opened while its question was up: that one's untracked
   files went.
3. The Manage Stashes window stays usable while a local op runs (here a
   hard reset held by a slow smudge filter): its buttons are off until
   the op is done. A Pop under the reset wrote the stash's files, failed
   on index.lock and kept the stash, "failed".
4. The Submodules window's Deinit while a local op runs is turned away
   before its question, not after it, and its row actions are off.
5. The in-progress bar's Skip at a rebase's `edit` stop with changes
   made there (an edit, a new file staged) refuses, saying they would be
   discarded: git's --skip resets the work tree first, and they were
   gone for good, with "Rebase complete.".
"""

from __future__ import annotations

import os
import shlex
import subprocess
import time

from e2elib import App, Failure, git, make_repo, on_screen, run


def boxes(app: App) -> list:
    return [w for w in app.cmd("list-widgets")["windows"]
            if w["class"] == "QMessageBox"]


def button(app: App, name: str):
    """The button named or labelled `name`: whether it's enabled, None
    when it isn't shown."""
    for b in app.cmd("list-widgets")["buttons"]:
        if b.get("objectName") == name or b.get("text") == name:
            return bool(b.get("enabled"))
    return None


def wait_for_window(app: App, title: str) -> None:
    app.wait_until(lambda s: title in s.get("windows", []), f"the {title} window")


def hand_over(app: App, repo: str, name: str) -> None:
    """A second launch hands `repo` over; it's on screen once this
    returns."""
    second = app.launch_second(repo)
    if second.returncode != 0:
        raise Failure(f"the second launch exited with {second.returncode}")
    app.wait_until(on_screen(name), f"{name} open under the dialog")


def sparse_patterns(binary: str, scratch: str, apps: list) -> None:
    a = make_repo(os.path.join(scratch, "sparseA"))
    b = make_repo(os.path.join(scratch, "sparseB"))
    os.makedirs(os.path.join(b, "src"))
    with open(os.path.join(b, "src", "main.c"), "w") as fh:
        fh.write("int main;\n")
    git("add", "-A", cwd=b)
    git("commit", "-qm", "src", cwd=b)

    app = App(binary, a)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(on_screen("sparseA"), "A open")
    app.ok("trigger repository.sparse-working-copy.edit-patterns")
    wait_for_window(app, "Sparse Checkout Patterns")
    app.ok("type QPlainTextEdit:0 keep\\n")
    hand_over(app, b, "sparseB")
    app.ok("click Save")
    app.wait_until(lambda s: "patterns were not applied" in s.get("statusMessage", ""),
                   "the note that nothing was applied")
    time.sleep(1)
    for repo in (a, b):
        try:
            sparse = git("config", "core.sparseCheckout", cwd=repo)
        except RuntimeError:
            sparse = ""
        if sparse:
            raise Failure(f"sparse checkout turned on in {os.path.basename(repo)}")
    if not os.path.exists(os.path.join(b, "src", "main.c")):
        raise Failure("B's src/main.c went")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def clean_after_switch(binary: str, scratch: str, apps: list) -> None:
    a = make_repo(os.path.join(scratch, "cleanA"))
    b = make_repo(os.path.join(scratch, "cleanB"))
    keep = os.path.join(b, "untracked-work.txt")
    with open(keep, "w") as fh:
        fh.write("not yet added\n")

    app = App(binary, a)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(on_screen("cleanA"), "A open")
    app.ok("trigger commands.clean-working-directory")
    app.wait_until(lambda s: bool(boxes(app)), "the question")
    hand_over(app, b, "cleanB")
    app.ok("click Yes")
    app.wait_until(lambda s: "nothing was cleaned" in s.get("statusMessage", ""),
                   "the note that nothing was cleaned")
    time.sleep(1)
    if not os.path.exists(keep):
        raise Failure("B's untracked file was cleaned away")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def stash_during_local_op(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "stash"), files=("a.txt", "other.txt"))
    hold = os.path.join(scratch, "hold-smudge")
    # Checking big.dat out waits while `hold` is there.
    git("config", "filter.slow.smudge",
        f"while [ -f {shlex.quote(hold)} ]; do sleep 0.1; done; cat", cwd=work)
    git("config", "filter.slow.clean", "cat", cwd=work)
    with open(os.path.join(work, ".gitattributes"), "w") as fh:
        fh.write("big.dat filter=slow\n")
    with open(os.path.join(work, "big.dat"), "w") as fh:
        fh.write("big\n")
    git("add", "-A", cwd=work)
    git("commit", "-qm", "big", cwd=work)
    with open(os.path.join(work, "other.txt"), "w") as fh:
        fh.write("stashed work\n")
    git("stash", "-q", cwd=work)
    with open(os.path.join(work, "big.dat"), "w") as fh:
        fh.write("changed\n")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("logRows", 0) >= 1, "repo open")
    app.ok("trigger act.stash")
    wait_for_window(app, "Manage Stashes")
    app.ok("select-row QListWidget:0 0")
    if not button(app, "stash.popBtn"):
        raise Failure("Pop off before any op runs")

    open(hold, "w").close()
    app.ok("trigger commands.reset-changes")
    app.wait_until(lambda s: "Reset Changes" in s.get("windows", []), "the reset picker")
    combos = app.cmd("list-widgets")["combos"]
    picker = next((c for c in combos
                   if any(i.startswith("hard") for i in c["items"])), None)
    if picker is None:
        raise Failure(f"the reset picker's combo not reported: {combos}")
    hard = next(i for i in picker["items"] if i.startswith("hard"))
    app.ok(f"select-item QComboBox:{picker['index']} {hard}")
    app.ok("click OK")
    app.wait_until(lambda s: bool(boxes(app)), "the hard reset's question")
    app.ok("click Yes")
    app.wait_until(lambda s: "Resetting" in s.get("opIndicator", ""), "the reset running")
    for name in ("stash.applyBtn", "stash.popBtn", "stash.dropBtn", "stash.newBtn"):
        if button(app, name):
            raise Failure(f"{name} enabled while git resets")
    if app.cmd("click stash.popBtn").get("ok"):
        raise Failure("Pop ran while git resets")

    os.remove(hold)
    app.wait_until(lambda s: s.get("opIndicator", "") == "", "the reset done", timeout=30)
    app.wait_until(lambda s: button(app, "stash.popBtn") is True, "Pop back once it's done")
    if len(git("stash", "list", cwd=work).splitlines()) != 1:
        raise Failure("the stash went")
    with open(os.path.join(work, "other.txt")) as fh:
        if fh.read() != "other.txt\n":
            raise Failure("the stash's change reached the work tree under the reset")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def deinit_during_local_op(binary: str, scratch: str, apps: list) -> None:
    sub = make_repo(os.path.join(scratch, "libsub-src"))
    work = make_repo(os.path.join(scratch, "subs"))
    git("-c", "protocol.file.allow=always", "submodule", "add", "-q", sub, "libsub",
        cwd=work)
    git("commit", "-qm", "add libsub", cwd=work)
    hold = os.path.join(scratch, "hold-merge")
    started = os.path.join(scratch, "merge-started")
    hooks = os.path.join(scratch, "subs-hooks")
    os.makedirs(hooks)
    with open(os.path.join(hooks, "post-merge"), "w") as fh:
        fh.write("#!/bin/sh\n"
                 f"touch {shlex.quote(started)}\n"
                 f"while [ -f {shlex.quote(hold)} ]; do sleep 0.1; done\n")
    os.chmod(os.path.join(hooks, "post-merge"), 0o755)
    git("config", "core.hooksPath", hooks, cwd=work)
    git("checkout", "-q", "-b", "topic", cwd=work)
    with open(os.path.join(work, "t.txt"), "w") as fh:
        fh.write("t\n")
    git("add", "t.txt", cwd=work)
    git("commit", "-qm", "topic", cwd=work)
    git("checkout", "-q", "main", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoOpen") and s.get("branch") == "main", "repo open")
    app.ok("trigger repository.manage-submodules")
    wait_for_window(app, "Submodules")
    app.wait_until(lambda s: any(v.get("rows") for v in app.cmd("list-widgets")["views"]
                                 if v["class"] == "QTableView"), "the submodule listed")
    app.ok("select-row QTableView:0 0")
    open(hold, "w").close()
    app.ok("trigger commands.merge-branches")
    app.wait_until(lambda s: "Merge Branch" in s.get("windows", []), "the merge picker")
    combos = app.cmd("list-widgets")["combos"]
    picker = next((c for c in combos if "topic" in c["items"]
                   and c.get("objectName") != "toolbar.branchCombo"), None)
    if picker is None:
        raise Failure(f"the merge picker's combo not reported: {combos}")
    app.ok(f"select-item QComboBox:{picker['index']} topic")
    app.ok("click OK")
    deadline = time.monotonic() + 20
    while not os.path.exists(started):
        if time.monotonic() > deadline:
            raise Failure("the merge's post-merge hook never ran")
        time.sleep(0.1)
    app.ok("trigger submodule.deinit")
    time.sleep(0.5)
    if boxes(app):
        raise Failure("Deinit asked its question while git merges")
    if "still running" not in app.state().get("statusMessage", ""):
        raise Failure("Deinit wasn't turned away while git merges")
    os.remove(hold)
    app.wait_until(lambda s: s.get("opIndicator", "") == "", "the merge done", timeout=30)
    status = git("submodule", "status", cwd=work)
    if status.startswith("-"):
        raise Failure(f"the submodule was deinitialized: {status}")
    if app.quit() != 0:
        raise Failure("GitBolt didn't exit cleanly")


def skip_keeps_work(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "edit"), files=("f1.txt",))
    for n in (2, 3):
        with open(os.path.join(work, f"f{n}.txt"), "w") as fh:
            fh.write(f"{n}\n")
        git("add", f"f{n}.txt", cwd=work)
        git("commit", "-qm", f"c{n}", cwd=work)
    # Stopped at an `edit` of c2.
    env = dict(os.environ, GIT_SEQUENCE_EDITOR="sed -i.bak '1s/^pick/edit/'")
    out = subprocess.run(["git", "rebase", "-i", "HEAD~2"], cwd=work, env=env,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise Failure(f"couldn't stop at an edit: {out.stderr}")
    with open(os.path.join(work, "f1.txt"), "w") as fh:
        fh.write("my edit\n")
    with open(os.path.join(work, "f9.txt"), "w") as fh:
        fh.write("new\n")
    git("add", "f9.txt", cwd=work)

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoState") == "rebase" and button(app, "Skip"),
                   "the rebase's bar")
    app.ok("click Skip")
    app.wait_until(lambda s: bool(boxes(app)), "Skip's answer")
    if button(app, "Yes") is not None:
        raise Failure("Skip asked to go ahead with changes it would discard")
    app.ok("click OK")
    time.sleep(1)
    if app.state().get("repoState") != "rebase":
        raise Failure("the rebase went on")
    status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"],
                            cwd=work, capture_output=True, text=True).stdout
    if status.splitlines() != [" M f1.txt", "A  f9.txt"]:
        raise Failure(f"the changes made at the stop went: {status!r}")
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
    sparse_patterns(binary, scratch, apps)
    clean_after_switch(binary, scratch, apps)
    stash_during_local_op(binary, scratch, apps)
    deinit_during_local_op(binary, scratch, apps)
    skip_keeps_work(binary, scratch, apps)


if __name__ == "__main__":
    run(test)
