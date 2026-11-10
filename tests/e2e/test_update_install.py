#!/usr/bin/env python3
"""E2E: a real package updates in place, end to end.

What is installed, and what the loopback feed offers:
- GITBOLT_E2E_INSTALL_APP (macOS): a .dmg, whose GitBolt.app the test
  copies into a scratch "Applications" folder, with a marker file
  planted inside the bundle. CI's just-built package; locally,
  `cpack -G DragNDrop -C Release` in the build. Nothing outside scratch
  folders is touched (no /Applications, no real Trash).
- GITBOLT_E2E_INSTALLED_APP (Windows, Linux): the binary of a GitBolt
  that CI installed from its package, system-wide (<dir>\\bin\\GitBolt.exe,
  /usr/bin/gitbolt). The test updates that install in place, so it
  changes the system: CI's throwaway runners only.
- GITBOLT_E2E_UPDATE_APP: the installer the feed offers, named as its
  release asset; GITBOLT_E2E_INSTALL_APP by default (the package updates
  itself). CI also has the latest published release update to the
  build, and then sets GITBOLT_E2E_PUBLISHED_VERSION to its version.
Skipped (exit 77) without them.

1. The installed copy runs as version 0.1.0 (GITBOLT_TEST_APP_VERSION)
   against a loopback stub feed (update_stub.py) whose release is the
   offered installer's version, with that installer and its
   SHA256SUMS.txt.
2. Help › Check for Updates › Install and Restart, through the bridge:
   the app downloads and checks the installer, gets it ready, starts the
   helper and quits.
3. The offered copy replaced it. macOS: the marker is gone from the
   bundle, the old bundle, marker and all, is in the scratch HOME's
   .Trash as GitBolt-0.1.0.app, and the new bundle has the offer's
   version and main binary, and verifies. Linux: dpkg has the offer's
   version, and the offer's binary is installed. Windows: the CI step
   checks the install (registry, version resource).
4. The relaunched app answers the same bridge (the helper keeps
   GITBOLT_INSTANCE_NAME and the bridge's variables), from the same
   path, as a new process. The download, the staging and the helper's
   log were in the session's own folder in its TMPDIR
   (gitbolt-updates-XXXXXX, dump-state's updateDir), not the user's
   cache, and the relaunched copy goes on using that folder
   (GITBOLT_TEST_UPDATE_DIR, inherited through the helper). There it
   reads the helper's install-result and the launch's pending-install
   marker, both gone afterwards, says "GitBolt was updated to <v>.",
   and logs the outcome: from 0.1.0, or from unknown after a 0.9.x
   release, which writes no marker.
5. macOS and Linux: the new binary's --version is the offer's.

The packaged app has no offscreen platform plugin, so it runs on the
real one: cocoa, xcb (CI's xvfb-run) or windows.
"""

from __future__ import annotations

import glob
import hashlib
import os
import plistlib
import re
import subprocess
import sys
import tarfile
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2elib import WINDOWS, App, Failure, kill_pid, pid_alive, run  # noqa: E402
from update_stub import UpdateFeed  # noqa: E402

SKIP = 77   # SKIP_RETURN_CODE in tests/e2e/CMakeLists.txt
OLD_VERSION = "0.1.0"
MARKER = "e2e-old-copy-marker"
# assetNameFor (src/update/Package.cpp), the version captured.
ASSET = {
    "darwin": r"GitBolt-([0-9.]+)-Darwin-arm64\.dmg",
    "win32": r"GitBolt-([0-9.]+)-Windows-AMD64\.exe",
    "linux": r"gitbolt_([0-9.]+)_amd64\.deb",
}


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


class MountedDmg:
    """A DMG attached read-only in scratch, for a `with`."""

    def __init__(self, dmg: str, scratch: str) -> None:
        self.dmg = dmg
        self.mount = tempfile.mkdtemp(prefix="gb-mnt-", dir=scratch)

    def __enter__(self) -> str:
        hdiutil("attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint", self.mount, self.dmg)
        return self.mount

    def __exit__(self, *_exc) -> None:
        hdiutil("detach", self.mount)


def sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def deb_member_sha256(deb: str, member: str) -> str:
    """SHA-256 of one file of a .deb's data, streamed from dpkg-deb."""
    proc = subprocess.Popen(["dpkg-deb", "--fsys-tarfile", deb], stdout=subprocess.PIPE)
    try:
        with tarfile.open(fileobj=proc.stdout, mode="r|") as tar:
            for info in tar:
                if info.name == member:
                    digest = hashlib.sha256()
                    with tar.extractfile(info) as fh:
                        for block in iter(lambda: fh.read(1 << 20), b""):
                            digest.update(block)
                    return digest.hexdigest()
    finally:
        proc.stdout.close()
        proc.wait()
    raise Failure(f"{deb} has no {member}")


