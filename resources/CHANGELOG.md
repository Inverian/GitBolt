# Changelog

GitBolt follows [semantic versioning](https://semver.org/). Entries are
kept most-recent-first.

## Unreleased

### Added
- Non-blocking repository open: File → Open / Recent / dashboard /
  clone / CLI all switch to the repository view immediately and
  stream data in from worker threads. A new animated loading
  overlay (`LoadingOverlayWidget`: translucent veil + rotating arc
  + message) covers the repo view until the first commit-log page
  arrives. Previously `GitService::openRepository` ran libgit2 open
  AND a recursive working-tree walk (for the file watcher) on the
  UI thread, freezing the app on whatever screen was active — up
  to ~a minute on large repositories. The walk now runs on a
  worker (`FileWatcher::enumerateWatchDirs`, cancellable), watch
  registrations apply in 512-path chunks across event-loop ticks,
  and `.git` internals + the repo root are watched immediately so
  external commits are noticed even while the walk is running.
  A generation counter discards superseded opens (rapid double-
  open is safe), and a failed open reverts to the previous view
  with the old repository's data repopulated and the libgit2
  error message shown in the warning dialog.
- Refresh workers (status / log / branches / stashes / submodules /
  tags / worktrees) now pin the repository via `shared_ptr` and
  re-check it under the repo mutex before emitting, so results
  from a superseded repository are dropped instead of briefly
  flashing the wrong repo's data after a switch (also closes a
  use-after-free window when a swap landed mid-refresh).
- `refreshLog` always emits the initial page even when the rev-walk
  fails or the repository has an unborn HEAD, so "loading" states
  driven by the first `logReady` can never get stuck.
- Settings → UI Design page: new "Default Pane Sizes" group with a
  bottom-inspector-pane percent slider (10–90%) that drives the
  default split between the revision graph and the inspector tabs;
  live preview label shows the implied "Revision graph: N% ·
  Inspector: M%" pair so the user can see what the number means
  before clicking Apply.
- Settings → UI Design: "Default Window Size" group — width × height
  spinboxes plus a "Restore previous window size on launch" toggle.
  When the toggle is on, the window remembers wherever it was last
  left; when off, it always opens at the configured size and Apply
  resizes the running window immediately. Min size 1024 × 700 with
  CorrectToNearestValue snapping any sub-min typed value up to the
  floor on focus-out.
- Settings → UI Design: "Default Dialog Size" group — one global
  width × height applied to every popup dialog (Commit, Clone, Tag,
  Stash, Rebase, Reflog, Cherry-Pick, Worktree, Remotes, TextEditor,
  Stash Manager, Settings) on next open. Same "Restore previous"
  toggle as the window-size group, with a current-dialog-size live
  readout under the spinboxes for picking values empirically (drag
  the dialog, click Apply, read off the size). Min 400 × 300.
- Per-dialog size persistence via
  `SettingsService::applyConfiguredSize(QDialog*, key)` helper.
  Each popup gets its own `layout/dialog/<key>/geom` storage slot
  and remembers its last drag-resized shape independently. The
  helper wires up save-on-close via `QDialog::finished` so any
  dialog that calls it once in its constructor gets persistence
  for free. Eleven popup dialogs adopted; CommitDialog gates its
  existing `showEvent` on the new `restoreLastDialogSize()` toggle.
- "Contained in branches" footer in the Commit inspector tab now
  resolves to a real branch list via `git branch --all --contains
  <sha> --format=%(refname:short)`. Skips `<remote>/HEAD` symbolic
  refs (alias noise), caps display at the first 10 branches with
  "+N more" for commits reachable from many heads, and falls back
  gracefully to `(none)` / `(unable to query)` / `(no repo open)`
  when the lookup is empty or fails.
- Pull is now in the Commands menu next to Fetch and Push. Was
  reachable only from the toolbar; the QAction was created and
  wired up but never `addAction`'d to the menu.
- Commit dialog section headers ("Unstaged Changes" / "Staged
  Changes") gained a small ⓘ info icon on the right with a rich
  tooltip explaining the file-status letters: M Modified, A Added,
  D Deleted, R Renamed, U Unmerged, ? Untracked. Same tooltip
  reused on both icons since the codes are universal.
- Inline activity indicator now also drives the Refresh button and the
  branch quick-switch combo. Refresh shows "Refreshing…" for ~1.5s
  (no completion signal exists since refresh fires five async ops in
  parallel). Branch switch shows italic "Switching to <branch>…",
  then green "✓ Switched to <branch>" for 2.5s. Both reuse the
  remoteOpClearTimer that fetch/pull/push already set up — clicking
  any of these mid-flight cancels the prior timer so the latest
  operation always wins the indicator.
- `GitService::process()` thread-safe accessor that locks the repo
  mutex briefly while libgit2 reports the workdir, then returns a
  GitProcess by value. UI callers that previously did
  `gitService_->repository()->process().run(...)` now use
  `gitService_->process().run(...)` — the libgit2 call is locked but
  the QProcess that runs the actual git command isn't (it's
  independent of repo_ state). Replaces 13 unsafe direct callsites
  in MainWindow that competed with the GitService background workers.
