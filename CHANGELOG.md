# Changelog

All notable changes to GitBolt are documented here. The format loosely
follows [Keep a Changelog](https://keepachangelog.com); versions follow
semver once 1.0 lands.

## [0.9.3] — 2026-10-10

A fix for Settings crashing GitBolt, and less padding around the
toolbar's Commit and Stash buttons in light mode.

### Fixed

- **Settings could crash GitBolt** once a repository had been opened,
  in every release since 0.9.0; on macOS it crashed every time. Tools →
  Settings, Plugins → Plugin Manager and the Preferences shortcut
  (Cmd+, on macOS) all open that dialog. Settings lists the menu
  commands for its Shortcuts page, and that list, taken at startup,
  included the entries of File → Recent Repositories. Opening a
  repository rebuilds that menu, deleting those entries, and Settings
  then read the deleted entries. Recent Repositories is now left out of
  the list (its entries are folders, not commands), and a menu entry
  deleted later is skipped.

### Changed

- **Toolbar**: in light mode (the Light theme, or System with a light
  appearance), Commit and Stash, the two toolbar buttons that show
  their text, had more padding than the icon buttons. On macOS they
  sat 34pt apart, against 25 to 27pt between the icon buttons; they are
  now 25pt apart. The dark theme is unchanged.
- **Contributing**: contributors now agree once to a contributor
  license agreement (`CLA.md`) by ticking its box in the pull
  request template. They keep their copyright, and the project can
  publish their contributions under the GPL-3.0 or a license it
  chooses later.
- Development: a new end-to-end test opens Settings after a
  repository opens. It runs GitBolt with a scratch home folder, so a
  crash leaves no crash report behind for the next real launch.

## [0.9.2] — 2026-10-09

Rebases you can plan commit by commit and carry on however they were
started, a background fetch that no longer freezes the window, a
faster terminal, and fixes to committing mid-merge or mid-rebase,
tags, checkout, cloning and launching.

### Added

- **In-progress bar**: while git is in the middle of a rebase, merge,
  cherry-pick or revert, started from GitBolt or from a terminal, a
  bar across the repository view says so. It offers Resolve
  Conflicts… while files are conflicted, Continue once none are
  (Commit… for a merge, or for a cherry-pick or revert of a single
  commit), Skip for a rebase, and Abort; Skip and Abort ask first. A
  stopped rebase could be carried on only from the Interactive Rebase
  dialog, and only if it had been started with the dialog's Start
  Rebase button and the dialog was still open: its Rebase button
  closed it and left nothing to carry on with. The dialog is now just
  the plan.
- **Rebase plans** (Commands → Rebase): each commit can be picked,
  reworded, edited, squashed, fixed up or dropped (buttons, or the
  keys P, R, E, S, F and D), and commits move with Move Up and Move
  Down (Ctrl+Up and Ctrl+Down, Cmd on macOS) as well as by dragging.
  Reword, or a double-click, asks for the new message, which the
  commit gets even if the rebase stops on a conflict first and is
  carried on from GitBolt. Edit stops the rebase after that commit and
  says so, and Continue on the bar carries on. A squash or fixup with
  no kept commit below it is caught before git starts. Merge commits
  in the range are left out, as git leaves them out, and a plan made
  before HEAD moved, or before another branch was checked out, is
  refused while the dialog is still open.

### Fixed

- **Committing while a rebase was stopped**, as the conflict resolver
  advised, threw away the rest of the rebase and left HEAD detached
  partway through it. A commit now clears only a merge's,
  cherry-pick's or revert's state, as `git commit` does, and during a
  rebase the resolver points to Continue. When a merge, rebase or
  cherry-pick stops, GitBolt now offers the resolver only if files
  are actually conflicted.
- **Staging, unstaging and committing right after git changed the
  index** (a merge or rebase, or a cherry-pick or `git add` run in a
  terminal) could work from an old copy of it and undo what git had
  staged, for example leaving a merge's cleanly merged files out of
  the merge commit.
- **Periodic background fetch** (Plugins → Periodic background fetch)
  froze the window for the whole fetch, every few minutes, went on
  fetching a repository after it was closed, and made quitting wait
  for it. It now runs in the background and quietly: a failure is a
  passing note and a flag on the auto-fetch label rather than a
  sticky error, clicking Fetch during an auto-fetch shows that fetch
  as running ("Fetching from origin…") and then its result, as for a
  toolbar Fetch, and closing the repository or opening another stops
  it.
- **Annotated tags**: the sidebar tooltip showed the tag object's
  hash, which is in no log, and never the tagger or the message. It
  now shows the tagged commit, the tagger and the message.
- **Checking out a name that is both a branch and a tag** took the
  tag's files but put HEAD on the branch, which then showed phantom
  changes. As in git, the branch now wins. This affected the sidebar,
  the toolbar branch switcher, Commands → Checkout branch and
  Commands → Checkout revision.
- **Checking out a branch that another worktree has checked out**
  wrote that branch's files and index before the switch was refused,
  which left them staged on the current branch; from a detached HEAD
  it wasn't refused at all, so two worktrees ended up on the same
  branch. It is now refused before anything changes, and the error
  says where the branch is checked out.
- **Cherry-picked commits** were credited to you instead of their
  author: GitBolt applies a pick and leaves the commit to the Commit
  dialog, which used your name and date. The picked commit's author
  and author date are now kept, with you as the committer.
- **A checkout, cherry-pick or stash apply that failed partway** (a
  file that couldn't be written, or another git holding the index
  lock) could leave files listed as staged that weren't, and the next
  commit included them. GitBolt now rereads the index after such a
  failure and before staging or committing.
- **Checkout as local branch** (sidebar, on a remote branch) failed
  whenever no local branch of that name existed yet. It now creates
  one that tracks the remote branch.
- **A clone whose post-checkout hook fails** (from `core.hooksPath` or
  `init.templateDir`) was deleted, although git keeps it. It is now
  kept and offered for opening, like a clone whose checkout failed.
- **Terminal**: large outputs draw several times faster (in one
  measurement, `seq 1 100000` in zsh took 2.7 s instead of 22.5 s).
  On macOS and Linux, a command that prints without stopping (`yes`,
  a huge `cat`) no longer freezes GitBolt, so Ctrl+C can stop it, and
  a character split between two reads no longer comes out as
  replacement characters (`�`, one per byte). On Linux, Ctrl+C copied
  instead of interrupting when nothing was selected.
- **Launching GitBolt while it is busy** (Windows): a second launch
  now waits for the busy window instead of saying it is not
  responding after 30 seconds, and takes over if that window quits or
  is ended. A launch waiting on a window that has truly hung shows
  nothing and waits until that window is ended (End task, say), then
  opens in its place.

### Changed

- Development: `tools/check-ci-pins.py` reports newer releases of
  linuxdeploy, its Qt plugin, the AppImage runtime and
  install-qt-action, which CI pins by hash, and can bump them (NSIS
  and the test image are still checked by hand); a flaky
  single-instance test is fixed; the test bridge gains `fire-timer`
  and reports the status-bar message in `dump-state`; unused libgit2
  clone code is gone.

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
