# Changelog

All notable changes to GitBolt are documented here. The format loosely
follows [Keep a Changelog](https://keepachangelog.com); versions follow
semver once 1.0 lands.

## [0.9.0] — 2026-09-24

First published release, a beta: the June 2026 feature batch, a
56-item hardening pass over the whole codebase, the test
infrastructure that guards both, and the September work that got it
building, tested and packaged on macOS, Linux and Windows.

### Added

- **Blame view**, wired into the file context menus.
- **Worktree management**: list, add (with new-branch support), lock,
  remove.
- **Merge-conflict resolution UI** with ours/theirs/manual resolution.
- **Hunk- and line-level staging** from the commit dialog's diff pane.
- **Clone cancellation** — cancel works during both fetch and checkout
  phases; partial clones are cleaned up for retry.
- **Credential prompting**: GUI askpass for git/ssh subprocesses
  (push/pull/fetch) and libgit2 credential callbacks for HTTPS clone —
  private remotes prompt instead of failing with raw stderr. SSH
  authenticates via ssh-agent.
- **Async repository open** with an immediate view switch and loading
  overlay — a cold 50k-commit repo shows commits in ~1.3s
  (docs/PERFORMANCE.md) and never freezes the window.
- **Real keyboard-shortcut editor**: live rebinding, persistence, and
  factory reset (the old page was a placebo).
- **Single-instance forwarding**: opening a repo from a second launch
  (Finder/CLI) raises the running window and switches it to that repo.
- **Test bridge** (`GITBOLT_TEST_BRIDGE=1`): an env-gated control
  channel for agents and harnesses — actions, clicks, row selection,
  multi-line typing, state dumps, screenshots (docs/AGENT_TESTING.md).
- **E2E test suite** driving the real binary through the bridge under
  CTest, plus GitService unit tests; 10 suites total.
- **Performance baseline** tooling (`tools/benchmark.py`) and recorded
  numbers (docs/PERFORMANCE.md).
- **Installers for every platform**, built and smoke-tested in CI: a
  macOS DMG (ad-hoc signed), a Windows NSIS installer (Qt, libgit2 and
  the MSVC runtime bundled), a Linux DEB (private Qt under
  /usr/lib/gitbolt) and an AppImage. A release is published
  automatically when the version changes on main.
- **Publishing new branches**: Push on a branch that has never been
  pushed publishes it and sets its upstream, instead of failing.
- **Deleting remote branches**: Commands → Delete remote branch, and on
  remote branches in the sidebar.
- Test bridge `select-item` for combo boxes, so agents can switch
  branches through the UI; `list-widgets` reports combo boxes.

### Fixed

- Commit & Push now actually pushes; tag "push after create", stash
  "keep index", worktree "create branch", GitFlow wizard branch names,
  and the GitFlow finish buttons were all silently dead or unwired.
- A use-after-free when closing a repository while a cherry-pick was
  queued, and dangling repository pointers captured by the reflog,
  rebase, and statistics dialogs.
- Tag/SHA checkout corrupted HEAD (now detaches properly); amend on an
  unborn HEAD no longer crashes.
- The crash handler and the terminal's forkpty child are now
  async-signal-safe.
- git subprocesses can no longer hang on hidden terminal prompts
  (GIT_TERMINAL_PROMPT=0 + askpass).
- Duplicate log pages garbling the commit graph; external ref updates
  (fetch/commit from a terminal) now auto-refresh via .git-internals
  watches.
- A persisted Light/Dark theme was clobbered at startup by a
  color-scheme reset that ran in the wrong order.
- Interactive-rebase drag-and-drop off-by-one; stacked rebase-complete
  handlers firing N times; terminal escape sequences split across
  reads; dropped paste chunks on a full pty.
- O(N²) lane recomputation on every log page; per-click UI stalls on
  branch-contains queries; a 9-second settings-dialog hang.
- A failed fetch, pull or push now stays visible until the next one
  (it vanished after 5–8 s), and a long error no longer pushes the
  toolbar's Filter box into the overflow menu.
- The sidebar's "Push" no longer freezes the window for the whole
  network round trip.
- Linux and Windows build for the first time: libgit2 1.7 support
  (Ubuntu 24.04), Linux's forkpty header, and the built-in terminal
  compiles on Windows (showing a "not available yet" notice; a ConPTY
  backend is still to come).
- Linux: the binary is now `gitbolt`, matching the desktop entry's
  Exec=; the AppStream metadata named the wrong license (MIT, now
  GPL-3.0-only).
- Tests that only failed in Release builds (a `Q_ASSERT` with a side
  effect in the test helper).

### Changed

- MainWindow decomposed (menu builders, a repo-command controller, a
  shared inline-op indicator, shared record-table panels); dead plugin
  host and page-eviction machinery deleted.
- Build system: explicit source lists (no GLOB), Debug default,
  compile-commands export, working sanitizer toggle, hermetic tests
  isolated from user git config, CI that builds, tests headless, and
  deploys Qt at install time so packages actually contain it.
- Version now flows from one place (CMake `project()`) into the app,
  the About dialog, and package names.
- Zero compiler warnings on clang, GCC and MSVC.
- CI: Qt 6.10 and Node 24 actions on all three platforms; pull requests
  build and test, pushes to main also package, and a manual dry run
  packages any branch without publishing.
- Docs: macOS needs Qt 6.9.2 or later; the Windows build steps now use
  Qt 6.10 and MSVC 2022; the README architecture graph is a Mermaid
  diagram checked against the real module dependencies.
