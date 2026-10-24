#!/usr/bin/env python3
"""Folders opened through LaunchServices, as Finder and the Dock send
them: open-documents events to the real cocoa app, launched by `open`,
never by the harness and never offscreen (test_file_open sends the same
events itself).

1. A cold launch with a subfolder (`open -n GitBolt.app <repo-a/sub>`,
   no arguments): the folder comes as an event, and repo-a opens.
2. `open -a GitBolt.app <repo-b>` goes to the running GitBolt.
3. A second launch with a folder (`open -n`, the same instance name)
   takes its event before the single-instance guard and hands it over:
   repo-c opens in the first, and the second exits.
4. Finder's service provider on the real NSApp (bridge service-open).
5. The service itself goes to the user's GitBolt only: macOS gives it
   to the first process that asks, so a GitBolt with an instance name
   of its own (this test's, the dogfood driver's) must never register
   it, and one in the default namespace must.

`open` registers the build's GitBolt.app with LaunchServices, which
only a throwaway CI runner may have done to it: the test runs only
with GITBOLT_E2E_LAUNCHSERVICES=1 on CI, and is skipped otherwise.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import (App, Failure, app_env, make_repo, make_runtime_dir,  # noqa: E402
                    on_screen, run)

SKIP = 77   # SKIP_RETURN_CODE in tests/e2e/CMakeLists.txt


def bundle_of(binary: str) -> str:
    """GitBolt.app, from GitBolt.app/Contents/MacOS/GitBolt."""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(binary))))


def processes(binary: str) -> list[str]:
    out = subprocess.run(["pgrep", "-f", os.path.abspath(binary)],
                         capture_output=True, text=True).stdout
    return out.split()


SERVICE = "com.gitbolt.app.ServiceProvider"


def service_registered() -> bool:
    """launchd has the service's endpoint (AppKit names it after the
    bundle ID)."""
    out = subprocess.run(["launchctl", "print", f"gui/{os.getuid()}"],
                         capture_output=True, text=True).stdout
    return SERVICE in out


def same_dir(a: str, b: str) -> bool:
    return os.path.realpath(a.rstrip("/")) == os.path.realpath(b.rstrip("/"))


class Launched(App):
    """A GitBolt that LaunchServices started (`open -n`): the bridge
    and teardown of App, with no process of the harness's own."""

    class _NoProcess:
        returncode = None

        def poll(self):
            return None

        def kill(self):
            pass

        def wait(self, timeout=None):
            return 0

    def __init__(self, binary: str, folder: str, runtime_dir: str,
                 instance_name: str | None, label: str):
        self.binary = binary
        self.label = label
        self.bridge_name = f"gb-ls-{uuid.uuid4().hex[:8]}"
        self.instance_name = instance_name
        # Removed by the harness's kill(), after a failure's log tail.
        self._owns_runtime_dir = True
        self.runtime_dir = runtime_dir
        self.log_path = os.path.join(runtime_dir, f"{self.bridge_name}.log")
        self.env = app_env(runtime_dir, instance_name or "", bridge_name=self.bridge_name,
                           home=os.path.join(runtime_dir, "home"))
        if not instance_name:
            # The default namespace: the user's own GitBolt's.
            del self.env["GITBOLT_INSTANCE_NAME"]
        self._log = open(self.log_path, "w")
        self.proc = self._NoProcess()
        launch(binary, folder, self.env, self.log_path, new_instance=True)

    def crash(self) -> None:
        pass   # see teardown()


def launch(binary: str, folder: str, env: dict, log: str, new_instance: bool) -> None:
    """`open` the folder with GitBolt.app; only the variables GitBolt
    reads go with it (LaunchServices gives the app none of ours)."""
    args = ["open", "-g"]
    if new_instance:
        args.append("-n")
        for key in ("GITBOLT_TEST_BRIDGE", "GITBOLT_INSTANCE_NAME", "TMPDIR",
                    "XDG_CONFIG_HOME", "HOME", "GITBOLT_TELEMETRY"):
            if key in env:
                args += ["--env", f"{key}={env[key]}"]
        args += ["--stderr", log]   # where Qt's warnings go
    args += ["-a", bundle_of(binary), folder]
    done = subprocess.run(args, capture_output=True, text=True, timeout=60)
    if done.returncode != 0:
        raise Failure(f"{' '.join(args)} -> {done.returncode}: {done.stderr}")


