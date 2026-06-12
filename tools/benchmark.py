#!/usr/bin/env python3
"""Open-time benchmark against a synthetic monster repository.

Generates (once, offline) a repo that is hostile on both axes GitBolt
cares about — history depth and tree width:

  - 50,000 commits via git fast-import (log paging, lane computation)
  - 20,000 directories x 1 file in the final tree (watcher
    enumeration, status walk)

then launches the real app through the test bridge and measures the
moments a user actually feels:

  t_bridge   process start -> bridge answering (app init)
  t_open     -> dump-state repoOpen (libgit2 open + watch internals)
  t_commits  -> logRows > 0 (first commits on screen)
  rss_mb     resident memory once those have settled

Usage:
  tools/benchmark.py <path-to-gitbolt-binary> [repo-dir]

The repo is cached in <repo-dir> (default /tmp/gitbolt-monster) and
reused on later runs; delete it to regenerate. Results print as a
markdown row ready for docs/PERFORMANCE.md.
"""

from __future__ import annotations

import json
import os
import platform
import socket
import subprocess
import sys
import tempfile
import time
import uuid

COMMITS = 50_000
WIDE_DIRS = 20_000
FILE_POOL = 2_000   # paths the 50k commits churn over


def generate(repo: str) -> None:
    print(f"generating monster repo at {repo} "
          f"({COMMITS} commits, {WIDE_DIRS} wide dirs)…", flush=True)
    os.makedirs(repo)
    subprocess.run(["git", "init", "-q", "-b", "main", repo], check=True)

    fi = subprocess.Popen(
        ["git", "fast-import", "--quiet"], cwd=repo,
        stdin=subprocess.PIPE)
    w = fi.stdin
    assert w is not None

    def emit(s: str) -> None:
        w.write(s.encode())

    # One blob per pool slot up front; commits then re-point paths at
    # a rotating blob, which is enough churn to make every commit's
    # tree distinct without 50k unique blobs.
    for i in range(FILE_POOL):
        emit(f"blob\nmark :{i + 1}\ndata 20\n{i:019d}\n")

    for c in range(COMMITS):
        path = (f"src/mod{(c % 40):02d}/dir{(c % 200):03d}/"
                f"file{(c % FILE_POOL):04d}.txt")
        emit(f"commit refs/heads/main\nmark :{FILE_POOL + c + 1}\n")
        emit("author Bench Mark <bench@gitbolt.test> "
             f"{1500000000 + c} +0000\n")
        emit("committer Bench Mark <bench@gitbolt.test> "
             f"{1500000000 + c} +0000\n")
        msg = f"commit {c}: touch {path}"
        emit(f"data {len(msg)}\n{msg}\n")
        if c > 0:
            emit(f"from :{FILE_POOL + c}\n")
        emit(f"M 100644 :{(c % FILE_POOL) + 1} {path}\n")

    # Final commit: the wide tree. One file per directory so the
    # watcher's directory enumeration sees WIDE_DIRS distinct dirs.
    emit(f"commit refs/heads/main\nmark :{FILE_POOL + COMMITS + 1}\n")
    emit("author Bench Mark <bench@gitbolt.test> "
         f"{1500000000 + COMMITS} +0000\n")
    emit("committer Bench Mark <bench@gitbolt.test> "
         f"{1500000000 + COMMITS} +0000\n")
    msg = f"wide tree: {WIDE_DIRS} directories"
    emit(f"data {len(msg)}\n{msg}\n")
    emit(f"from :{FILE_POOL + COMMITS}\n")
    for d in range(WIDE_DIRS):
        emit(f"M 100644 :1 wide/d{d:05d}/f.txt\n")
    emit("done\n")
    w.close()
    if fi.wait() != 0:
        raise RuntimeError("git fast-import failed")

    subprocess.run(["git", "checkout", "-q", "main"], cwd=repo, check=True)
    print("generation done.", flush=True)


def bridge_cmd(name: str, line: str, timeout: float = 5.0) -> dict:
    path = os.path.join(
        os.environ.get("TMPDIR", tempfile.gettempdir()), name)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(path)
        s.sendall(line.encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode())


def measure(binary: str, repo: str) -> dict:
    tag = uuid.uuid4().hex[:8]
    bridge = f"gb-bench-{tag}"
    env = os.environ.copy()
    env["GITBOLT_TEST_BRIDGE"] = bridge
    env["GITBOLT_INSTANCE_NAME"] = f"gb-bench-inst-{tag}"
    env.setdefault("QT_QPA_PLATFORM", "offscreen")

    t0 = time.monotonic()
    proc = subprocess.Popen([binary, repo], env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    t_bridge = t_open = t_commits = None
    deadline = t0 + 180
    try:
        while time.monotonic() < deadline:
            try:
                state = bridge_cmd(bridge, "dump-state", timeout=2)
            except (OSError, json.JSONDecodeError):
                time.sleep(0.02)
                continue
            now = time.monotonic() - t0
            if t_bridge is None:
                t_bridge = now
            if t_open is None and state.get("repoOpen"):
                t_open = now
            if t_commits is None and state.get("logRows", 0) > 0:
                t_commits = now
                break
            time.sleep(0.02)
        if t_commits is None:
            raise RuntimeError("timed out before first commits appeared")

        time.sleep(3)   # let refreshes/watch enumeration settle
        rss_kb = int(subprocess.run(
            ["ps", "-o", "rss=", "-p", str(proc.pid)],
            capture_output=True, text=True).stdout.strip() or 0)
        log_rows = bridge_cmd(bridge, "dump-state").get("logRows", 0)
    finally:
        try:
            bridge_cmd(bridge, "quit")
            proc.wait(10)
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()

    return {
        "t_bridge_s": round(t_bridge, 2),
        "t_open_s": round(t_open, 2),
        "t_commits_s": round(t_commits, 2),
        "rss_mb": round(rss_kb / 1024),
        "log_rows_at_settle": log_rows,
    }


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    repo = sys.argv[2] if len(sys.argv) > 2 else "/tmp/gitbolt-monster"

    if not os.path.isdir(os.path.join(repo, ".git")):
        generate(repo)

    print("measuring (3 runs)…", flush=True)
    runs = [measure(binary, repo) for _ in range(3)]
    best = min(runs, key=lambda r: r["t_commits_s"])

    print("\nruns:", json.dumps(runs, indent=2))
    print(f"\nmachine: {platform.platform()} / "
          f"{os.cpu_count()} cores")
    print(f"repo: {COMMITS} commits, {WIDE_DIRS} wide dirs")
    print("\nmarkdown row (best of 3):")
    print("| date | commits | dirs | bridge | open | first commits |"
          " RSS |")
    print(f"| {time.strftime('%Y-%m-%d')} | {COMMITS} | {WIDE_DIRS} "
          f"| {best['t_bridge_s']}s | {best['t_open_s']}s "
          f"| {best['t_commits_s']}s | {best['rss_mb']} MB |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
