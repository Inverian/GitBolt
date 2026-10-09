#!/usr/bin/env python3
"""E2E: a second launch hands its repo path over and exits 0.

This is how "Open in GitBolt" from a file manager reaches an already-
open window. The second process must exit cleanly (not show a modal
warning) and the first instance must switch to the forwarded repo. A
bare relaunch (no path, e.g. a launcher click) must be answered too.
And when the first instance is alive but busy — its GUI thread stuck
in a long synchronous git call, so the hand-off goes unanswered — the
path is still queued for it: the second launch exits 0 with a note,
not an error dialog and not a rival window, and the owner opens the
path once it is free.
"""

from __future__ import annotations

import os
import signal

from e2elib import App, Failure, make_repo, run


def _on(name: str):
    return lambda s: s.get("repoPath", "").rstrip("/").endswith(name)


def test(binary: str, scratch: str, apps: list) -> None:
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))

    first = App(binary, repo_a, label="first instance")
    apps.append(first)
    first.wait_bridge()
    first.wait_until(_on("repo-a"), "repo A open")

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

    # Busy owner (stopped here): the hand-off connects and its line is
    # sent, but the receipt never comes in the launch's wait.
    os.kill(first.proc.pid, signal.SIGSTOP)
    try:
        busy = first.launch_second(repo_a)
    finally:
        os.kill(first.proc.pid, signal.SIGCONT)
    if busy.returncode != 0 or b"busy" not in busy.stderr:
        raise Failure("launch against a busy instance should leave the "
                      "path queued and exit 0 with a note, got "
                      f"rc={busy.returncode}: {busy.stderr[-400:]}")
    first.wait_until(_on("repo-a"),
                     "busy instance to open the queued repo A once free")
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