class Offer:
    """The installer the feed offers: its asset name and version, and
    the SHA-256 of its main binary (macOS, Linux)."""

    def __init__(self, path: str, scratch: str) -> None:
        self.path = os.path.abspath(path)
        self.name = os.path.basename(path)
        match = re.fullmatch(ASSET[sys.platform], self.name)
        if not match:
            raise Failure(f"{self.name} isn't named as this platform's release asset")
        self.version = match.group(1)
        self.binary_sha256 = None
        if sys.platform == "darwin":
            with MountedDmg(self.path, scratch) as mount:
                self.binary_sha256 = sha256(os.path.join(mount, "GitBolt.app", "Contents", "MacOS", "GitBolt"))
        elif sys.platform == "linux":
            self.binary_sha256 = deb_member_sha256(self.path, "./usr/lib/gitbolt/gitbolt")


class MacInstall:
    """The DMG's GitBolt.app in a scratch Applications folder."""

    platform = "cocoa"
    package = "dmg"

    def __init__(self, dmg: str, scratch: str) -> None:
        with MountedDmg(dmg, scratch) as mount:
            source = os.path.join(mount, "GitBolt.app")
            if not os.path.isdir(source):
                raise Failure(f"no GitBolt.app in {dmg}")
            self.folder = os.path.join(scratch, "Applications")
            os.makedirs(self.folder)
            self.bundle = os.path.join(self.folder, "GitBolt.app")
            subprocess.run(["ditto", source, self.bundle], check=True)
        self.binary = os.path.join(self.bundle, "Contents", "MacOS", "GitBolt")
        self.app_path = self.binary
        self.marker = os.path.join(self.bundle, "Contents", "Resources", MARKER)
        with open(self.marker, "w") as fh:
            fh.write("the old copy\n")

    @staticmethod
    def bundle_version(bundle: str) -> str:
        with open(os.path.join(bundle, "Contents", "Info.plist"), "rb") as fh:
            return plistlib.load(fh)["CFBundleShortVersionString"]

    def check_replaced(self, home: str, offer: Offer) -> None:
        if os.path.exists(self.marker):
            raise Failure("the marker is still in the bundle: the old copy wasn't replaced")
        version = self.bundle_version(self.bundle)
        if version != offer.version:
            raise Failure(f"the bundle is version {version}, not {offer.version}")
        if sha256(self.binary) != offer.binary_sha256:
            raise Failure("the bundle's GitBolt isn't the offered DMG's")
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


def dpkg_query(field: str) -> str:
    return subprocess.run(["dpkg-query", "-W", f"-f=${{{field}}}", "gitbolt"],
                          capture_output=True, text=True, check=True).stdout


class DebInstall:
    """The gitbolt package apt installed: /usr/bin/gitbolt, a link to
    /usr/lib/gitbolt/gitbolt."""

    platform = "xcb"
    package = "deb"

    def __init__(self, binary: str) -> None:
        self.binary = binary
        self.app_path = os.path.realpath(binary)
        self.start_version = dpkg_query("Version")

    def check_replaced(self, _home: str, offer: Offer) -> None:
        version = dpkg_query("Version")
        if version == self.start_version:
            raise Failure(f"dpkg still has gitbolt {version}: nothing was installed (an "
                          "installed package of the offer's version must be relabelled lower)")
        if version != offer.version:
            raise Failure(f"dpkg has gitbolt {version}, not {offer.version}")
        status = dpkg_query("Status")
        if status != "install ok installed":
            raise Failure(f"gitbolt's dpkg status is {status!r}")
        if sha256(self.app_path) != offer.binary_sha256:
            raise Failure(f"{self.app_path} isn't the offered .deb's")


class WindowsInstall:
    """GitBolt as the NSIS installer put it: <dir>\\bin\\GitBolt.exe."""

    platform = "windows"
    package = "nsis"

    def __init__(self, binary: str) -> None:
        self.binary = binary
        self.app_path = binary

    def check_replaced(self, _home: str, _offer: Offer) -> None:
        pass    # the CI step's Assert-Updated: the registry, the version resource


def same_file(a: str, b: str) -> bool:
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


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


