# Changelog

All notable changes to GitBolt are documented here. The format loosely
follows [Keep a Changelog](https://keepachangelog.com); versions follow
semver once 1.0 lands.

## [0.9.0] — 2026-06-12

First tracked release: the June 2026 feature batch, a 56-item
hardening pass over the whole codebase, and the test infrastructure
that now guards both.

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
