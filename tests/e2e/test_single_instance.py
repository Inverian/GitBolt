#!/usr/bin/env python3
"""E2E: a second launch hands its repo path over and exits 0.

This is how "Open in GitBolt" from a file manager reaches an already-
open window. The second process must exit cleanly (not show a modal
warning) and the first instance must switch to the forwarded repo. A
bare relaunch (no path, e.g. a launcher click) must be answered too.
And when the first instance is alive but busy — its GUI thread stuck
in a long synchronous git call, so the hand-off goes unanswered — the
launch waits for as long as that instance lives, with a note after
10 s (and, where someone can see it, a window to cancel the wait), not
an error dialog and not a rival window: the owner opens the path once
it is free, and the launch exits 0. If the owner dies busy instead, the
launch takes its place and opens the path itself. It used to give up
after 10 s and exit 0, leaving the path with an owner that might never
read it.
"""

from __future__ import annotations

import os
import signal
import time

from e2elib import App, Failure, make_repo, run


def _on(name: str):
    return lambda s: s.get("repoPath", "").rstrip("/").endswith(name)


def test(binary: str, scratch: str, apps: list) -> None:
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))

    first = App(binary, repo_a, label="first instance")
    dying = App(binary, repo_a, label="owner that dies busy")
    apps += [first, dying]
    for owner in (first, dying):
        owner.wait_bridge()
        owner.wait_until(_on("repo-a"), "repo A open")

    # Same instance namespace => the lock is taken and the new process
    # hands off instead of starting a window.
    second = first.launch_second(repo_b)
    if second.returncode != 0:
        raise Failure("second instance should forward and exit 0, got "
                      f"rc={second.returncode}: {second.stdout[-400:]}"
                      f"{second.stderr[-400:]}")
    first.wait_until(_on("repo-b"),
                     "first instance to switch to forwarded repo B")

    # No path: the running instance still answers (only raising its
    # window), so the relaunch exits 0 and the open repo stays.
    bare = first.launch_second()
    if bare.returncode != 0:
        raise Failure(f"bare relaunch should exit 0, got "
                      f"rc={bare.returncode}: {bare.stderr[-400:]}")
    if not _on("repo-b")(first.state()):
        raise Failure(f"bare relaunch changed the repo: {first.state()}")

    # Busy owners (stopped here): the hand-off connects and its line is
    # sent, but no receipt comes while they are stopped.
    os.kill(first.proc.pid, signal.SIGSTOP)
    os.kill(dying.proc.pid, signal.SIGSTOP)
    busy = first.start_second(repo_a)
    try:
        taker = App(binary, repo_b, instance_name=dying.instance_name,
                    runtime_dir=dying.runtime_dir,
                    label="launch waiting on the owner that dies")
        apps.append(taker)
        # Past the 10 s after which a launch used to give up, and now
        # leaves its note.
        time.sleep(13)
        for owner, launch in (("busy", busy), ("dying", taker.proc)):
            if launch.poll() is not None:
                raise Failure(f"launch gave up on the {owner} owner, "
                              f"alive: rc={launch.returncode}")
        os.kill(first.proc.pid, signal.SIGCONT)
        _, err = busy.communicate(timeout=30)
        if busy.returncode != 0 or b"busy" not in err:
            raise Failure("launch against a busy instance should wait, with "
                          "a note, and exit 0 once it is free, got "
                          f"rc={busy.returncode}: {err[-400:]}")
        first.wait_until(_on("repo-a"),
                         "busy instance to open repo A once free")

        dying.crash()
        taker.wait_bridge()
        taker.wait_until(_on("repo-b"),
                         "launch to take the dead owner's place with repo B")
    finally:
        # Before teardown kills its owner: it would take over and run
        # on with nobody to quit it.
        if busy.poll() is None:
            busy.kill()
            busy.wait()
        if first.proc.poll() is None:
            os.kill(first.proc.pid, signal.SIGCONT)
    # It still owns the namespace: nothing took the lock or the socket
    # while it was stopped.
    again = first.launch_second(repo_b)
    if again.returncode != 0 or b"busy" in again.stderr:
        raise Failure("hand-off after thaw should be answered, got "
                      f"rc={again.returncode}: {again.stderr[-400:]}")
    first.wait_until(_on("repo-b"), "switch to repo B after thaw")

    rc = first.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{first.tail()}")
    left = first.instance_files()
    if left:
        raise Failure(f"graceful quit left {left} in {first.runtime_dir}")


if __name__ == "__main__":
    run(test)