- `GitService::repoMutex()` accessor — exposes the same mutex used
  internally so future UI callers can lock other libgit2-touching
  reads (`head()`, `branches()`, `submodules()`, etc.) consistently.
- Toolbar Stash button now opens the Manage Stashes dialog (apply /
  pop / drop / new) instead of being a permanently-disabled
  placeholder. Implemented by promoting the Commands menu's
  "Manage stashes…" QAction to a MainWindow member and sharing it
  with the toolbar via `addAction(QAction*)` — same lockstep
  enable-state pattern fetch/pull/push use. Action title is
  "&Stash..." so the toolbar reads as just "Stash" while the menu
  shows "Stash..." with its mnemonic.
- Inline activity indicator on the toolbar between push and Commit.
  Empty by default; while a Fetch / Pull / Push is running it reads
  "Fetching from origin…" in italic; on success switches to a green
  "✓ Fetch complete." for four seconds; on failure switches to a red
  "✗ fetch failed: <git stderr>" for eight seconds. The acting button
  also disables for the duration of the op so the user sees a state
  change on the thing they clicked. Status-bar text at the bottom of
  the window was the only previous feedback and was easy to miss.
  Multi-line git stderr is collapsed to a single row (newlines → " | ")
  with the full text parked in a tooltip.

- Commit dialog redesigned to match Git Extensions' commit window:
  horizontal main split with two file-list panes on the left (Unstaged
  on top, Staged on bottom; each with its own substring filter and
  per-pane action buttons) and a vertical right pane with the diff
  viewer on top and a two-column commit-controls panel on the bottom
  (Commit / Commit & Push / Amend stacked vertically beside the
  message editor). Selecting a single file in either list now drives
  the diff pane via the existing `DiffViewerWidget`. A footer status
  strip shows committer (read from `user.name` / `user.email`),
  current branch, and "Staged X / total". Window title is
  `<repoName> - Commit to <branch>` and refreshes whenever
  `branchesReady` fires (so it stays right after a checkout).
  Persisted geometry / splitter keys bumped to `/v2` so the new
  three-splitter layout starts at sensible defaults instead of
  trying to restore the old single-vertical-splitter sizes.
- Dashboard redesign with recent-repository status probe, action cards,
  and a Contribute link row.
- Animated GitBolt bolt logo in the dashboard corner (constant colors
  with randomly-flipping binary digits and a subtle shimmer).
- Tools → Settings: new Recent Repositories page with max-count, sort,
  and shortening-strategy preferences.
- File menu: Home (return to dashboard without closing the current repo).
- Commands menu wiring: Manage stashes, Create / Delete tag, Cherry pick,
  Create / Delete / Checkout branch, Merge branches, Reset changes
  (soft / mixed / hard), Clean working directory, Undo last commit,
  Archive revision, Checkout revision.
- Repository menu wiring: File Explorer (OS-native reveal), Manage
  submodules, Manage worktrees, Git maintenance, Update all submodules,
  Synchronize all submodules.
- Tools menu wiring: Git bash (terminal window), Git command log,
  GitK launcher.
- Help menu wiring: User manual, Report an issue, Changelog.
- Plugins menu: GitFlow, Delete obsolete branches (multi-select picker
  over `git branch --merged`), Find large files (top 50 blobs by size,
  human-readable units, sourced via `git rev-list --objects --all` +
  `git cat-file --batch-check`), Statistics (commit count, distinct
  authors, top contributors, branch / tag / remote counts), Plugin
  Manager (opens Settings), Periodic background fetch (toggleable
  with editable interval, persists across launches).
- Repository menu: Edit .gitignore / .git/info/exclude / .gitattributes
  / .mailmap (shared TextEditorDialog with monospace font, no-wrap,
  undo/redo); Sparse Working Copy submenu (Initialize cone-mode,
  Edit patterns, Disable); Remote repositories CRUD (RemotesDialog
  with name/URL table, Add / Edit URL / Remove buttons; push URL
  shown inline when it differs from fetch URL).
- Commands menu: Bisect submenu (Start, Mark good, Mark bad, Skip,
  Reset); Rebase two-stage flow (RebaseDialog wired with branch
  picker, target ref → commit walk via RevWalk hide/pushHead,
  rebase plan handed to GitService::interactiveRebase, status-bar
  reflection via rebaseComplete); Format patch / Apply patch
  (range and output dir prompts; choice between `git am` and
  `git apply`).
