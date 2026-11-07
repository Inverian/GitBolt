#!/usr/bin/env python3
"""E2E: a real package updates itself in place, end to end.

Skipped (exit 77) unless GITBOLT_E2E_INSTALL_APP names a packaged GitBolt
installer of this platform: a .dmg on macOS, an .AppImage on Linux (CI's
just-built package; locally, `cpack -G DragNDrop -C Release` in the
build). Everything happens in scratch folders; nothing outside them is
touched (no /Applications, no real Trash, no installed GitBolt).

1. The package's app is put where an install would be: on macOS the
   DMG's GitBolt.app copied into a scratch "Applications" folder, with a
   marker file planted inside the bundle; on Linux the AppImage copied
   into a scratch folder.
2. That copy runs as version 0.1.0 (GITBOLT_TEST_APP_VERSION) against a
   loopback stub feed (update_stub.py) whose release is the package's
   real version, with the real installer and its SHA256SUMS.txt.
3. Help › Check for Updates › Install and Restart, through the bridge:
   the app downloads and checks the installer, gets it ready, starts the
   helper and quits.
4. The old copy was replaced: on macOS the marker is gone from the
   bundle, and the old bundle, marker and all, is in the scratch HOME's
   .Trash as GitBolt-0.1.0.app; the new bundle is the release's version
   and its signature verifies. On Linux the AppImage at the same path is
   a new file with the release's bytes.
5. The relaunched app answers the same bridge (the helper keeps
   GITBOLT_INSTANCE_NAME and the bridge's variables), from the same
   path, as a new process. The download, the staging and the helper's
   log were in the session's own folder in its TMPDIR
   (gitbolt-updates-XXXXXX, dump-state's updateDir), not the user's
   cache, and the relaunched copy goes on using that folder
   (GITBOLT_TEST_UPDATE_DIR, inherited through the helper), where the
   helper leaves its install-result.

The packaged app has no offscreen platform plugin (macdeployqt and
linuxdeploy ship none), so it runs on the real one: cocoa on macOS, and
on Linux whatever QT_QPA_PLATFORM says (xcb under xvfb-run in CI).
"""

from __future__ import annotations

import glob
import hashlib
import os
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import App, Failure, run  # noqa: E402
from update_stub import UpdateFeed  # noqa: E402

SKIP = 77   # SKIP_RETURN_CODE in tests/e2e/CMakeLists.txt
OLD_VERSION = "0.1.0"
MARKER = "e2e-old-copy-marker"


def hdiutil(*args: str, attempts: int = 5) -> None:
    """hdiutil, retried: attach and detach hit an intermittent "Resource
    busy" (actions/runner-images#7522), as in CI."""
    for attempt in range(1, attempts + 1):
        result = subprocess.run(["hdiutil", *args], capture_output=True, text=True)
        if result.returncode == 0:
            return
        if attempt == attempts:
            raise Failure(f"hdiutil {' '.join(args)}: {result.stderr.strip()}")
        time.sleep(attempt * 2)


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


class MacInstall:
    """The DMG's GitBolt.app in a scratch Applications folder."""

    def __init__(self, dmg: str, scratch: str) -> None:
        mount = tempfile.mkdtemp(prefix="gb-mnt-", dir=scratch)
        hdiutil("attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint", mount, dmg)
        try:
            source = os.path.join(mount, "GitBolt.app")
            if not os.path.isdir(source):
                raise Failure(f"no GitBolt.app in {dmg}")
            self.folder = os.path.join(scratch, "Applications")
            os.makedirs(self.folder)
            self.bundle = os.path.join(self.folder, "GitBolt.app")
            subprocess.run(["ditto", source, self.bundle], check=True)
        finally:
            hdiutil("detach", mount)
        self.version = self.bundle_version(self.bundle)
        self.asset = f"GitBolt-{self.version}-Darwin-arm64.dmg"
        self.binary = os.path.join(self.bundle, "Contents", "MacOS", "GitBolt")
        self.marker = os.path.join(self.bundle, "Contents", "Resources", MARKER)
        with open(self.marker, "w") as fh:
            fh.write("the old copy\n")
        self.platform = "cocoa"

    @staticmethod
    def bundle_version(bundle: str) -> str:
        with open(os.path.join(bundle, "Contents", "Info.plist"), "rb") as fh:
            return plistlib.load(fh)["CFBundleShortVersionString"]

    def check_replaced(self, home: str) -> None:
        if os.path.exists(self.marker):
            raise Failure("the marker is still in the bundle: the old copy wasn't replaced")
        version = self.bundle_version(self.bundle)
        if version != self.version:
            raise Failure(f"the bundle is version {version}, not {self.version}")
        trashed = os.path.join(home, ".Trash", f"GitBolt-{OLD_VERSION}.app")
        if not os.path.isfile(os.path.join(trashed, "Contents", "Resources", MARKER)):
            raise Failure(f"the old bundle isn't in the Trash as {trashed}: "
                          f"{os.listdir(os.path.join(home, '.Trash'))}")
        leftovers = [n for n in os.listdir(self.folder) if n != "GitBolt.app"]
        if leftovers:
            raise Failure(f"left in the Applications folder: {leftovers}")
        verify = subprocess.run(["codesign", "--verify", "--deep", "--strict", self.bundle],
                                capture_output=True, text=True)
        if verify.returncode != 0:
            raise Failure(f"the new bundle doesn't verify: {verify.stderr.strip()}")


