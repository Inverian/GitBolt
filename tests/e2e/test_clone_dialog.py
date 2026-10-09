#!/usr/bin/env python3
"""E2E: File > Clone Repository, driven through the dialog's widgets.

Clone runs `git clone` through the CLI (GitProcess::clone), like push /
pull / fetch. First a real clone: type a file:// URL and a destination,
click Clone, and the app opens the new repository — asserted with git
on disk. Then a clone from a "server" that never answers (ext:: running
a stub that stalls), cancelled from the dialog: the partial destination
is removed, the stub — a grandchild of git, where git-remote-https and
ssh live in a real clone — is gone too, and the dialog stays open,
ready for a retry. Then a clone whose fetch completes but whose
checkout fails: git keeps that repository, and the dialog offers to
open it — until the destination is edited, which makes the next
click a fresh clone again. Last, a clone checked out in full whose
post-checkout hook fails: git keeps that one too, and so does the
dialog, with the same offer.
"""

from __future__ import annotations

import os
import signal
import sys
import time

from e2elib import App, Failure, git, make_repo, run

# Records its pid (write-then-rename, so a reader never sees half of
# it), then sleeps far longer than the test runs.
STALL_STUB = """\
import os, sys, time
pid_file = sys.argv[1]
with open(pid_file + ".tmp", "w") as fh:
    fh.write(str(os.getpid()))
os.rename(pid_file + ".tmp", pid_file)
time.sleep(60)
"""

HOOK = """\
#!/bin/sh
case "$(pwd)" in
*/hooked) echo "post-checkout: refused" >&2; exit 3 ;;
esac
"""


