# Tools

Development, build, debugging, and automation helpers for working on GitBolt. These are internal tools for contributors, **not** part of the shipped application.

Everything here is optional — the main build and run instructions in [`docs/BUILDING.md`](../docs/BUILDING.md) don't depend on any of it. These scripts exist to make the inner development loop faster, to unblock end-to-end testing in tricky environments, and to automate repetitive tasks.

---

## Index

| Tool | Platform | Purpose |
|---|---|---|
| [`run-dev.sh`](#run-devsh) | macOS | Kill + rebuild + launch GitBolt in one command |
| [`click.py`](#clickpy) | macOS | Synthetic mouse click via Quartz CGEvents (bypasses overlay apps) |
| [`check-ci-pins.py`](#check-ci-pinspy) | any | Find newer releases of what the builds pin, and advisories against them or the git GitBolt runs |
| [`update-qt-notices.py`](#update-qt-noticespy) | any | Write the licences of the code of others in the Qt that GitBolt ships, for its packages' notices |
| [`symbolize.py`](#symbolizepy) | any | Read a crash report's GitBolt frames with a release's symbol files |

---

## `run-dev.sh`

**The primary development loop helper.** Kills any running GitBolt, rebuilds the Debug target, and relaunches it — optionally with a repository path to open.

### Usage

```bash
tools/run-dev.sh                        # Fresh launch on the Dashboard
tools/run-dev.sh .                      # Open the current directory
tools/run-dev.sh ~/projects/linux       # Open an arbitrary repository
```

### What it does

1. Makes sure Homebrew's `brew shellenv` is loaded, so `cmake`, `ninja` and Qt are all on `PATH` even if you ran the script from a non-login shell.
2. Kills any running GitBolt instance with `pkill -9` (including orphaned ones that Launch Services may be keeping alive).
3. Configures the build directory if it hasn't been configured yet (`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`). Subsequent runs skip this step.
4. Incrementally builds with `cmake --build build --parallel`.
5. Launches via `open -n ".../GitBolt.app" --args <repo>` so the app goes through Launch Services and gets a proper Dock icon, Cmd-Tab entry, and NSApplicationActivationPolicy.

### Why you want this

During development, the cycle "make a code change → stop the running app → rebuild → relaunch with a test repo" is what you do dozens of times per hour. This script compresses it to a single command. Launching directly from `.app/Contents/MacOS/GitBolt` works but skips Launch Services, which means the window has no Dock icon and `osascript tell application "GitBolt" to activate` can't find it — both frustrating paper cuts that `open -n --args` fixes.

### Requirements

- Homebrew with `cmake`, `qt` and `ninja` installed (the first configure downloads and builds the pinned libgit2; see [`docs/BUILDING.md`](../docs/BUILDING.md))
- macOS — the script uses `open`, `pkill`, and Homebrew paths that don't exist on Linux or Windows

---

## `click.py`

**Synthetic mouse click injector** for automated UI testing on macOS. Posts `CGEvent` mouse events directly to the HID event tap, which bypasses application-level click interception.

### Usage

```bash
tools/click.py x y                  # single left click
tools/click.py x y double           # double click (properly marked)
tools/click.py x y triple           # triple click
tools/click.py x y right            # right click
tools/click.py x y move             # move cursor without clicking
tools/click.py x1 y1 drag x2 y2     # press, drag, release
```

Coordinates are in **macOS screen points**, not pixels. See the [calibration](#calibrating-screenshot-pixel--screen-point) section below.

### Why this tool exists

During end-to-end testing of GitBolt we discovered that **BetterDisplay** (a popular menu-bar display manager) installs a fullscreen invisible overlay that intercepts clicks. Our MCP/computer-use tool's click-collision check refused to click "through" it, which blocked every automated UI interaction with GitBolt.

`click.py` bypasses this by calling `CGEventPost(kCGHIDEventTap, ...)` — which injects events at the lowest layer, before any application-level event filtering can see them. This is the same mechanism [cliclick](https://github.com/BlueM/cliclick), xdotool-for-macOS, and most Quartz-based automation tools use.

It's also useful for:

- **Driving Qt apps from CI** where the default `QTest::mouseClick` paths don't reach native widgets correctly
- **Working around AppleScript's limitations** on clicking at arbitrary screen coordinates (which has been broken in various macOS versions with error `-25208`)
- **Reproducing user-reported bugs** that depend on specific click sequences that are tedious to do by hand

### Double-click behavior

Setting `kCGMouseEventClickState` on both the mouse-down and mouse-up events is required for AppKit and Qt to recognize a proper double-click:

```python
# Second click of a double-click pair:
down_event.click_state = 2
up_event.click_state   = 2
```

If you only set the down, or mismatch the count between down and up, the target app sees two independent single-clicks instead of one double-click. `click.py` handles this correctly in its `double` and `triple` modes.

**Known caveat:** some Qt widgets (notably `QListWidget::itemDoubleClicked`) are still picky about synthetic double-clicks even when the CGEvent click state is set correctly. GitBolt's DashboardView uses `QListWidget::itemActivated` instead, which responds to both double-click and Enter and works reliably with synthetic events.

### Calibrating screenshot pixel ↔ screen point

`click.py` takes coordinates in **screen points** (what macOS calls "logical pixels"), but the screenshots captured by MCP clients are in **image pixels** at a downscaled resolution. You need to convert between them.

**One-time calibration:**

```bash
python3 -c "
from Quartz import CGMainDisplayID, CGDisplayBounds
b = CGDisplayBounds(CGMainDisplayID())
print(f'Display: {b.size.width:.0f} x {b.size.height:.0f} points')
"
```

Compare against the screenshot dimensions reported by your MCP client. On a 2560×1440 point display with an MCP screenshot at 1456 wide, the conversion factor is `2560 / 1456 ≈ 1.758×`.

**Converting a click target:**

```bash
# Screenshot pixel (722, 380) with scale factor 1.758
python3 -c "ssx, ssy = 722, 380; print(round(ssx*1.758), round(ssy*1.758))"
# → 1269 668
tools/click.py 1269 668
```

### Requirements

- Python 3 (the `/usr/bin/python3` that ships with macOS is fine)
- `pip3 install --user pyobjc-framework-Quartz`
- macOS — this is a Quartz-specific tool and has no Windows or Linux equivalent

### Safety

`click.py` injects events at the lowest level, which means it will click **whatever is under those coordinates**. There is no safety net. Always verify your coordinates against a fresh screenshot before clicking, and prefer using it on non-destructive UI (buttons, table rows) rather than directly on "Delete" confirmations.

---

## `check-ci-pins.py`

What GitBolt's builds pin stays put until someone moves it: CI keeps actions on major tags, and pins Qt, NSIS and what installs Qt (`.github/qt-requirements.*`); the build pins libgit2. A tool CI downloads from a tagged GitHub release (`fetch <url> <sha256>` in a workflow) is checked against the sha256 it had when it was pinned; CI downloads none today, since the AppImage's tools went with the AppImage. That keeps a re-uploaded asset out of our installers, but nothing bumps the pins, and nothing says when one of them, or the git GitBolt runs, has a security advisory. This script does both:

- **Pins:** the newest tagged release of each tool (never a `continuous` build, which is rebuilt in place, and the highest version where a project also releases fixes for an older major), with the new hash; a new major of each action; a newer patch release of libgit2's series, Qt's series (and whether its open-source support has ended) and NSIS; a release of aqtinstall that has the commit CI pins. A release on GitHub counts once it has been out 14 days, the wait `.github/qt-requirements.in` sets for the lock, so a bad upload can be noticed and pulled first; Qt's and NSIS's count at once, as nothing else here would show their security fixes. An action pinned other than by major tag (`@vN`) is reported, since it can't be followed. CI's Python is a minor version (`3.14`) that setup-python keeps on its newest patch release, so it isn't followed.
- **Advisories**, reported as soon as they're published: libgit2's, git's, Git for Windows', Git LFS's, py7zr's and aqtinstall's since the date in a `check-ci-pins: <owner>/<repo> advisories reviewed <YYYY-MM-DD>` comment (in `cmake/Libgit2.cmake`, `src/git/GitVersion.cpp` and `.github/qt-requirements.in`; move it on once you've read what came after it, and the libgit2 and git ones must be there); and what GitHub's reviewed advisory database lists against every locked Python package (not their newest releases: the lock is refreshed on purpose).

`.github/workflows/dependency-check.yml` runs it every Monday; a failed run is GitHub's email to the owner. A bump that has to wait is accepted until a date by a comment in any file it reads (`# check-ci-pins: accept <key> until <YYYY-MM-DD>`, the key it prints in brackets).

### Usage

```bash
tools/check-ci-pins.py            # report; exits 1 if anything is behind or has an advisory, 2 if something couldn't be checked
tools/check-ci-pins.py --write    # also rewrite the fetch and uses: pins in .github/workflows
tools/check-ci-pins.py --offline  # only list the pins it finds
```

A bumped tool or action can change the installers, so run the workflow on the branch (Actions → CI → Run workflow builds every installer) before merging. libgit2, Qt, NSIS and the Python lock are bumped by hand (the report says where; `.github/qt-requirements.in` says how to refresh the lock). The ubuntu and debian image digests the DEB is tested in are not followed; the script prints a reminder.

### Requirements

- Python 3 and network access. It calls the GitHub API without a token; the advisory lookups take more than its 60 requests an hour, so set `GITHUB_TOKEN` (the workflow passes its own).

---

## Adding new tools

If you write a helper that makes development, debugging, or testing easier, consider dropping it in this folder and adding a section above. Guidelines:

- **One tool, one job.** Don't bundle unrelated features into a single script.
- **Shebang + comment header.** Every script should be runnable as `./tools/<name>` (no `bash` prefix) and have a comment at the top explaining what it does and why it exists.
- **Document platform requirements.** We build on macOS, Windows, and Linux — make it clear which platform(s) your tool targets.
- **Explain *why*, not just *what*.** A script that `touch`es a file is obvious, but the reason you need to is not. The "Why this tool exists" section in `click.py` above is a good template — describe the original problem that motivated the tool so future contributors can tell if it's still relevant.
- **No secrets.** Never commit API keys, tokens, passwords, or user-specific paths.
- **No shipped behavior.** Tools here are for developers. They don't become part of the installed GitBolt application.

<!-- End-to-end commit workflow test marker -->

---

## `update-qt-notices.py`

Writes `resources/licenses/qt-<version>-third-party.txt`: the copyright notices and licence texts of the code of others compiled into the Qt libraries and plugins that GitBolt's packages ship, from Qt's own `qt_attribution.json` files (and one `REUSE.toml`) at the version's tag of qt/qtbase and qt/qtsvg. `cmake/Notices.cmake` puts the file in every package's notices, and CI configures with `GITBOLT_STRICT_NOTICES=ON`, so a Qt update fails until the file for its version is there.

### Usage

```bash
python3 tools/update-qt-notices.py 6.11.3            # write the file for Qt 6.11.3
python3 tools/update-qt-notices.py 6.11.3 --check    # exit 1 if the committed file is out of date
```

It exits 1 when Qt's metadata needs a look: a component it lists is gone, or a new one turned up in a folder that goes into a shipped library. Add the new one to `SHIPPED` (with what it is used in) or `NOT_SHIPPED` (with why), then run it again. Standard library only; set `GITHUB_TOKEN` if the GitHub API's rate limit gets in the way.

The other texts in `resources/licenses/` are fetched by hand at a pinned version; `resources/licenses/SOURCES.md` says from where.

---

## `symbolize.py`

Reads the GitBolt frames of a crash report with the symbol files of the release that crashed. A report's call stack has GitBolt's frames as positions only (see [PRIVACY.md](../PRIVACY.md#crash-reports)); this turns them into functions, files and lines. Each release publishes `GitBolt-<version>-<platform>-symbols.zip` beside its installers: the macOS dSYM, the Windows `GitBolt.exe` with its `GitBolt.pdb`, or Linux's `gitbolt.debug`. Reports carry a `Build ID:` line (the crash summary's `build_id` tag), which must match the symbol files.

### Usage

```bash
python3 tools/symbolize.py crash.log                    # prints the gh command that downloads the right zip
python3 tools/symbolize.py crash.log GitBolt-1.0.0-Darwin-arm64-symbols.zip
python3 tools/symbolize.py event.json symbols/          # a GlitchTip event's JSON; a folder of the zip's files
python3 tools/symbolize.py --id build/src/app/GitBolt.app/Contents/MacOS/GitBolt   # a binary's build ID
python3 tools/symbolize.py --check SYMBOLS SHIPPED      # CI: the symbol files belong to the shipped binary
```

The report is a `crash.log` (as the Crash Detected box or an issue shows it) or a GlitchTip event's JSON, as sent or from GlitchTip's API. The symbols are the release's zip, a folder of its files, a `.dSYM` or a binary. A build ID that doesn't match exits 1; `--force` symbolizes anyway. A report without a build ID (0.9.x) gets a warning, and the tool trusts its version, OS and architecture. `--tool auto|llvm-symbolizer|atos|addr2line` picks the lookup tool; `auto` takes llvm-symbolizer when it finds one, else atos for macOS and addr2line for Linux.

Only GitBolt's own frames are looked up. Qt's and the system's frames keep what the report says: a function name at most on macOS and Linux, a position only on Windows. Reading a Qt frame needs Qt's own debug info for the Qt version the release was built with (Qt's installer offers it as debug information files), which GitBolt's symbol files don't include; a system frame needs the system's symbols.

### Requirements

- Python 3.9 or newer, standard library only.
- macOS reports: `atos` (Xcode Command Line Tools). Windows reports: `llvm-symbolizer` (`brew install llvm`, or the LLVM installer on Windows). Linux reports: `addr2line` (binutils) or llvm-symbolizer.
- `gh` (GitHub CLI) to download a release's symbol files.
