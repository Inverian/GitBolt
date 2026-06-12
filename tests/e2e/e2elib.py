"""Shared harness for GitBolt's end-to-end tests.

Each test launches the real app binary with the test bridge enabled
(docs/AGENT_TESTING.md) and asserts repository outcomes on disk or
through `dump-state` — never by scraping widgets. Isolation, so a
test run can coexist with a real GitBolt session and with other tests:

- GITBOLT_TEST_BRIDGE: unique socket name per App instance.
- GITBOLT_INSTANCE_NAME: unique namespace for the single-instance
  guard + forwarding socket, so a test launch can never forward its
  repo into a session the user has open.
- XDG_CONFIG_HOME: per-test temp dir, so GitBolt.ini reads/writes
  never touch the user's real settings.
- QT_QPA_PLATFORM=offscreen: no focus stealing, same behavior on
  headless CI. (QLocalServer named sockets are unix-domain sockets,
  which is why these tests are gated off on Windows.)
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid


def _tmpdir() -> str:
    # QLocalServer puts named sockets in QDir::tempPath(): $TMPDIR on
    # macOS (per-user), else /tmp.
    return os.environ.get("TMPDIR", tempfile.gettempdir())


def git(*args: str, cwd: str) -> str:
    """Run git, returning stripped stdout; raises on failure."""
    out = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(args)} failed in {cwd}:\n{out.stderr}")
    return out.stdout.strip()


def make_repo(path: str, files=("a.txt",), msg: str = "base") -> str:
    """Init a repo at `path` with one commit containing `files`."""
    os.makedirs(path, exist_ok=True)
    git("init", "-q", "-b", "main", cwd=path)
    git("config", "user.email", "e2e@gitbolt.test", cwd=path)
    git("config", "user.name", "GitBolt E2E", cwd=path)
    for f in files:
        with open(os.path.join(path, f), "w") as fh:
            fh.write(f"{f}\n")
    git("add", "-A", cwd=path)
    git("commit", "-q", "-m", msg, cwd=path)
    return path


class Failure(Exception):
    """Raised by assertions; main() turns it into a FAIL + exit 1."""


class App:
    """One GitBolt process plus its private bridge connection."""

    def __init__(self, binary: str, repo: str | None = None,
                 config_home: str | None = None,
                 instance_name: str | None = None,
                 label: str = "app"):
        tag = uuid.uuid4().hex[:8]
        self.label = label
        self.bridge_name = f"gb-e2e-{tag}"
        self.instance_name = instance_name or f"gb-e2e-inst-{tag}"
        self.log_path = os.path.join(_tmpdir(), f"gb-e2e-{tag}.log")

        env = os.environ.copy()
        env["GITBOLT_TEST_BRIDGE"] = self.bridge_name
        env["GITBOLT_INSTANCE_NAME"] = self.instance_name
        env.setdefault("QT_QPA_PLATFORM", "offscreen")
        if config_home:
            env["XDG_CONFIG_HOME"] = config_home
        self.env = env

        args = [binary] + ([repo] if repo else [])
        self._log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            args, env=env, stdout=self._log, stderr=subprocess.STDOUT)

    # ---- bridge protocol -------------------------------------------------

    def cmd(self, line: str, timeout: float = 10.0) -> dict:
        path = os.path.join(_tmpdir(), self.bridge_name)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout)
            sock.connect(path)
            sock.sendall(line.encode() + b"\n")
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf.decode())

    def wait_bridge(self, timeout: float = 20.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise Failure(
                    f"{self.label} exited early "
                    f"(rc={self.proc.returncode}) — log: {self.tail()}")
            try:
                if self.cmd("dump-state").get("ok"):
                    return
            except (OSError, json.JSONDecodeError):
                time.sleep(0.25)
        raise Failure(f"{self.label}: bridge never came up "
                      f"— log: {self.tail()}")

    def state(self) -> dict:
        return self.cmd("dump-state")

    def wait_until(self, pred, what: str, timeout: float = 20.0) -> dict:
        """Poll dump-state until pred(state) is truthy."""
        deadline = time.monotonic() + timeout
        last = {}
        while time.monotonic() < deadline:
            last = self.state()
            if pred(last):
                return last
            time.sleep(0.3)
        raise Failure(f"{self.label}: timed out waiting for {what}; "
                      f"last state: {json.dumps(last)}")

    # ---- widget helpers ----------------------------------------------------

    def find_action_slug(self, prefix: str) -> str:
        """First enabled action whose slug is `prefix` or `prefix-<n>`
        (live-count suffixes: 'commands/commit' matches
        'commands/commit-4')."""
        actions = self.cmd("list-actions")["actions"]
        for a in actions:
            slug = a["slug"]
            if slug == prefix or (
                    slug.startswith(prefix + "-")
                    and slug[len(prefix) + 1:].isdigit()):
                if a["enabled"]:
                    return slug
        raise Failure(f"no enabled action matching {prefix}; have: "
                      + ", ".join(a["slug"] for a in actions[:40]))

    def view_rows(self, object_name: str) -> int:
        for v in self.cmd("list-widgets")["views"]:
            if v.get("objectName") == object_name:
                return v["rows"]
        raise Failure(f"view not visible: {object_name}")

    def wait_view_rows(self, object_name: str, expected: int,
                       timeout: float = 15.0) -> None:
        """Poll until a view holds exactly `expected` rows.

        click/trigger are queued (the bridge replies before the action
        runs), and views populate from async refreshes — any read-after-
        write of a row count must poll or it races the event loop.
        """
        deadline = time.monotonic() + timeout
        last: int | None = None
        while time.monotonic() < deadline:
            try:
                last = self.view_rows(object_name)
            except Failure:
                last = None    # view not created/visible yet
            if last == expected:
                return
            time.sleep(0.2)
        raise Failure(f"{self.label}: {object_name} expected {expected} "
                      f"rows, last saw {last}")

    def ok(self, line: str) -> dict:
        """Send a command and fail the test on {"ok": false}."""
        resp = self.cmd(line)
        if not resp.get("ok"):
            raise Failure(f"`{line}` -> {json.dumps(resp)}")
        return resp

    # ---- teardown ----------------------------------------------------------

    def quit(self, timeout: float = 10.0) -> int:
        """Graceful close; returns exit code."""
        try:
            self.cmd("quit")
        except OSError:
            pass
        try:
            return self.proc.wait(timeout)
        finally:
            self._log.close()

    def kill(self) -> None:
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(5)
        if not self._log.closed:
            self._log.close()

    def tail(self, lines: int = 15) -> str:
        self._log.flush()
        try:
            with open(self.log_path) as fh:
                return "\n" + "".join(fh.readlines()[-lines:])
        except OSError:
            return "(no log)"


def run(test_fn) -> None:
    """Entry point: scratch dir + app binary from argv, PASS/FAIL exit."""
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <path-to-gitbolt-binary>")
        sys.exit(2)
    binary = sys.argv[1]
    if not os.access(binary, os.X_OK):
        print(f"FAIL: not executable: {binary}")
        sys.exit(1)
    scratch = tempfile.mkdtemp(prefix="gb-e2e-")
    apps: list[App] = []
    try:
        test_fn(binary, scratch, apps)
        print("PASS")
        sys.exit(0)
    except Failure as e:
        print(f"FAIL: {e}")
        sys.exit(1)
    finally:
        for app in apps:
            app.kill()
        shutil.rmtree(scratch, ignore_errors=True)
