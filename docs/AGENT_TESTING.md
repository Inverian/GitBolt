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
tools/bridge.py list-widgets          # windows, buttons, views, editors (with classes)
tools/bridge.py select-row QTableView:0 0     # the AX-impossible operation
tools/bridge.py select-row commit.unstagedList 0,2,5   # multi-row (ExtendedSelection lists)
tools/bridge.py click "Lock/Unlock"
tools/bridge.py type CommitMessageEdit:0 "subject\n\nbody line"   # \n \t \\ decoded
tools/bridge.py screenshot /tmp/state.png     # in-process grab(), no overlay issues
tools/bridge.py quit                  # MainWindow::close() — clean exit for relaunch tests
```

`dump-state` also reports `headOid`, `theme`, and `logRows` so a
harness can assert "HEAD moved", "Dark persisted", and "the log
loaded" without shelling out or screenshotting.

Addressing:
- **Actions** by `objectName` (every action has one — see below), by
  menu-path slug (`commands/merge-branches`), or trailing suffix
  (`merge-branches`); `trigger`/`click` are queued so an action that
  opens a modal returns immediately — assert with `dump-state`.
- **Buttons** by visible text (case-insensitive, mnemonics/`...` stripped)
  with a prefix fallback so `Commit` matches the live-count `Commit (1)`.
- **Views / editors** by `objectName` or `ClassName[:index]`, matched
  against the whole superclass chain — `QPlainTextEdit:0` resolves a
  `CommitMessageEdit`. Active window's widgets are ordered first.

Two real bugs were caught the first time the bridge drove a full
merge→resolve→commit cycle: `Repository::conflictEntries()` read a stale
cached libgit2 index (CLI-side merges were invisible — fixed with
`git_index_read(force)`), and the brittle exact-text button match
(fixed with the prefix fallback). Worktree lock/remove — which no
synthetic input could ever reach because they need a selected table
row — were verified end to end via `select-row`.

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
