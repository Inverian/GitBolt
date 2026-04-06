# Tools

Development, build, debugging, and automation helpers for working on GitBolt. These are internal tools for contributors, **not** part of the shipped application.

Everything here is optional — the main build and run instructions in [`docs/BUILDING.md`](../docs/BUILDING.md) don't depend on any of it. These scripts exist to make the inner development loop faster, to unblock end-to-end testing in tricky environments, and to automate repetitive tasks.

---

## Index

| Tool | Platform | Purpose |
|---|---|---|
| [`run-dev.sh`](#run-devsh) | macOS | Kill + rebuild + launch GitBolt in one command |
| [`click.py`](#clickpy) | macOS | Synthetic mouse click via Quartz CGEvents (bypasses overlay apps) |

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

1. Makes sure Homebrew's `brew shellenv` is loaded, so `cmake`, `ninja`, Qt, and libgit2 are all on `PATH` even if you ran the script from a non-login shell.
2. Kills any running GitBolt instance with `pkill -9` (including orphaned ones that Launch Services may be keeping alive).
3. Configures the build directory if it hasn't been configured yet (`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`). Subsequent runs skip this step.
4. Incrementally builds with `cmake --build build --parallel`.
5. Launches via `open -n ".../GitBolt.app" --args <repo>` so the app goes through Launch Services and gets a proper Dock icon, Cmd-Tab entry, and NSApplicationActivationPolicy.

### Why you want this

During development, the cycle "make a code change → stop the running app → rebuild → relaunch with a test repo" is what you do dozens of times per hour. This script compresses it to a single command. Launching directly from `.app/Contents/MacOS/GitBolt` works but skips Launch Services, which means the window has no Dock icon and `osascript tell application "GitBolt" to activate` can't find it — both frustrating paper cuts that `open -n --args` fixes.

### Requirements

- Homebrew with `cmake`, `qt`, `libgit2`, `pkg-config`, and `ninja` installed (see [`docs/BUILDING.md`](../docs/BUILDING.md))
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

## Adding new tools

If you write a helper that makes development, debugging, or testing easier, consider dropping it in this folder and adding a section above. Guidelines:

- **One tool, one job.** Don't bundle unrelated features into a single script.
- **Shebang + comment header.** Every script should be runnable as `./tools/<name>` (no `bash` prefix) and have a comment at the top explaining what it does and why it exists.
- **Document platform requirements.** We build on macOS, Windows, and Linux — make it clear which platform(s) your tool targets.
- **Explain *why*, not just *what*.** A script that `touch`es a file is obvious, but the reason you need to is not. The "Why this tool exists" section in `click.py` above is a good template — describe the original problem that motivated the tool so future contributors can tell if it's still relevant.
- **No secrets.** Never commit API keys, tokens, passwords, or user-specific paths.
- **No shipped behavior.** Tools here are for developers. They don't become part of the installed GitBolt application.