def test(binary: str, scratch: str, apps: list) -> None:
    if processes(binary):
        raise Failure(f"GitBolt is already running from {binary}")
    repo_a = make_repo(os.path.join(scratch, "repo-a"))
    sub = os.path.join(repo_a, "sub")
    os.makedirs(sub)
    repo_b = make_repo(os.path.join(scratch, "repo-b"))
    repo_c = make_repo(os.path.join(scratch, "repo-c"))
    repo_d = make_repo(os.path.join(scratch, "repo-d"))

    runtime_dir = make_runtime_dir()
    instance = f"gb-ls-inst-{uuid.uuid4().hex[:8]}"
    first = Launched(binary, sub, runtime_dir, instance, "first")
    apps.append(first)
    try:
        first.wait_bridge(timeout=60)
        state = first.wait_until(on_screen("repo-a"), "repo-a from a cold launch", timeout=30)
        if not same_dir(state["repoPath"], repo_a):
            raise Failure(f"cold launch opened {state['repoPath']}")

        launch(binary, repo_b, first.env, first.log_path, new_instance=False)
        first.wait_until(on_screen("repo-b"), "repo-b sent to the running GitBolt", timeout=30)

        second_env = dict(first.env)
        second_env["GITBOLT_TEST_BRIDGE"] = f"{first.bridge_name}-second"
        launch(binary, repo_c, second_env,
               os.path.join(runtime_dir, "second.log"), new_instance=True)
        try:
            first.wait_until(on_screen("repo-c"), "repo-c handed over by a second launch",
                             timeout=30)
        except Failure as e:
            raise Failure(f"{e}; the second launch's log: "
                          f"{read(os.path.join(runtime_dir, 'second.log'))}") from None
        deadline = time.monotonic() + 20
        while len(processes(binary)) > 1:
            if time.monotonic() > deadline:
                raise Failure(f"the second launch never exited: {processes(binary)}; "
                              f"its log: {read(os.path.join(runtime_dir, 'second.log'))}")
            time.sleep(0.5)

        first.ok(f"service-open {repo_d}")
        first.wait_until(on_screen("repo-d"), "repo-d from the service provider", timeout=30)

        if service_registered():
            raise Failure(f"a GitBolt with an instance name of its own took {SERVICE}")
    finally:
        teardown(binary, first)

    # The default namespace (its lock still in a runtime dir of its own).
    default_dir = make_runtime_dir()
    users = Launched(binary, repo_a, default_dir, None, "default namespace")
    apps.append(users)
    try:
        users.wait_bridge(timeout=60)
        users.wait_until(on_screen("repo-a"), "repo-a in the default namespace", timeout=30)
        deadline = time.monotonic() + 15
        while not service_registered():
            if time.monotonic() > deadline:
                raise Failure(f"the default-namespace GitBolt never registered {SERVICE}")
            time.sleep(0.5)
    finally:
        teardown(binary, users)


def read(path: str) -> str:
    try:
        with open(path) as fh:
            return fh.read()[-2000:]
    except OSError:
        return "(none)"


def teardown(binary: str, app: App) -> None:
    try:
        app.cmd("quit")
    except OSError:
        pass
    deadline = time.monotonic() + 10
    while processes(binary) and time.monotonic() < deadline:
        time.sleep(0.5)
    for pid in processes(binary):
        subprocess.run(["kill", "-9", pid])


if __name__ == "__main__":
    if sys.platform != "darwin":
        print("SKIP: macOS only")
        sys.exit(SKIP)
    if os.environ.get("GITBOLT_E2E_LAUNCHSERVICES") != "1" or not (
            os.environ.get("CI") or os.environ.get("GITHUB_ACTIONS")):
        print("SKIP: registers the app with LaunchServices; "
              "CI only, with GITBOLT_E2E_LAUNCHSERVICES=1")
        sys.exit(SKIP)
    run(test)
