# Driving GitBolt from an AI agent — friction log & proposed test bridge

This document records where UI automation (AppleScript / accessibility
tree / synthetic clicks) breaks down when an AI agent tries to
exercise GitBolt end-to-end, and proposes a built-in test interface so
agents can verify features without fighting the GUI layer.

## Observed friction points

Logged while QA-testing the June 2026 feature batch (blame, worktrees,
merge-conflict resolver, clone cancel, hunk staging). Each is a place
an agent reliably gets stuck:

1. **Qt in-window menu bar is invisible to the standard AX path.**
   `menu bar 1 of window 1` throws -1728. The menu bar is reachable
   only as a generic `AXMenuBar` child found by iterating
   `UI elements of window 1`. `whose name is "X"` specifiers on those
   children also throw intermittently; only manual `repeat` iteration
   works.

2. **An open menu becomes "window 1".** While a menu is dropped down,
   it is the frontmost AX window, so any code that reads
   `position of window 1` (e.g. a coordinate helper) targets the menu,
   not the main window — coordinates silently land in the wrong place.
   `AXMenuBar` also vanishes from the (real) window's children until
   the menu closes.

3. **Menu coordinates must be read, not estimated.** Hard-coding
   `winX + 152` for a menu lands one item off (opened Navigate instead
   of Commands). Item centers have to be read from each menubar item's
   `position`/`size` — which only works when no menu is open (see #2),
   a chicken-and-egg.

4. **Qt table / list row selection ignores synthetic input.**
   `select row N`, `perform action "AXPress"` on a cell, `cliclick`
   on the row's real screen center, and "focus table + arrow key" all
   leave `selectedRows()` empty. Anything gated on a selected row
   (worktree Lock/Remove, file-list staging) can't be driven. Real
   mouse input works fine — this is purely an automation gap.

5. **Dialog Cmd+W needs the dialog explicitly raised.** A modeless
   dialog isn't automatically key, so `keystroke "w" using command down`
   goes to the main window. `perform action "AXRaise"` on the dialog
   first fixes it.

6. **BetterDisplay overlay blocks computer-use coordinate clicks.**
   The MCP computer-use guard refuses clicks that land on an invisible
   BetterDisplay window over parts of the screen. `cliclick` bypasses
   that guard but then hits #4 for table rows anyway.

Net effect: menu-driven actions are *barely* drivable (read coords with
no menu open, then click), and anything needing a table-row selection
is **not** drivable from automation at all.

## Proposed built-in: an environment-gated test bridge

Add a control channel, active only when `GITBOLT_TEST_BRIDGE=1` (or a
`--test-bridge` flag), so it never ships in normal use. A `QLocalServer`
on a fixed socket name (or a line-reader on stdin) accepting newline
commands:

| Command | Effect |
|---|---|
| `list-actions` | Dump every `QAction` objectName + text + enabled state |
| `trigger <objectName>` | Invoke a QAction (covers all menu/toolbar items) |
| `list-widgets` | Dump objectNames of live top-level widgets / dialogs |
| `select-row <widgetObjectName> <n>` | Drive a model-view selection directly |
| `click <objectName>` | `click()` a named QPushButton / QToolButton |
| `dump-state` | JSON: open repo path, repo state (merging/rebasing), current dialog, conflict count |
| `screenshot <path>` | `grab()` the focused window to a PNG (no screencapture race) |

Implementation notes:
- Requires giving the relevant `QAction`s, dialogs, and views stable
  `setObjectName()` values (most don't have them yet — that work is the
  bulk of it and is independently good for QSS/styling and crash logs).
- The bridge lives in `app/` behind the env gate; it resolves objects
  via `qApp->findChildren<QAction*>()` / `QApplication::topLevelWidgets()`.
- `dump-state` is the highest-value single command: it lets an agent
  assert outcomes (e.g. "repo is MERGING", "3 conflicts", "commit has 2
  parents") without screenshots or AX traversal.

## The bridge (implemented)

`src/app/TestBridge.{h,cpp}` implements the design above; `tools/bridge.py`
is the client. Enable it by setting the env var at launch:

```bash
GITBOLT_TEST_BRIDGE=1 ./build/src/app/GitBolt.app/Contents/MacOS/GitBolt /path/to/repo &
tools/bridge.py dump-state
tools/bridge.py list-actions          # every menu/toolbar action + slug
tools/bridge.py trigger commands/resolve-conflicts
tools/bridge.py list-widgets          # windows (a message box's text too), buttons, views, editors (with classes)
tools/bridge.py select-row QTableView:0 0     # the AX-impossible operation
tools/bridge.py select-row commit.unstagedList 0,2,5   # multi-row (ExtendedSelection lists)
tools/bridge.py select-row settings.nav 0/5    # child row 5 of top-level row 0 (Settings › GitBolt › Usage Statistics)
tools/bridge.py click "Lock/Unlock"
tools/bridge.py select-item toolbar.branchCombo feature/login   # exact text, or '#N'
tools/bridge.py type CommitMessageEdit:0 "subject\n\nbody line"   # \n \t \\ decoded
tools/bridge.py fire-timer periodicFetchTimer # the auto-fetch tick now, not in N minutes
tools/bridge.py file-open /path/to/repo/sub   # a QFileOpenEvent, as a Dock drop or `open -a` sends
tools/bridge.py service-open /path/to/repo    # Finder's Open in GitBolt (macOS, cocoa only)
tools/bridge.py minimize              # the main window to the Dock; dump-state's `minimized` says
tools/bridge.py screenshot /tmp/state.png     # in-process grab(), no overlay issues
tools/bridge.py quit                  # MainWindow::close() — clean exit for relaunch tests
```

On Windows the bridge listens on the named pipe
`\\.\pipe\<name>`, where `<name>` is `GITBOLT_TEST_BRIDGE`'s value
(`gitbolt-test-bridge` for `1`), not on a socket in `$TMPDIR`.
`tools/bridge.py` speaks Unix sockets only; the e2e harness
(`tests/e2e/e2elib.py`) has the Windows client.

The history table is `log.view`, so `select-row log.view N` selects a
commit. The Cherry-Pick and Revert dialogs' fields are
`cherryPick.commit` and `revert.commit`, the merge parent's combo
`cherryPick.parent` and `revert.parent`, and the revert's note
`revert.note` (`tests/e2e/test_revert_commit.py`).

`dump-state` also reports `headOid`, `theme`, `logRows`,
`statusMessage` and `opIndicator` (the toolbar's inline op label:
"Merging topic…", "✓ Fetch complete.", "✗ fetch failed: …") so a
harness can assert "HEAD moved", "Dark persisted", "the log loaded",
"the fetch failed" and "the merge is still running" without shelling
out or screenshotting, and `sequencePaused`: a cherry-pick or revert
of several commits is paused between two of them (`repoState` is then
`cherry-pick` or `revert`), with `sequenceSkippable` saying whether
git's `--skip` would leave out the commit it stopped at (the in-progress
bar offers Skip only then). `desktopFileName` is the desktop entry the app
claims, its app_id on Wayland (`tests/e2e/test_desktop_identity.py`).

Usage statistics (`src/telemetry`, [PRIVACY.md](../PRIVACY.md)) are
never sent from a bridge session. `dump-state` reports `telemetry`, the
gate the session runs under: `compiled-out` in a build without
endpoints (any build configured without `GITBOLT_TELEMETRY_*`),
`inert:test-bridge` in one with them;
the other values are `inert:instance-name`, `inert:headless`,
`inert:ci`, `off:env`, `off:setting` and `on`.
`telemetryIfConfigured` is the gate the same session would get with
endpoints compiled in. Neither is ever `on` under the bridge
(`tests/e2e/test_telemetry_inert.py`). Address the Settings switch as
`settings.usage-statistics`.

In-app updates (`src/update`, `src/ui/UpdateController.h`,
PRIVACY.md's "Update checks") never reach GitHub from a test session.
`dump-state` reports `updates`, the gate main() evaluated:
`compiled-out` (a build configured without `GITBOLT_UPDATES=ON`,
outside a test session, so never under the bridge), `off:test-session`
(any build in a test session without a test feed),
`off:test-feed-refused` (the test variables below didn't pass; the log
says why), `test-feed`, and `on` (an official build outside a test
session; never under the bridge). Two variables, read only in a test
session, point the updater at a loopback stub
(`tests/e2e/update_stub.py`):

- `GITBOLT_TEST_UPDATE_FEED`: the feed URL, plain `http://` to
  `127.0.0.1` or `[::1]` only (not `localhost`, not https); it turns
  the updater on, with loopback downloads allowed.
- `GITBOLT_TEST_APP_VERSION`: the version the updater takes the running
  app to be, e.g. `0.1.0`.

A `test-feed` session downloads into a folder of its own in its
`TMPDIR`, `gitbolt-updates-XXXXXX` (0700, never the shared
`gitbolt-updates`), which `dump-state`'s `updateDir` names: the
download, the staging, `install-helper.sh`, `install.log`, the
helper's `install-result` (one line, `<status> <version> <reason>`)
and the `pending-install` marker the launch leaves (one line,
`<from> <to> <unix seconds>`) go there. The app hands it on in
`GITBOLT_TEST_UPDATE_DIR`, so the copy the install helper relaunches
uses the same folder (and reads `install-result` and
`pending-install` from it once, at startup, and deletes them); a
folder handed on that isn't a private one of this user's in `TMPDIR`
is ignored and a new one made. Each attempt's outcome is one log line,
`Update: <status> (<stage>, <reason>), <from> to <to>, <package>`,
for example `Update: ok (install, none), 0.1.0 to 1.0.0, dmg`.

The rest of `dump-state`'s update keys: `updateTimerActive` (the daily
check is scheduled: never in a test session), `updateDialog` (while
the dialog is up: `checking`, `up-to-date`, `available`,
`not-installable`, `downloading`, `preparing`, `failed`, `restarting`,
`quit-needed` or `declined`, Windows' UAC having said no) with
`updateDialogText` (every line it shows but the notes), `updateNotice`
(the notice bar's text while it shows), and
`pid` and `appPath` (which process answers, from where: an install test
tells the relaunched copy from the one it replaced). After an update,
`statusMessage` is "GitBolt was updated to <v>.". The dialog's buttons
are `updateDialog.install`, `.skip`, `.later`, `.viewOnGitHub`,
`.openReleasePage`, `.close`, `.cancel`, `.retry` and `.quit`; the bar's
`updateNotice.view` and `updateNotice.dismiss`; the Settings switch
`settings.check-for-updates`.

`fire-timer` emits a running `QTimer`'s timeout (found by
`objectName`; a stopped timer is an error, since it could never fire,
unless the timer has the `fireWhileStopped` property). It's how a test
reaches work the app only does every few minutes. The one timer with
`fireWhileStopped` is a `test-feed` session's `updateCheckTimer`: a
test session never schedules the daily update check, yet a test fires
it to see what an automatic find does (the notice bar). It asks only
when the last answer is a day old, so a second fire the same day asks
nothing.

The "Crash Detected" box that a crash leaves for the next launch never
shows in a test session (bridge, instance name, offscreen or CI):
`~/.gitbolt/crash.log` is the user's, and stays for their next launch.
A test of the box sets `GITBOLT_TEST_CRASH_BOX=1`, and only with a
`HOME` of its own with a `crash.log` planted in it
(`tests/e2e/test_crash_box_handoff.py`). The box comes up over the
window once the bridge and the single-instance listener are up, so
`list-widgets` finds it by its `objectName`, `app.crashDetected`
(macOS message boxes have no title), and `click OK` dismisses it.

A test session that crashes writes its report to
`~/.gitbolt/crash-test.log`, never `crash.log`, and nothing reads or
deletes it. On macOS it also stays out of the system's crash reporter:
no "GitBolt quit unexpectedly" box on the screen and no `GitBolt-*.ips`
in `~/Library/Logs/DiagnosticReports`, since the handler of a test
session takes `EXC_CRASH` itself (`util::CrashHandler::install`). The
processes it starts (git, hooks, ssh, the terminal's shell) don't
inherit that port: one that crashes is reported by macOS as usual. A
user's session keeps macOS's crash reporting.

A test session also writes its log to `GitBolt-test.log`, never
`GitBolt.log`, in the log folder (`~/Library/Logs/GitBolt`,
`%LOCALAPPDATA%\GitBolt\logs`, `$XDG_STATE_HOME/GitBolt`). The e2e
harness gives each app an `XDG_STATE_HOME` of its own; on macOS the
folder follows the test's `HOME`, and on Windows it is the real
profile's.

Addressing:
- **Actions** by `objectName` (every action has one — see below), by
  menu-path slug (`commands/merge-branches`), or trailing suffix
  (`merge-branches`); `trigger`/`click` are queued so an action that
  opens a modal returns immediately — assert with `dump-state`.
- **Buttons** by visible text (case-insensitive, mnemonics/`...` stripped)
  with a prefix fallback so `Commit` matches the live-count `Commit (1)`.
- **Combo boxes** (`select-item`) by `objectName` or `QComboBox[:index]`;
  the item by exact text or `#N`. It also emits `activated` /
  `textActivated` (queued), because code that must react only to a real
  user pick — the toolbar branch switcher checks out on `activated` —
  never sees a bare `setCurrentIndex`. `list-widgets` reports every
  visible combo with its items.
- **Views / editors** by `objectName` or `ClassName[:index]`, matched
  against the whole superclass chain — `QPlainTextEdit:0` resolves a
  `CommitMessageEdit`. Indices count the open modal dialog's widgets
  first, then the active window's, then other dialogs', so `:0` means
  "in the dialog just opened" even while GitBolt isn't the frontmost
  app. Prefer an `objectName` for anything destructive: select-item on
  the toolbar's `toolbar.branchCombo` checks the branch out.

Two real bugs were caught the first time the bridge drove a full
merge→resolve→commit cycle: `Repository::conflictEntries()` read a stale
cached libgit2 index (CLI-side merges were invisible — fixed with
`git_index_read(force)`), and the brittle exact-text button match
(fixed with the prefix fallback). Worktree lock/remove — which no
synthetic input could ever reach because they need a selected table
row — were verified end to end via `select-row`.

## End-to-end tests on Windows

The e2e tests (`ctest --test-dir build -L e2e`) run on Windows too.
There the harness also gives each app `TMP` and `TEMP` of its own
(`QDir::tempPath()`, where the instance lock and the update folder
go) and sets `QT_FORCE_STDERR_LOGGING=1`, because GitBolt is
a GUI-subsystem app whose messages would otherwise go to
`OutputDebugString`. An instance-named session with `XDG_CONFIG_HOME`
set keeps `GitBolt.ini` (and the usage statistics queue beside it) in
`%XDG_CONFIG_HOME%\GitBolt`, not in `%APPDATA%\GitBolt`. `HOME` moves
only git's and libgit2's config: `QDir::homePath()` (`~/.gitbolt`,
`crash-test.log`) and the log folder are the real profile's.

What skips on Windows, with the reason printed:

- Whole tests (exit 77): `test_git_notice` (the login shell's git),
  `test_home_isolated` (`QDir::homePath()` ignores `HOME`) and
  `test_crash_box_handoff` (it would read and delete the real
  `~/.gitbolt/crash.log`).
- Parts of tests ("skipped on Windows: ..."): `test_commit_hooks`' login
  shell `PATH` case and FIFO-held reopen, `test_file_open` part 3 (the
  `$HOME` ceiling), `test_instance_lock`'s live owner without its
  socket, the held-open checks of `test_rebase_progress` section 3,
  and `test_remote_op_leave_repo` section 7 (the FIFO gate).
- Not registered: `test_instance_busy_owner` and
  `test_launchservices_open` (macOS only; the first one's 64 KB
  argument is also over Windows' 32,767-character command line), and
  `test_update_install`, which CI runs directly in the NSIS smoke step.

## The update tests in CI

`tests/e2e/test_update_install.py` updates a real package in place,
through the bridge: Help › Check for updates, then Install and
Restart, against the loopback feed. Its inputs:

- `GITBOLT_E2E_INSTALL_APP` (macOS): a DMG, whose GitBolt.app the test
  copies into a scratch Applications folder.
- `GITBOLT_E2E_INSTALLED_APP` (Windows, Linux): a GitBolt that CI
  installed system-wide (`<dir>\bin\GitBolt.exe`, `/usr/bin/gitbolt`).
  The test changes the system, so it is for throwaway runners only.
- `GITBOLT_E2E_UPDATE_APP`: the installer the feed offers, named as its
  release asset; `GITBOLT_E2E_INSTALL_APP` by default.
- `GITBOLT_E2E_PUBLISHED_VERSION`: set when the installed copy is that
  published release.

Without them it skips. The old copy always runs as 0.1.0
(`GITBOLT_TEST_APP_VERSION`); on Linux CI also relabels the installed
package's dpkg version to 0.1.0, so apt sees an upgrade even when the
versions are equal. Afterwards the test checks that the new copy is the
offer's version and binary, that `pending-install` and `install-result`
are gone, and that the log says
`Update: ok (install, none), <from> to <version>, dmg|deb|nsis`, where
`<from>` is 0.1.0, or `unknown` after a release up to 0.9.12, which
writes no `pending-install` marker. In CI, pkexec is the real one,
allowed by a polkit rule for the runner's user.

Each platform runs two in-app updates on push and dispatch runs: the
build updating itself (the DMG and the .deb; Windows in NSIS smoke
parts 5 and 6), and the latest published release, fetched with
`gh release download --repo Inverian/GitBolt`, updating to the build
through its own Install and Restart (the DMG and the .deb, and NSIS
part 7). A release with no asset for the platform skips only the
published case, with a warning.

**This makes a contract one release back.** Each release's CI drives
the published release before it. So these must keep working in
release builds, and a change to one must still work with the
published release's copy of it, which the tests drive and the new
build reads after the update:

- the test bridge and the loopback test feed, and the variables
  `GITBOLT_TEST_BRIDGE`, `GITBOLT_INSTANCE_NAME`,
  `GITBOLT_TEST_UPDATE_FEED`, `GITBOLT_TEST_APP_VERSION` and
  `GITBOLT_TEST_UPDATE_DIR`;
- the bridge commands `trigger help.check-for-updates`,
  `click updateDialog.install`, `dump-state` and `quit`, and the
  widget names they use;
- `dump-state`'s keys `updates`, `updateDialog`, `updateDialogText`,
  `updateDir`, `pid`, `appPath` and `statusMessage`;
- the `install-result` line.

Compiling the bridge or the test feed out of release builds, say,
would break the next release's CI.

CI's manual runs take two inputs: `os` (`all`, `macos-latest`,
`ubuntu-24.04` or `windows-latest`) and `tests` (a ctest `-R` regex;
one that matches nothing fails). For example:
`gh workflow run ci.yml --ref <branch> -f os=windows-latest -f tests='^e2e_'`.
The installers of a filtered run come from a binary that ran only
those tests; a manual run never releases.

## Every action is named

`MainWindow::assignActionObjectNames()` runs after the menu and toolbar
are built and assigns a stable `objectName` (derived from the menu
path, e.g. `commands.resolve-conflicts`) to any action that doesn't
already have an explicit one, so `list-actions` reports a name for
100% of menu / toolbar / widget-toolbar actions. New features should
still set an explicit `setObjectName()` — see the Testability section
in [CONTRIBUTING.md](../CONTRIBUTING.md) — because an explicit name
survives a display-text change, whereas the derived fallback does not.

## Fallback when the bridge isn't available

Drive what the GUI strictly requires through coordinate clicks (menus,
buttons by AX name), and verify *outcomes* by inspecting the on-disk git
repository with shell `git` — the GUI is just the trigger; correctness
lives in the repository state.
