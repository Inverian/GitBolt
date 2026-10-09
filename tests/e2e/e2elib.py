"""Shared harness for GitBolt's end-to-end tests.

Each test launches the real app binary with the test bridge enabled
(docs/AGENT_TESTING.md) and asserts repository outcomes on disk or
through `dump-state` — never by scraping widgets. Isolation, so a
test run can coexist with a real GitBolt session and with other tests:

- GITBOLT_TEST_BRIDGE: unique socket name per App instance.
- GITBOLT_INSTANCE_NAME: unique namespace for the single-instance
  lock + forwarding socket, so a test launch can never forward its
  repo into a session the user has open.
- TMPDIR + XDG_RUNTIME_DIR: a private runtime dir per App. Everything
  the app creates at runtime lands in it — the instance lock and
  socket (QDir::tempPath() on macOS, $XDG_RUNTIME_DIR on Linux), the
  bridge socket, the log — and kill() removes it, so even a SIGKILLed
  app leaves nothing behind.
- XDG_CONFIG_HOME: a private dir unless the test passes config_home,
  so GitBolt.ini reads/writes never touch the user's real settings.
- QT_QPA_PLATFORM=offscreen: no focus stealing, same behavior on
  headless CI. An exported QT_QPA_PLATFORM wins for the apps a test
  drives, so a developer can watch them; never for a second launch,
  which may end in a modal nobody would dismiss. (QLocalServer named
  sockets are unix-domain sockets, which is why these tests are gated
  off on Windows.)
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


def make_runtime_dir() -> str:
    """A private (0700) dir to serve as an app's TMPDIR and
    XDG_RUNTIME_DIR. Short prefix: the unix sockets inside it must fit
    sun_path (104 bytes on macOS)."""
    return tempfile.mkdtemp(prefix="gb-")


def app_env(runtime_dir: str, instance_name: str,
            bridge_name: str | None = None,
            config_home: str | None = None) -> dict:
    """Environment for a GitBolt process isolated into `runtime_dir`."""
    env = os.environ.copy()
    env.pop("GITBOLT_TEST_BRIDGE", None)
    if bridge_name:
        env["GITBOLT_TEST_BRIDGE"] = bridge_name
    env["GITBOLT_INSTANCE_NAME"] = instance_name
    env["TMPDIR"] = runtime_dir
    env["XDG_RUNTIME_DIR"] = runtime_dir
    env["XDG_CONFIG_HOME"] = (config_home
                              or os.path.join(runtime_dir, "config"))
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    return env


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
                 runtime_dir: str | None = None,
                 label: str = "app"):
        tag = uuid.uuid4().hex[:8]
        self.binary = binary
        self.label = label
        self.bridge_name = f"gb-e2e-{tag}"
        self.instance_name = instance_name or f"gb-e2e-inst-{tag}"
        # Pass another App's runtime_dir (with its instance_name) to
        # launch into its single-instance namespace; only the App that
        # created the dir removes it.
        self._owns_runtime_dir = runtime_dir is None
        self.runtime_dir = runtime_dir or make_runtime_dir()
        self.log_path = os.path.join(self.runtime_dir,
                                     f"{self.bridge_name}.log")
        self.env = app_env(self.runtime_dir, self.instance_name,
                           bridge_name=self.bridge_name,
                           config_home=config_home)

        args = [binary] + ([repo] if repo else [])
        self._log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            args, env=self.env, stdout=self._log, stderr=subprocess.STDOUT)

    @property
    def lock_path(self) -> str:
        return os.path.join(self.runtime_dir, self.instance_name + ".lock")

    def instance_files(self) -> list[str]:
        """This app's lock and sockets still on disk (names only)."""
        names = (self.instance_name, self.instance_name + ".lock",
                 self.instance_name + ".lock.rmlock", self.bridge_name)
        return [n for n in names
                if os.path.exists(os.path.join(self.runtime_dir, n))]

    def launch_second(self, *args: str,
                      timeout: float = 30) -> subprocess.CompletedProcess:
        """Run another GitBolt in this app's single-instance namespace
        to completion — it should hand `args` over to this app."""
        env = dict(self.env)
        # Its own bridge name: a window it wrongly opened must not take
        # over this app's bridge socket.
        env["GITBOLT_TEST_BRIDGE"] = f"{self.bridge_name}-second"
        # Offscreen whatever the developer exported: a launch that gives
        # up shows its "not responding" modal on any real platform
        # (wayland, xcb, cocoa), and nobody would dismiss it — the run
        # would hang to the timeout below instead of seeing exit 1.
        env["QT_QPA_PLATFORM"] = "offscreen"
        try:
            return subprocess.run([self.binary, *args], env=env,
                                  capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise Failure(f"second launch {list(args)} never exited — "
                          "it opened its own window or hung on a "
                          "dialog") from None

    # ---- bridge protocol -------------------------------------------------

    def cmd(self, line: str, timeout: float = 10.0) -> dict:
        path = os.path.join(self.runtime_dir, self.bridge_name)
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

    def crash(self) -> None:
        """SIGKILL, leaving the lock and sockets behind as a crash or a
        force-quit does."""
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(5)

    def kill(self) -> None:
        """Teardown: SIGKILL if still running, then remove the runtime
        dir with whatever the app left in it."""
        self.crash()
        if not self._log.closed:
            self._log.close()
        if self._owns_runtime_dir:
            shutil.rmtree(self.runtime_dir, ignore_errors=True)

    def tail(self, lines: int = 15) -> str:
        if not self._log.closed:
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
    passed = False
    try:
        test_fn(binary, scratch, apps)
        passed = True
    except Failure as e:
        print(f"FAIL: {e}")
    finally:
        if not passed:
            # The logs go with each app's runtime dir; keep their ends.
            for app in apps:
                print(f"--- {app.label} log:{app.tail()}")
        for app in apps:
            app.kill()
        shutil.rmtree(scratch, ignore_errors=True)
    if passed:
        print("PASS")
    sys.exit(0 if passed else 1)