def test(binary: str, scratch: str, apps: list) -> None:
    src = make_repo(os.path.join(scratch, "src"), files=("a.txt", "b.txt"))
    head = git("rev-parse", "HEAD", cwd=src)
    origin = os.path.join(scratch, "origin.git")
    git("clone", "-q", "--bare", src, origin, cwd=scratch)
    url = "file://" + origin

    stub = os.path.join(scratch, "stall.py")
    with open(stub, "w") as fh:
        fh.write(STALL_STUB)
    pid_file = os.path.join(scratch, "stall.pid")
    stall_url = "ext::" + " ".join(
        _ext_escape(a) for a in (sys.executable, stub, pid_file))

    # A repository whose files go through a smudge filter that the
    # app's git is told is required and that always fails: a clone of
    # it fetches everything, then fails its checkout.
    broken_src = os.path.join(scratch, "broken-src")
    os.makedirs(broken_src)
    with open(os.path.join(broken_src, ".gitattributes"), "w") as fh:
        fh.write("*.txt filter=broken\n")
    make_repo(broken_src, files=("a.txt",))
    broken_head = git("rev-parse", "HEAD", cwd=broken_src)
    broken_origin = os.path.join(scratch, "broken.git")
    git("clone", "-q", "--bare", broken_src, broken_origin, cwd=scratch)
    broken_url = "file://" + broken_origin

    # A post-checkout hook, as core.hooksPath or init.templateDir can
    # give every clone, that fails in a clone named "hooked" and passes
    # everywhere else.
    hooks = os.path.join(scratch, "hooks")
    os.makedirs(hooks)
    hook = os.path.join(hooks, "post-checkout")
    with open(hook, "w") as fh:
        fh.write(HOOK)
    os.chmod(hook, 0o755)

    # git keeps ext:: off unless allowed. Only the app's git sees this,
    # the filter config and the hooks.
    app_env = {
        "GIT_ALLOW_PROTOCOL": "file:ext",
        "GIT_CONFIG_COUNT": "3",
        "GIT_CONFIG_KEY_0": "filter.broken.smudge",
        "GIT_CONFIG_VALUE_0": "false",
        "GIT_CONFIG_KEY_1": "filter.broken.required",
        "GIT_CONFIG_VALUE_1": "true",
        "GIT_CONFIG_KEY_2": "core.hooksPath",
        "GIT_CONFIG_VALUE_2": hooks,
    }
    os.environ.update(app_env)
    try:
        app = App(binary, config_home=os.path.join(scratch, "config"))
    finally:
        for name in app_env:
            del os.environ[name]
    apps.append(app)
    app.wait_bridge()

    # 1. Clone, and the dialog hands the new repo to the main window.
    dest = os.path.join(scratch, "cloned")
    _open_dialog(app)
    app.ok(f"type clone.url {url}")
    app.ok(f"type clone.path {dest}")
    app.ok("click Clone")
    state = app.wait_until(
        lambda s: s.get("repoOpen") and _same_path(s.get("repoPath", ""), dest),
        "the cloned repository to open", timeout=30)
    if "Clone Repository" in state.get("windows", []):
        raise Failure("dialog still open after a successful clone")
    if git("rev-parse", "HEAD", cwd=dest) != head:
        raise Failure("clone HEAD differs from the source")
    if git("remote", "get-url", "origin", cwd=dest) != url:
        raise Failure("origin does not point at the cloned URL")

    # 2. Cancel a clone that is stuck waiting for the server.
    stalled = os.path.join(scratch, "stalled")
    pid = 0
    try:
        _open_dialog(app)
        app.ok(f"type clone.url {stall_url}")
        app.ok(f"type clone.path {stalled}")
        app.ok("click Clone")
        pid = _wait_for_pid(pid_file)
        if not os.path.isdir(stalled):
            raise Failure("git never created the destination")

        app.ok("click Cancel")
        _wait(lambda: not os.path.exists(stalled),
              "the partial clone to be removed")
        _wait(lambda: not _alive(pid), "the stalled transport to be killed")
        app.wait_until(lambda s: _button_enabled(app, "Clone"),
                       "Clone to be enabled again")
        if "Clone Repository" not in app.state().get("windows", []):
            raise Failure("cancelling the clone closed the dialog")

        # Not cloning any more: Cancel now just closes the dialog.
        app.ok("click Cancel")
        app.wait_until(
            lambda s: "Clone Repository" not in s.get("windows", []),
            "the dialog to close")
    finally:
        if pid and _alive(pid):
            os.kill(pid, signal.SIGKILL)

    # 3. Fetched, but the checkout failed: git keeps the repository and
    #    the dialog offers it. Editing the path withdraws the offer.
    kept = os.path.join(scratch, "kept")
    _open_dialog(app)
    app.ok(f"type clone.url {broken_url}")
    app.ok(f"type clone.path {kept}")
    app.ok("click Clone")
    app.wait_until(lambda s: _button_enabled(app, "Open Repository"),
                   "Open Repository to be offered", timeout=30)
    if git("rev-parse", "HEAD", cwd=kept) != broken_head:
        raise Failure("the kept clone's HEAD differs from the source")
    app.ok(f"type clone.path {kept}-2")
    app.wait_until(lambda s: _button_enabled(app, "Clone"),
                   "editing the path to bring Clone back")
    if not os.path.isdir(os.path.join(kept, ".git")):
        raise Failure("withdrawing the offer removed the kept clone")

    # Cloned again, this time opened from the dialog.
    kept = kept + "-2"
    app.ok("click Clone")
    app.wait_until(lambda s: _button_enabled(app, "Open Repository"),
                   "Open Repository to be offered again", timeout=30)
    app.ok("click Open Repository")
    state = app.wait_until(
        lambda s: s.get("repoOpen") and _same_path(s.get("repoPath", ""), kept),
        "the kept repository to open", timeout=30)
    if "Clone Repository" in state.get("windows", []):
        raise Failure("dialog still open after opening the kept clone")

    # 4. Cloned and checked out, then the post-checkout hook failed: git
    #    keeps the repository, whole, and the dialog offers it as well.
    hooked = os.path.join(scratch, "hooked")
    _open_dialog(app)
    app.ok(f"type clone.url {url}")
    app.ok(f"type clone.path {hooked}")
    app.ok("click Clone")
    app.wait_until(lambda s: _button_enabled(app, "Open Repository"),
                   "Open Repository to be offered after the hook failed",
                   timeout=30)
    if git("rev-parse", "HEAD", cwd=hooked) != head:
        raise Failure("the hooked clone's HEAD differs from the source")
    if git("status", "--porcelain", cwd=hooked):
        raise Failure("the hooked clone's work tree is not checked out")
    app.ok("click Open Repository")
    state = app.wait_until(
        lambda s: s.get("repoOpen") and _same_path(s.get("repoPath", ""), hooked),
        "the hooked repository to open", timeout=30)
    if "Clone Repository" in state.get("windows", []):
        raise Failure("dialog still open after opening the hooked clone")

    rc = app.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{app.tail()}")


def _open_dialog(app: App) -> None:
    app.ok("trigger file.clone-repository")
    app.wait_until(lambda s: "Clone Repository" in s.get("windows", []),
                   "clone dialog visible")


def _button_enabled(app: App, text: str) -> bool:
    return any(b["text"] == text and b["enabled"]
               for b in app.cmd("list-widgets")["buttons"])


def _ext_escape(arg: str) -> str:
    # In an ext:: command '%' escapes and a space separates arguments.
    return arg.replace("%", "%%").replace(" ", "% ")


def _same_path(a: str, b: str) -> bool:
    # dump-state reports libgit2's workdir (trailing slash), and the
    # temp dir sits behind a symlink on macOS (/var -> /private/var).
    return bool(a) and os.path.realpath(a.rstrip("/")) == os.path.realpath(b)


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def _wait(pred, what: str, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pred():
            return
        time.sleep(0.1)
    raise Failure(f"timed out waiting for {what}")


def _wait_for_pid(path: str, timeout: float = 20.0) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open(path) as fh:
                return int(fh.read())
        except (OSError, ValueError):
            time.sleep(0.1)
    raise Failure("the stalled transport never started")


if __name__ == "__main__":
    run(test)