def session_log(app: App, pid: int) -> list[str]:
    """What process `pid` wrote to the test-session log (util/LogFile.h),
    from its session line on: its stderr goes nowhere on Windows."""
    if sys.platform == "darwin":
        folder = os.path.join(app.env["HOME"], "Library", "Logs", "GitBolt")
    elif WINDOWS:
        folder = os.path.join(os.environ["LOCALAPPDATA"], "GitBolt", "logs")
    else:
        folder = os.path.join(app.env["XDG_STATE_HOME"], "GitBolt")
    lines: list[str] = []
    mine = False
    for name in ("GitBolt-test.2.log", "GitBolt-test.1.log", "GitBolt-test.log"):
        try:
            with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for line in text.splitlines():
            if " started: " in line:
                mine = line.endswith(f", pid {pid}, test session")
                if mine:
                    lines = []
            elif mine:
                lines.append(line)
    return lines


def make_install(scratch: str):
    if sys.platform == "darwin":
        return MacInstall(os.path.abspath(os.environ["GITBOLT_E2E_INSTALL_APP"]), scratch)
    binary = os.environ["GITBOLT_E2E_INSTALLED_APP"]
    return WindowsInstall(binary) if WINDOWS else DebInstall(binary)


def test(_binary: str, scratch: str, apps: list) -> None:
    install = make_install(scratch)
    offer = Offer(os.environ.get("GITBOLT_E2E_UPDATE_APP") or os.environ["GITBOLT_E2E_INSTALL_APP"],
                  scratch)
    # 0.9.x releases leave no pending-install marker to say where from.
    published = os.environ.get("GITBOLT_E2E_PUBLISHED_VERSION", "")
    from_version = "unknown" if published.startswith("0.") else OLD_VERSION

    stub = UpdateFeed(offer.version)
    stub.add_asset(offer.name, path=offer.path)
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
        if not same_file(os.path.dirname(update_dir), app.runtime_dir) \
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
        if not same_file(new_state.get("appPath", ""), install.app_path):
            raise Failure(f"the relaunched copy runs from {new_state.get('appPath')!r}, "
                          f"not {install.app_path!r}")
        install.check_replaced(home, offer)
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
        want = f"GitBolt was updated to {offer.version}."
        deadline = time.monotonic() + 15
        said = new_state.get("statusMessage")
        while said != want and time.monotonic() < deadline:
            time.sleep(0.3)
            said = app.cmd("dump-state", timeout=5).get("statusMessage")
        if said != want:
            raise Failure(f"the relaunched copy's status bar says {said!r}, not {want!r}")
        for name in ("install-result", "pending-install"):
            if os.path.exists(os.path.join(update_dir, name)):
                raise Failure(f"{name} is still there after the relaunched copy read it")
        outcome = f"Update: ok (install, none), {from_version} to {offer.version}, {install.package}"
        logged = session_log(app, relaunched)
        if not any(line.endswith(outcome) for line in logged):
            raise Failure(f"the relaunched copy didn't log {outcome!r}: " + "\n".join(logged[-20:]))

        app.cmd("quit")
        deadline = time.monotonic() + 30
        while pid_alive(relaunched) and time.monotonic() < deadline:
            time.sleep(0.3)
        if pid_alive(relaunched):
            raise Failure("the relaunched copy didn't quit")
        relaunched = 0

        if not WINDOWS:
            env = dict(app.env)
            env.pop("GITBOLT_TEST_BRIDGE")
            out = subprocess.run([install.binary, "--version"], env=env, capture_output=True,
                                 text=True, errors="replace", timeout=60).stdout
            if f"GitBolt {offer.version}" not in out.splitlines():
                raise Failure(f"{install.binary} --version says {out.strip()!r}")
    except Failure as failure:
        raise Failure(f"{failure}{install_log(app)}") from None
    finally:
        stub.close()
        if relaunched and pid_alive(relaunched):
            kill_pid(relaunched)


if __name__ == "__main__":
    inputs = {"darwin": "GITBOLT_E2E_INSTALL_APP", "linux": "GITBOLT_E2E_INSTALLED_APP",
              "win32": "GITBOLT_E2E_INSTALLED_APP"}.get(sys.platform)
    if not inputs or not os.environ.get(inputs):
        print("SKIP: set GITBOLT_E2E_INSTALL_APP to a packaged GitBolt (.dmg, macOS), or "
              "GITBOLT_E2E_INSTALLED_APP to an installed one and GITBOLT_E2E_UPDATE_APP "
              "to the installer to offer (Windows, Linux)")
        sys.exit(SKIP)
    if sys.platform != "darwin" and not os.environ.get("GITBOLT_E2E_UPDATE_APP"):
        print("SKIP: GITBOLT_E2E_INSTALLED_APP needs GITBOLT_E2E_UPDATE_APP, the installer to offer")
        sys.exit(SKIP)
    run(test)