- Manage Stashes dialog (StashManageDialog) replaces the save-only
  StashDialog at the menu entry: lists existing stashes with
  Apply / Pop / Drop on the selected entry plus a "New stash..."
  button that round-trips to StashDialog. Drop confirm includes
  the stash message, not just its index.
- Tools → Git command log (F12): subscribes to a new
  `git::GitProcessLog` singleton that GitProcess::run posts to on
  every external git invocation. Pre-populates with a 200-entry
  ring buffer so the dialog opens with the recent history visible.
- Navigate menu: Go to current revision (Cmd+Shift+C, resolves
  HEAD via libgit2), Go to commit (Cmd+Shift+G, accepts SHAs /
  short SHAs / branch names / tags / HEAD~3 via the new
  `Repository::resolveRef`), Go to first / last parent commit, Go
  to child commit (Cmd+N) / parent commit (Cmd+P), backward /
  forward history (Cmd+[ / Cmd+]) with browser-style stack
  invalidation on new navigations.
- Help menu: Check for updates (opens GitHub Releases).
- View menu: real column-visibility toggles for Graph / Author /
  Date / SHA columns; section toggles for Stashes / Remote
  branches / Tags in the branch tree; both groups persist to
  QSettings and reapply after each repo opens.
- View menu: Show filtered branches (Cmd+Shift+T) opens a
  BranchPickerDialog with checkable rows, search filter, and
  Select All / Clear All operating on the visible subset. Apply
  switches GitService to a new `LogScope::SelectedBranches` that
  pushes only the picked branch tips onto the rev walk; both the
  scope and the branch list persist across launches. Cancel or an
  empty selection reverts the radio to the previous scope so the
  log is never left blank by the dialog.
- Recent Repositories: right-click → Forget this repository on
  any row; auto-prune of stale paths (non-existent or non-git
  directories) at startup.
- Bolt logo timer pauses while the widget is hidden (no CPU spend
  while on the repo view).
- Toolbar: Branch quick-switch dropdown next to the refresh /
  fetch / pull / push group. Lists every local branch, current
  branch pre-selected, choosing a different entry calls
  GitService::checkoutBranch (which also refreshes status / log /
  branches). Disabled and hidden on the home screen, populated
  from each branchesReady refresh.
- Toolbar: Commit button now shows the live changed-file count in
  parentheses ("Commit (4)..." when four files differ from HEAD).
  Wired to GitService::statusReady; counts every entry whose
  status is not Current/Ignored, matching the staging widget's
  definition. Drops back to plain "Commit..." on close. The
  Commit toolbar action is the one button rendered with text
  beside its icon — the rest of the toolbar stays icon-only,
  matching the Git Extensions reference layout.
- Revision graph: modifier-click (Cmd on macOS, Ctrl on other
  platforms) on the currently-selected commit row clears the
  selection. The inspector tabs (Commit / Diff / File Tree)
  blank back to the "Select a commit to view details" state.
  QAbstractItemView's SingleSelection mode otherwise keeps
  exactly one row selected forever; this lets the user reach a
  zero-selection state without having to switch repos.

### Changed
- Settings dialog input widths constrained across all pages so they
  no longer sprawl across a dragged-wide dialog. Every form layout
  uses `FieldsStayAtSizeHint`; QLineEdits cap at 360 px (still wide
  enough for a long GitHub no-reply email), QSpinBoxes for size
  values cap at 140 px, the Code Font picker at 260 px, the Theme
  combo at 260 px, and the bottom-pane slider at 480 px. The empty
  space to the right of inputs stays empty, matching the
  GitExtensions reference.
- Diff viewer line backgrounds rebuilt on a new `DiffTextEdit`
  subclass (`QPlainTextEdit` with a custom `paintEvent`) that
  pre-paints full-line-height colored stripes for `+` / `-` / `@@`
  lines from each block's top to the next block's top, so
  consecutive added (or deleted) lines form one contiguous green
  (or red) band with zero white gap — the GitHub / GitExtensions
  look. Root cause of the stubborn gaps turned out to be data, not
  paint: libgit2 returns `line.content` with the source newline
  still attached, and joining those with `'\n'` created an *empty
  block* between every real diff line. Both unified and
  side-by-side renderers now strip trailing newlines at ingest.
  Earlier attempts via `QTextCharFormat` backgrounds,
  `QTextBlockFormat::setBackground`, and `ExtraSelection` +
  `FullWidthSelection` are documented in the widget for posterity.
- DiffSyntaxHighlighter is now foreground-only (green/red/blue
  text, bold file headers); all background fill moved to
  DiffTextEdit's painter.
