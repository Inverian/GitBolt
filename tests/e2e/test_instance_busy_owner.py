#!/usr/bin/env python3
"""E2E (macOS): a launch never gives up on an owner that is alive.

While the owner's GUI thread is busy, a launch's hand-off line can't
leave on Windows: Qt's pipes have no buffer, so the write waits for the
owner's event loop. That wait used to end after 30 s in "not
responding" (rc 1) for an owner that was merely busy for longer. Now:

- the launch waits for as long as the owner lives, and hands over once
  it is free — rc 0, no dialog;
- if the owner dies instead, the launch takes its place: the owner was
  alive until then, so its lock is about to come free, and nothing is
  "not responding".

A Unix socket buffers a short line at once, so these launches send one
longer than macOS's 8 KB socket buffers — the path padded with spaces,
which the owner trims — and wait on a stopped owner just as every
hand-off waits on a busy one on Windows. (Linux buffers far more, and
caps one argument at 128 KB, so this test is macOS-only.)
"""

from __future__ import annotations

import os
import signal
import subprocess
import time

from e2elib import App, Failure, make_repo, run

# Longer than the 30 s the hand-off used to give a busy owner.
BUSY_FOR = 35


def _on(name: str):
    return lambda s: s.get("repoPath", "").rstrip("/").endswith(name)


def _held_up(path: str) -> str:
    """`path` as a hand-off line the socket can't take in one go."""
    return path + " " * (64 * 1024)


def _wait_for_lock(app: App, timeout: float = 20.0) -> None:
    """Until `app` holds the single-instance lock: its PID in it."""
    deadline = time.monotonic() + timeout
    holder = ""
    while time.monotonic() < deadline:
        if app.proc.poll() is not None:
            raise Failure(f"{app.label} exited (rc={app.proc.returncode}) "
                          f"instead of taking over{app.tail()}")
        try:
            with open(app.lock_path) as fh:
                holder = fh.readline().strip()
        except OSError:
            holder = ""
        if holder == str(app.proc.pid):
            return
        time.sleep(0.2)
    raise Failure(f"{app.label} never took the lock (it names "
                  f"{holder or 'nobody'}){app.tail()}")


def test(binary: str, scratch: str, apps: list) -> None:
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))

    busy = App(binary, repo_a, label="busy owner")
    dying = App(binary, repo_a, label="owner that dies busy")
    apps += [busy, dying]
    for owner in (busy, dying):
        owner.wait_bridge()
        owner.wait_until(_on("repo-a"), "repo A open")

    os.kill(busy.proc.pid, signal.SIGSTOP)
    os.kill(dying.proc.pid, signal.SIGSTOP)
    started = time.monotonic()
    waiting = busy.start_second(_held_up(repo_b))
    try:
        taker = App(binary, _held_up(repo_b),
                    instance_name=dying.instance_name,
                    runtime_dir=dying.runtime_dir,
                    label="launch waiting on the dying owner")
        apps.append(taker)

        # Past the 10 s a launch gives a lock that nobody listens for:
        # this one is connected, so its owner is alive, and it waits.
        time.sleep(12)
        if taker.proc.poll() is not None:
            raise Failure("launch gave up on a live owner "
                          f"(rc={taker.proc.returncode}){taker.tail()}")
        dying.crash()
        # The lock, not its bridge: running, it goes on to open its
        # padded path, and its error box spends far longer than any
        # wait here laying that out — on a core the rest needs, so it
        # is stopped as soon as it holds the lock.
        _wait_for_lock(taker)
        taker.crash()

        time.sleep(max(0.0, BUSY_FOR - (time.monotonic() - started)))
        if waiting.poll() is not None:
            _, err = waiting.communicate()
            raise Failure(f"launch gave up on an owner busy for "
                          f"{BUSY_FOR} s: rc={waiting.returncode}: "
                          f"{err[-400:]}")
        os.kill(busy.proc.pid, signal.SIGCONT)
        _, err = waiting.communicate(timeout=30)
        if waiting.returncode != 0 or b"not responding" in err:
            raise Failure("hand-off to the freed owner should exit 0, "
                          f"got rc={waiting.returncode}: {err[-400:]}")
        busy.wait_until(_on("repo-b"), "freed owner to open repo B")
    except subprocess.TimeoutExpired:
        raise Failure("launch still waiting after its owner was "
                      "free") from None
    finally:
        # Before teardown kills its owner: it would take over and run
        # on with nobody to quit it.
        if waiting.poll() is None:
            waiting.kill()
            waiting.wait()
        if busy.proc.poll() is None:
            os.kill(busy.proc.pid, signal.SIGCONT)


if __name__ == "__main__":
    run(test)