class AppImageInstall:
    """The AppImage, copied into a scratch folder under a name of its
    own: one named like the release's asset would be refused (the
    download would land on the running file)."""

    def __init__(self, appimage: str, scratch: str) -> None:
        match = re.search(r"GitBolt-(\d+\.\d+\.\d+)-x86_64\.AppImage$", os.path.basename(appimage))
        self.version = os.environ.get("GITBOLT_E2E_INSTALL_VERSION") or (match and match.group(1))
        if not self.version:
            raise Failure(f"can't tell {appimage}'s version: name it GitBolt-<version>-x86_64.AppImage "
                          "or set GITBOLT_E2E_INSTALL_VERSION")
        self.asset = f"GitBolt-{self.version}-x86_64.AppImage"
        self.folder = os.path.join(scratch, "apps")
        os.makedirs(self.folder)
        self.binary = os.path.join(self.folder, "GitBolt.AppImage")
        shutil.copyfile(appimage, self.binary)
        os.chmod(self.binary, 0o755)
        self.old_inode = os.stat(self.binary).st_ino
        self.release_sha = sha256(appimage)
        self.platform = os.environ.get("QT_QPA_PLATFORM", "xcb")

    def check_replaced(self, home: str) -> None:
        if os.stat(self.binary).st_ino == self.old_inode:
            raise Failure("the AppImage is the same file: the old copy wasn't replaced")
        if sha256(self.binary) != self.release_sha:
            raise Failure("the AppImage isn't the release's")
        if not os.access(self.binary, os.X_OK):
            raise Failure("the new AppImage isn't executable")
        leftovers = [n for n in os.listdir(self.folder) if n != "GitBolt.AppImage"]
        if leftovers:
            raise Failure(f"left next to the AppImage: {leftovers}")


def install_log(app: App | None) -> str:
    """The helper's log: a test session downloads into a folder of its
    own in its TMPDIR."""
    if app is None:
        return ""
    paths = sorted(glob.glob(os.path.join(app.runtime_dir, "gitbolt-updates-*", "install.log")))
    if not paths:
        return f"\n(no gitbolt-updates-*/install.log in {app.runtime_dir})"
    out = ""
    for path in paths:
        try:
            with open(path, errors="replace") as fh:
                out += f"\n--- {path}:\n" + fh.read()[-4000:]
        except OSError as error:
            out += f"\n(can't read {path}: {error})"
    return out