- File Tree inspector preview uses the same explicit Menlo 11 pt
  font as the diff viewer so the two inspector tabs render text at
  identical density, and the file tree lists directories before
  files at every level, alphabetical within each group (GitHub
  style) — the previous comparator mixed two orderings and was
  non-transitive (undefined behavior under `std::sort`).
- Bolt logo's digit ink and body fill sampled from
  `resources/icons/gitbolt-256.png` so colors stay in lock-step with
  the artwork (bright amber outline, transparent interior, bright
  amber digits).
- Dashboard Contribute row is a single compact line with a bolded
  "Contribute:" label inline with Develop / Donate / Issues links.
- Toolbar "Filter: Search commits…" is hidden on the home screen and
  appears when a repository is open.
- Revision-graph column proportions: the Hash column is no longer
  stretched to fill remaining horizontal space (it sized to ~700
  pixels of whitespace around a 7-char short SHA). The Message
  column now stretches as configured, Hash sits snug at content +
  padding, and Author / Date keep their interactive widths.
- Revision-graph Author column: extra horizontal padding so the
  author name reads with breathing room between it and the Date
  column. The Message column (Stretch) absorbs the difference.
- Revision-graph header sections no longer go bold when a row is
  selected in their column. QHeaderView::highlightSections is
  the Qt default and was making the column titles jump in weight
  on every selection change.
- Light-theme scrollbar track is now a noticeably darker gray
  (#d4d4d4) so it stands out from the surrounding white panels.
  Previously the track inherited the system default which on
  macOS rendered as essentially the same color as the window —
  the handle had nothing to slide against visually.
- Toolbar Commit and Stash buttons render their text at the
  application default font size, matching the "Branch:" and
  "Filter:" QLabels in the same toolbar. Qt6 on macOS picks a
  smaller font for QToolButton than for QLabel by default, which
  made the only two text-bearing buttons read as visually
  demoted next to the inline labels. Setting them to
  QApplication::font() puts every text element on the toolbar
  at one consistent size.

### Fixed
- **Concurrent libgit2 crash** (heap corruption / `malloc_zone_error`
  inside `git_pool_clear` under `git_status_list_new`). GitService's
  `runner_.run()` uses `QtConcurrent::run`, which submits to the
  global QThreadPool — so two `refreshStatus` / `refreshLog` /
  `refreshBranches` calls can land on different worker threads at the
  same time. libgit2 isn't safe for concurrent access on a single
  `git_repository*`, so the workers stomped on each other's pool
  state. Fixed by serializing every Repository call through a single
  `std::mutex repoMutex_` on GitService — the worker lambdas take
  the lock at the top, and synchronous main-thread mutators
  (`stageFile`, `commitChanges`, `checkoutBranch`, `push`, `pull`,
  `fetch`, stash ops, tag ops, worktree ops, …) take it around their
  repo_ accesses too. The mutex is released before refreshes are
  scheduled so the queued worker doesn't deadlock. Stress-tested
  with rapid concurrent refresh + fetch + commit dialog open/close
  cycles that previously crashed within seconds.
- `git push origin ""` / `git pull origin ""` produced
  `fatal: invalid refspec ''` because the toolbar Push/Pull buttons
  pass an empty branch argument when no branch override is set.
  `GitProcess::push` and `GitProcess::pull` now drop empty arguments
  before invoking git, so the bare `git push origin` /
  `git pull origin` form runs and honors `push.default` / the
  upstream tracking branch.
- Fetch / Pull / Push silently treated non-zero git exit codes as
  success. `GitProcess::run` returns Result::ok() whenever the
  subprocess started AND finished — the actual git exit code lives
  inside `ProcessOutput::exitCode`, which
  `GitService::{fetch,pull,push}` weren't checking. So
  "Repository not found" or "rejected (non-fast-forward)" silently
  looked like a successful fetch. They now check exitCode and emit
  `operationFailed("fetch", stderr)` when git reports failure, which
  lets the new toolbar indicator render the red ✗ state correctly.

### Removed
- Three misleading "not yet implemented" placeholder menu items that
  flashed a status-bar message on click but did nothing real:
  View → Show git notes (model has no Notes category), View → Show
  author avatar column (CommitLogModel has no avatar column), and
  Plugins → Impact Graph (no impact-analysis plugin yet). Dropped
  the now-unused `addPlaceholder()` helper.
- Unused `diffHeaderLabel_` from RepositoryView's Diff inspector
  tab. The "(N) Diff with <sha>" title row was redundant with the
  selected commit row in the log table.
- Repository → Repository settings… menu item. Git Extensions' model
  of one unified Tools → Settings (scope determined by storage
  location) applies here too.
- Plugins → Plugins settings… menu item. Per-plugin configuration
  belongs under Tools → Settings, following the same pattern.
- Placeholder "Translate" entry from the Contribute row. No
  localization workflow is in place yet.
