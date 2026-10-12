#!/usr/bin/env python3
"""E2E: quitting while a rebase step runs.

A rebase step (the Rebase dialog's rebase, or Continue, Skip and Abort
on the in-progress bar) runs git on a worker that nothing stopped:
quitting while its hook or `exec` ran left GitBolt running, windowless,
until the step ended, and a second launch meanwhile found nobody to
hand its path to and gave up. Quitting now stops git and what it runs,
as Ctrl-C would, without asking: GitBolt exits promptly, the rebase
stays in progress, and the next launch carries it on.

Continue runs into an `exec` that notes its pid and waits while `hold`
exists, so the step runs for as long as the test likes.
"""

from __future__ import annotations

import os
import shlex
import subprocess
import time

from e2elib import App, Failure, git, make_repo, run


def buttons(app: App) -> dict[str, bool]:
    """Visible buttons: text -> enabled."""
    return {b["text"]: b["enabled"] for b in app.cmd("list-widgets")["buttons"]}


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test(binary: str, scratch: str, apps: list) -> None:
    work = make_repo(os.path.join(scratch, "work"))
    with open(os.path.join(work, "b.txt"), "w") as fh:
        fh.write("b\n")
    git("add", "b.txt", cwd=work)
    git("commit", "-q", "-m", "second", cwd=work)

    # The rebase stops at `exec false`; Continue runs the next exec,
    # which waits while `hold` exists (a minute at most).
    hold = os.path.join(scratch, "hold")
    pid_file = os.path.join(scratch, "exec.pid")
    waiter = (f"echo $$ > {shlex.quote(pid_file)}; n=0; "
              f"while [ -f {shlex.quote(hold)} ] && [ $n -lt 600 ]; do "
              "sleep 0.1; n=$((n + 1)); done")
    if subprocess.run(["git", "rebase", "--exec", "false", "--exec", waiter,
                       "HEAD~1"], cwd=work, capture_output=True).returncode == 0:
        raise Failure("the rebase didn't stop at `exec false`")
    rebase_dir = os.path.join(work, ".git", "rebase-merge")

    app = App(binary, work)
    apps.append(app)
    app.wait_bridge()
    app.wait_until(lambda s: s.get("repoState") == "rebase"
                   and buttons(app).get("Continue") is True,
                   "the rebase on offer to continue")

    open(hold, "w").close()
    app.ok("click Continue")
    deadline = time.monotonic() + 20
    while not os.path.exists(pid_file):
        if time.monotonic() > deadline:
            raise Failure("Continue never got to the exec that waits")
        time.sleep(0.1)
    with open(pid_file) as fh:
        pid = int(fh.read().strip() or 0)
    if pid <= 0 or not alive(pid):
        raise Failure("the exec that waits isn't running")

    start = time.monotonic()
    try:
        rc = app.quit(timeout=10)
    except subprocess.TimeoutExpired:
        raise Failure("GitBolt was still running 10 s after quit: it "
                      "waited for the rebase step") from None
    if rc != 0:
        raise Failure(f"GitBolt exited with {rc} after quitting during "
                      "a rebase step")
    print(f"quit took {time.monotonic() - start:.1f} s")
    deadline = time.monotonic() + 3
    while alive(pid):
        if time.monotonic() > deadline:
            raise Failure("the rebase's exec ran on after GitBolt quit")
        time.sleep(0.1)
    if not os.path.isdir(rebase_dir):
        raise Failure("the rebase isn't in progress any more")
    if os.path.exists(os.path.join(work, ".git", "index.lock")):
        raise Failure("git left its index.lock behind")

    # The next launch carries it on.
    os.remove(hold)
    again = App(binary, work, label="relaunch")
    apps.append(again)
    again.wait_bridge()
    again.wait_until(lambda s: s.get("repoState") == "rebase"
                     and buttons(again).get("Continue") is True,
                     "the rebase in progress, on offer to continue")
    again.ok("click Continue")
    again.wait_until(lambda s: s.get("repoState") == "none",
                     "the rebase done")
    rc = again.quit()
    if rc != 0:
        raise Failure(f"the relaunch exited with {rc}")


if __name__ == "__main__":
    run(test)
