# Changelog

All notable changes to GitBolt are documented here. The format loosely
follows [Keep a Changelog](https://keepachangelog.com); versions follow
semver once 1.0 lands.

## [0.9.1] — 2026-10-09

Fixes from the first weeks after 0.9.0: rebasing, tags, cloning,
starting and quitting, a thread race, the Windows terminal, and
installers that run on the macOS versions they claim.

### Added

- **Windows terminal**: the built-in terminal (Tools → Git bash, the
  Console tab) now runs cmd.exe under ConPTY instead of showing "not
  available yet". Multi-line paste runs each line, and a flood of
  output stays responsive.
- **Open a clone whose checkout failed**: when git fetches everything
  but can't check the files out, the repository is kept and the Clone
  dialog offers to open it.

### Fixed

- **Rebase** (Commands → Rebase) never rebased: git rejected every
  plan, and GitBolt said "Rebase complete." It now runs the plan in
  the order the dialog shows, keeps git's error on the status bar
  when a step fails, and offers the conflict resolver when a commit
  conflicts. Continue, Skip and Abort report failures too, and
  Continue no longer waits on a text editor GitBolt can't show.
- **Quitting during a fetch, pull or push** stops git and exits.
  GitBolt used to stay running without a window until the operation
  ended (up to two minutes on a stalled network) and could then
  crash.
- **Delete tag** failed for every tag, and the picker and sidebar
  showed `refs/tags/…` names.
- **Create tag** failed for its default (branch) target, and "push
  after create" failed when a branch had the same name as the tag.
- **Clone** now runs the git CLI, like push, pull and fetch: SSH
  clones work on Windows, credential helpers (Keychain, Git Credential
  Manager) and your SSH config apply, and errors show git's own
  message. Cancel stops git and everything it started.
- **Clone into a folder holding only hidden files** (e.g. `.env`) no
  longer deletes the folder when the clone fails.
- **Launching GitBolt** no longer fails with "another instance is
  running" after a crash or many `--version` runs. A second launch
  while GitBolt is busy now hands over quietly instead of claiming it
  is not responding.
- **Background refreshes** after fetch, pull, push, rebase and
  cherry-pick ran on the wrong thread, a data race that could crash.
- **macOS**: the app claimed macOS 12 while its binaries were built
  for macOS 26. It now targets macOS 13, the minimum of the Qt it
  ships, and the binaries and the declared minimum always agree.
- **Help → Changelog** showed an old copy; it now shows this file.

### Changed

- libgit2 1.9.7 is built into the macOS and Windows apps (static), so
  the installers carry no libgit2, pcre or zlib libraries. GitBolt
  uses libgit2 only for local repository access; every network
  operation goes through the git CLI.
- GitBolt never opens a text editor for git: a git command that
  would ask for one keeps the message git prepared.
- CI: the build inputs are pinned (Qt, the Qt installer action, the
  AppImage tools and runtime, NSIS, the test image), Windows test
  output is no longer lost, and a flaky Windows test is fixed.

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