def test(_binary: str, scratch: str, apps: list) -> None:
    installer = os.path.abspath(os.environ["GITBOLT_E2E_INSTALL_APP"])
    install = (MacInstall if sys.platform == "darwin" else AppImageInstall)(installer, scratch)

    stub = UpdateFeed(install.version)
    stub.add_asset(install.asset, path=installer)
    stub.add_checksums()
    # A Trash of its own, where the old bundle goes (macOS).
    home = os.path.join(scratch, "home")
    os.makedirs(os.path.join(home, ".Trash"))
    relaunched = 0
    app = None
    try:
        app = App(install.binary, home=home, label="old copy",
                  extra_env={"GITBOLT_TEST_UPDATE_FEED": stub.feed_url,
                             "GITBOLT_TEST_APP_VERSION": OLD_VERSION,
                             "QT_QPA_PLATFORM": install.platform})
        apps.append(app)
        app.wait_bridge(timeout=60)
        state = app.state()
        old_pid = state.get("pid")
        if state.get("updates") != "test-feed":
            raise Failure(f"updates gate {state.get('updates')!r}")
        update_dir = state.get("updateDir", "")
        if os.path.realpath(os.path.dirname(update_dir)) != os.path.realpath(app.runtime_dir) \
                or not os.path.basename(update_dir).startswith("gitbolt-updates-"):
            raise Failure(f"downloads go to {update_dir!r}, not a folder of the session's own "
                          f"in its TMPDIR {app.runtime_dir!r}")

        app.ok("trigger help.check-for-updates")
        state = app.wait_until(lambda s: s.get("updateDialog") in ("available", "not-installable"),
                               "the dialog's offer", timeout=30)
        if state["updateDialog"] != "available":
            raise Failure(f"this copy can't install: {state.get('updateDialogText')!r}")
        app.ok("click updateDialog.install")

        # Download, checks, staging, the helper: then the old copy quits.
        deadline = time.monotonic() + 240
        while app.proc.poll() is None:
            if time.monotonic() > deadline:
                raise Failure(f"the old copy never quit; last state: {app.state()}")
            try:
                state = app.state()
                if state.get("updateDialog") == "failed":
                    raise Failure(f"the install failed: {state.get('updateDialogText')!r}")
            except (OSError, ValueError):
                pass    # quitting
            time.sleep(0.5)
        if app.proc.returncode != 0:
            raise Failure(f"the old copy exited with {app.proc.returncode}")

        # The new copy, started by the helper, answers the same bridge.
        deadline = time.monotonic() + 120
        new_state = None
        while time.monotonic() < deadline:
            try:
                reply = app.cmd("dump-state", timeout=5)
                if reply.get("ok") and reply.get("pid") != old_pid:
                    new_state = reply
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.5)
        if new_state is None:
            raise Failure("no relaunched GitBolt answered the bridge" + install_log(app))
        relaunched = int(new_state["pid"])
        if os.path.realpath(new_state.get("appPath", "")) != os.path.realpath(install.binary) \
                and sys.platform == "darwin":
            raise Failure(f"the relaunched copy runs from {new_state.get('appPath')!r}, "
                          f"not {install.binary!r}")
        install.check_replaced(home)
        # Downloaded, staged and logged in the session's own folder in
        # its TMPDIR, never in the user's cache (macOS's CacheLocation
        # ignores HOME); the relaunched copy uses the same folder.
        if not os.path.isfile(os.path.join(update_dir, "install.log")):
            raise Failure(f"no install.log in {update_dir}")
        if new_state.get("updateDir") != update_dir:
            raise Failure(f"the relaunched copy downloads into {new_state.get('updateDir')!r}, "
                          f"not {update_dir!r}: it can't read the helper's install-result")
        # The helper's install-result, read at start: the new copy says
        # the update went through (the status bar; dump-state's
        # statusMessage), and the result is gone, said once.
        want = f"GitBolt was updated to {install.version}."
        deadline = time.monotonic() + 15
        said = new_state.get("statusMessage")
        while said != want and time.monotonic() < deadline:
            time.sleep(0.3)
            said = app.cmd("dump-state", timeout=5).get("statusMessage")
        if said != want:
            raise Failure(f"the relaunched copy's status bar says {said!r}, not {want!r}")
        if os.path.exists(os.path.join(update_dir, "install-result")):
            raise Failure("install-result is still there after the relaunched copy read it")

        app.cmd("quit")
        deadline = time.monotonic() + 30
        while alive(relaunched) and time.monotonic() < deadline:
            time.sleep(0.3)
        if alive(relaunched):
            raise Failure("the relaunched copy didn't quit")
        relaunched = 0
    except Failure as failure:
        raise Failure(f"{failure}{install_log(app)}") from None
    finally:
        stub.close()
        if relaunched and alive(relaunched):
            os.kill(relaunched, signal.SIGKILL)


if __name__ == "__main__":
    if not os.environ.get("GITBOLT_E2E_INSTALL_APP"):
        print("SKIP: set GITBOLT_E2E_INSTALL_APP to a packaged GitBolt installer "
              "(.dmg on macOS, .AppImage on Linux)")
        sys.exit(SKIP)
    if sys.platform not in ("darwin", "linux"):
        print("SKIP: macOS and Linux only")
        sys.exit(SKIP)
    run(test)
