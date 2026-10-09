#!/usr/bin/env python3
"""E2E: the single-instance lock never outlives its owner.

The guard used to be QSharedMemory — a System V segment on macOS,
which nothing reclaims. Every `--version`/`--help` run (exit() from
inside the argument parser skips destructors) and every SIGKILLed or
crashed instance leaked one; once macOS's 32 were used up, each launch
was turned away as "already running" behind a modal. Locked here:

- early exits (--version, --help, a bad option) leave nothing behind
  (on macOS: no shared-memory segment either);
- a relaunch after SIGKILL starts normally and owns the namespace (a
  further launch hands off to it);
- a leftover lock whose contents can't prove it stale (emptied, as a
  crash between creating and writing it would leave it) is reclaimed
  once nothing holds it, and a held one never is;
- that reclaiming waits its turn on "<lock>.rmlock", as QLockFile's
  own stale removal does — two launchers removing at once could
  otherwise both end up running;
- a graceful quit removes the lock and the socket.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time

from e2elib import App, Failure, app_env, make_repo, run


def _shm_segments() -> dict[str, str]:
    """macOS: SysV shared-memory segment id -> creator PID."""
    out = subprocess.run(["ipcs", "-m", "-p"], capture_output=True,
                         text=True).stdout
    segments = {}
    for line in out.splitlines():
        cols = line.split()   # T ID KEY MODE OWNER GROUP CPID LPID
        if len(cols) >= 8 and cols[0] == "m":
            segments[cols[1]] = cols[6]
    return segments


def _early_exits(binary: str, scratch: str) -> None:
    runtime = os.path.join(scratch, "early")
    os.mkdir(runtime, 0o700)
    env = app_env(runtime, "gb-e2e-inst-early")
    before = _shm_segments() if sys.platform == "darwin" else {}
    pids = set()
    for args, want in [(["--version"], 0), (["--help"], 0),
                       (["--no-such-option"], 1)] * 3:
        proc = subprocess.Popen([binary, *args], env=env,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        try:
            out, _ = proc.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise Failure(f"{args} never exited") from None
        pids.add(str(proc.pid))
        if proc.returncode != want:
            raise Failure(f"{args} exited {proc.returncode}, expected "
                          f"{want}: {out[-400:]}")
        if args == ["--version"] and b"GitBolt " not in out:
            raise Failure(f"--version printed {out[-400:]}")

    left = sorted(os.listdir(runtime))
    if left:
        raise Failure(f"early exits left {left} behind")
    if sys.platform == "darwin":
        leaked = [seg for seg, cpid in _shm_segments().items()
                  if seg not in before and cpid in pids]
        if leaked:
            raise Failure(f"early exits leaked shared memory {leaked}")


def _on(name: str):
    return lambda s: s.get("repoPath", "").rstrip("/").endswith(name)


def test(binary: str, scratch: str, apps: list) -> None:
    _early_exits(binary, scratch)

    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    repo_b = make_repo(os.path.join(scratch, "repo-b"))

    first = App(binary, repo_a, label="first instance")
    apps.append(first)
    first.wait_bridge()
    first.crash()
    left = first.instance_files()
    if first.instance_name + ".lock" not in left:
        raise Failure(f"SIGKILL left only {left}: nothing stale to test")

    # Same namespace, same dir: the lock names a dead PID.
    second = App(binary, repo_a, instance_name=first.instance_name,
                 runtime_dir=first.runtime_dir,
                 label="relaunch after SIGKILL")
    apps.append(second)
    second.wait_bridge()
    second.wait_until(_on("repo-a"), "repo A open after SIGKILL relaunch")
    handoff = second.launch_second(repo_b)
    if handoff.returncode != 0:
        raise Failure("hand-off to the relaunched instance should exit 0, "
                      f"got rc={handoff.returncode}: "
                      f"{handoff.stderr[-400:]}")
    second.wait_until(_on("repo-b"), "relaunched instance to take repo B")

    # No PID to test in an empty lock, so only the check that nothing
    # still holds it can clear it.
    second.crash()
    with open(second.lock_path, "w"):
        pass
    hour_ago = time.time() - 3600
    os.utime(second.lock_path, (hour_ago, hour_ago))
    # Another remover's turn: QLockFile reads a fresh, unreadable
    # .rmlock as held (for 30 s), so the launch has to wait it out.
    rmlock = second.lock_path + ".rmlock"
    with open(rmlock, "w"):
        pass

    third = App(binary, repo_a, instance_name=first.instance_name,
                runtime_dir=first.runtime_dir,
                label="relaunch over an empty lock")
    apps.append(third)
    time.sleep(3)
    if third.proc.poll() is not None:
        raise Failure("launch gave up while another removal had its turn "
                      f"(rc={third.proc.returncode}){third.tail()}")
    try:
        untouched = os.path.getsize(second.lock_path) == 0
    except OSError:
        untouched = False
    if not untouched:
        raise Failure("the stale lock was removed while its .rmlock was "
                      "held")
    os.remove(rmlock)
    third.wait_bridge()
    third.wait_until(_on("repo-a"), "repo A open over the empty lock")

    # That recovery must never take a lock that is still held: with
    # the live owner's socket gone, a launch finds no listener, retries
    # the removal for its whole wait, and has to give up.
    os.remove(os.path.join(third.runtime_dir, third.instance_name))
    blocked = third.launch_second(repo_b)
    if blocked.returncode != 1 or b"not responding" not in blocked.stderr:
        raise Failure("launch against a live owner without a socket should "
                      f"give up with rc 1, got rc={blocked.returncode}: "
                      f"{blocked.stderr[-400:]}")
    if not os.path.exists(third.lock_path):
        raise Failure("a live instance's lock was removed")
    if not _on("repo-a")(third.state()):
        raise Failure(f"owner changed repo: {third.state()}")

    rc = third.quit()
    if rc != 0:
        raise Failure(f"app exited rc={rc} after quit{third.tail()}")
    left = third.instance_files()
    if left:
        raise Failure(f"graceful quit left {left} in {third.runtime_dir}")


if __name__ == "__main__":
    run(test)
