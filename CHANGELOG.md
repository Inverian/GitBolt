# Changelog

All notable changes to GitBolt are documented here. The format loosely
follows [Keep a Changelog](https://keepachangelog.com); versions follow
semver once 1.0 lands.

## [Unreleased]

### Changed

- **`DO_NOT_TRACK` no longer turns usage statistics off.** Turn them
  off in **Tools › Settings › General**, or with the environment
  variable `GITBOLT_TELEMETRY=0`. If you turned them off with
  `DO_NOT_TRACK` alone, GitBolt sends them after this update until you
  do one of those.

### Fixed

- **Opening a submodule or a worktree**: Repository › Manage
  Submodules' "Open in New Window" did nothing, and Manage Worktrees'
  showed the worktree in Finder or Explorer. Both menus now have Open
  in GitBolt, which opens the submodule or worktree in the window (a
  submodule that isn't checked out says so), and Open in File
  Explorer, which shows its folder.
- **The Console tab in a bare repository** now sets GIT_DIR, as Git
  bash has since 0.9.7, so git typed there works where git's
  `safe.bareRepository` setting is `explicit`. Opening a bare
  repository, or leaving one, sets or unsets GIT_DIR in the Console's
  shell at its prompt (sh, bash, zsh, ksh, csh, tcsh or fish), so the
  shell keeps its jobs and history; a command still running in it is
  left to finish first, and the line at the prompt is cleared with a
  Ctrl+C first. On Windows, and with a shell of another kind, the
  shell is started again once no command runs in it.
- **Recent repositories listed a repository twice** when it was opened
  by two spellings of its path: on macOS, `/tmp/…` from Finder and
  `/private/tmp/…` from Terminal, a path through a symlinked folder,
  and on Windows a short (8.3) name, a `subst` or mapped drive, or a
  junction. It is now listed once, under the spelling it was last
  opened by.
- **More crashes on config lines with no value** (a name and no
  `= value`; see 0.9.7). A line with none in a branch's section of
  `.git/config` (`rebase`, say, which git reads as `true`) crashed
  Delete Branch. One in a remote's section (`prune`, `mirror`) crashed
  removing the remote, and a `fetch` with none crashed adding one.
  Each also left `.git/config.lock` behind, which then stopped every
  change to the config, git's own included; these now just work. A
  `merge.conflictstyle` with no value, in any git config, crashed
  switching branches, discarding changes, stashing and cherry-picking,
  and a `merge.default` with none crashed cherry-picks; they now stop
  with git's own message ("missing value for …"). As in 0.9.7, the
  fixes are in the libgit2 built into every GitBolt download.
- **Selecting a commit no longer waits for its diff.** The Stats row,
  the Diff tab and the File Tree now fill in from a background thread,
  so a large commit no longer freezes the window; when that takes more
  than a moment, the Stats row says "(counting…)" meanwhile. Selecting
  another commit, switching repositories or quitting stops a read
  still going for the commit before, where they used to wait for it.
- **A commit or a working tree that couldn't be read looked
  unchanged.** When git can't read a commit's files (a tree nested
  past the 2,048-level limit, say), the Stats row, the Diff tab and the
  File Tree now say why, where the commit looked as if it changed
  nothing. When the working tree's status can't be read, the toolbar
  shows "Commit (?)" with the reason in its tooltip, the Commit dialog
  says why, and Recent repositories shows "—" instead of what read as
  a clean status; a submodule list that can't be read says so too.
- **Open Externally on drives that keep no file modes** (FAT, exFAT
  and NTFS drives, and WSL's `/mnt/c`) asked about every file, since
  every file there looks executable. It now goes by the mode git has
  for the file, and by what the file is: a program, or a script your
  system would run (on macOS one with no extension, or a shell or Perl
  script, which iTerm2 runs).
- **Discard and Unstage of a file whose name looks like a pattern**
  (`*`, `[ab]`, `!x`, or one ending in a space) acted on every file the
  pattern matched: discarding a file named `*` threw away the changes
  to all of them. They now act on that one file.
- **A commit message or name of one line of megabytes** froze the
  window while GitBolt laid it out. Such a line is now shown cut, with
  "…" and a note, in the Commit tab, the log and the other views that
  show commits; Copy in the log still copies all of it.

### Security

These fixes close ways in which a repository you clone or open, or a
server you fetch from, could use GitBolt against you. Most needed you
to do something in GitBolt with the content, such as push, resolve a
conflict or open a file.

- **Push could run a command named by a branch.** A branch name that
  begins with `-` reached git as an option instead of a name: from the
  sidebar's Push, Merge, Rename, Set Upstream, Checkout as local branch
  and Delete from remote, and from the toolbar's Push. In a repository
  whose `origin` is a folder on your disk, a branch could be named so
  that pushing it ran a program. GitBolt now passes names after git's
  `--` and pushes branches by their full name, and where that isn't
  possible it refuses a name that begins with `-` and says why. Rename
  and Delete from remote still work on such a branch, to get rid of it.
- **Resolve Conflicts wrote through symbolic links.** When the file in
  conflict was a symbolic link, the resolution was written to wherever
  the link pointed, which could be any file of yours outside the
  repository. Resolutions now go into the repository's index and are
  written to the working tree from there, never through a link. A
  link in conflict is resolved by taking Ours or Theirs as it is; the
  dialog says which sides are links. Such a file's resolution used to
  be lost from the index, too. A file whose executable bit only the
  other side changed keeps it, as git does. Repository › Edit
  .gitignore, .gitattributes and .mailmap don't open or save one of
  those files when it is a link.
- **Open Externally ran programs.** On macOS, a file marked executable
  opens in Terminal and runs, whatever its name (`CHANGELOG`, say), and
  so does a `.command` file; a cloned repository can contain either.
  Open Externally now asks before opening a file marked executable or
  of a type that runs, or that opens what it points to (on Windows
  also shell, Python, Ruby and Perl scripts; on a drive that keeps no
  file modes, where every file looks executable, one git has as
  executable or one your system would run), with Cancel as the
  default and Open Containing Folder offered instead. A symbolic
  link that leads out of the working tree isn't opened. File Explorer,
  and Open in File Explorer for submodules and worktrees, show folders
  and never launch an app, and a submodule path that isn't a path in
  the working tree (on Windows, a network path) is refused before
  anything touches it.
- **Removing a worktree could delete another folder.** Manage
  Worktrees › Remove deleted whichever folder the worktree's files in
  `.git` named. Anyone who could write to the repository's `.git`
  folder (a repository handed over as a folder or an archive, or kept
  in a shared folder) could make it delete any folder. GitBolt now
  checks that the worktree links back to the repository, as git does,
  and deletes nothing when it doesn't. The question names the folder
  that will be deleted.
- **Text from repositories and servers was read as formatting.**
  Branch, tag, path and submodule names, and messages from git and
  from servers, could carry markup that changed what a question said
  (another name shown in place of the real one) or, in a message box,
  added a link that one click sent to your browser or another app.
  Message boxes, status labels, tooltips and the password prompt now
  show such text exactly as it is written.
- **The Commit tab**: a commit's author or committer email could add
  attributes to its link, to restyle the tab or point the link at a
  file. The email is now just the email. Clicking a link there blanked
  the tab; a Parent or Child hash now selects that commit, and an email
  opens your mail app. A long author name or email no longer pushes the
  other rows out of view.
- **The Console** kept an escape sequence that never ended in memory,
  all of it, so printing a crafted file, or a server's messages, slowed
  the Console down more and more; and one short escape sequence could
  make it take gigabytes of memory, or, repeated, grow one line without
  end. All are limited now.
- **Repositories that froze or crashed GitBolt.** A pattern of many
  `*` in a committed `.gitattributes` or `.gitignore` made selecting a
  commit (in some repositories, opening one) run forever at full CPU.
  Patterns now give up after a bounded effort, and the patterns real
  repositories use match as before. A repository whose commits nest
  folders thousands of levels deep used up all memory when GitBolt
  opened it, and crashed the File Tree; as git does, GitBolt now stops
  at 2,048 levels ("tree too deep"). And a small `.gitattributes` that
  nests attribute macros could make GitBolt take gigabytes of memory;
  what one file's rules can expand to is now limited. These fixes are
  in the libgit2 built into every GitBolt download.

## [0.9.7] — 2026-10-18

Open in GitBolt comes to Finder on macOS, and a folder inside a
repository now opens that repository on every platform. Also: Fetch
and Push run in a bare repository instead of wherever GitBolt was
started, linked worktrees and submodules refresh on their own, Edit
URL keeps a remote's upstream branches, GitBolt's links follow the
project to its new home at github.com/Inverian/GitBolt, and there is a
security policy for reporting vulnerabilities privately.

### Added

- **Open in GitBolt from Finder (macOS)**: right-click a folder in
  Finder and choose Open in GitBolt (in the Services submenu when there
  are several) to open its repository, whether GitBolt is running or
  not. It is a macOS service that comes with GitBolt.app, so there is
  nothing to install; it shows once GitBolt has been opened once (if
  it doesn't, turn it on in System Settings › Keyboard › Keyboard
  Shortcuts… › Services › Files and Folders). Folders dropped on
  GitBolt's Dock icon, and `open -a GitBolt <folder>`, now open too;
  before, GitBolt ignored them.

### Changed

- **A folder inside a repository opens that repository**, from Finder,
  File › Open Repository, Explorer, Nautilus or `gitbolt <path>`.
  Before, only the repository's own folder opened, and Nautilus offered
  Open in GitBolt only there. The search stops at your home folder: a
  repository there (dotfiles, say) never claims a folder below it that
  is in no repository of its own.
- **A folder that is in no repository** now says so ("… is not in a
  Git repository."), instead of showing libgit2's error.
- **A minimized window comes back** when a folder is sent to GitBolt
  (Open in GitBolt from Finder or Explorer, a Dock drop,
  `gitbolt <path>`, and Nautilus on X11). Before, the repository opened
  in the window while it stayed minimized. Under Wayland GitBolt isn't
  told that its window is minimized, so it can't bring it back there.
- **Links**: the project moved to the Inverian organization on GitHub,
  and GitBolt's links went with it: Help › User manual, Report an
  issue and Check for updates, the About box, the home screen's
  Develop and Issues links, the privacy policy link in Settings, the
  Windows installer's help link and the Linux AppStream metadata now
  point to github.com/Inverian/GitBolt. The old links still redirect
  there.

### Fixed

- **Bare repositories**: with a bare repository open, Fetch, Push,
  Pull, the background fetch and the other commands GitBolt runs
  through git ran in GitBolt's own working folder instead of the
  repository. Started from a terminal inside another repository, Push
  pushed that repository's commits to its remote. They now run in the
  bare repository, also where git's `safe.bareRepository` setting is
  `explicit`. Pull and Commit, which need a working tree, are off
  there. File Explorer, Git bash and GitK open the bare repository,
  and Open Externally and Edit .gitignore (and the other working-tree
  files) say it has no working tree.
- **GIT_DIR, GIT_WORK_TREE and git's other repository variables in
  GitBolt's own environment** (a shell that exports them for a bare
  dotfiles repository, say) no longer reach anything GitBolt starts:
  its git commands, its Console and Git bash terminals, GitK, or an
  editor that Open Externally opens. They sent git's commands to that
  repository: Push pushed its commits while GitBolt showed the one
  that was open. An exported GIT_CONFIG hid the repository's own fetch
  settings from Fetch's prune check, so a fetch could delete local tags
  that the server doesn't have, and it left Settings › Git › Git Config
  empty, with Apply saving nothing.
- **Crashes on config lines with no value** (a name and no `= value`,
  which git reads as `true`, and refuses where it needs text): a
  committed `.gitmodules` whose `path` had none, or an empty one,
  crashed GitBolt as it opened the repository, a freshly cloned one
  included, and so did an `extensions.objectformat` with none. A
  `url.<base>.insteadOf` or `pushInsteadOf` with none, in any git
  config, your global one included, crashed Repository › Remote
  repositories and Plugins › Statistics, and a `branch.<name>.remote`
  with none crashed removing a remote. The repository now opens, or,
  for `objectformat`, says what is wrong with it.
- **A submodule that leads back out of its repository**: a
  `.gitmodules` whose submodule path is `/`, or goes through a committed
  symlink to `.` or `..` (one submodule down or several), made GitBolt
  open a repository it was already reading as a submodule of its own,
  over and over, until it crashed, as it opened the repository, a
  freshly cloned one included. Such a submodule is now taken as not
  checked out, and, like git, GitBolt no longer follows a submodule's
  path through a symlink.
- **Linked worktrees and submodules** opened in GitBolt now refresh
  when a commit, staging or a branch switch happens outside GitBolt.
  Their `.git` is a file pointing elsewhere, and GitBolt never watched
  the git dir it points to, so only changes to the files in the working
  tree were noticed.
- **Edit .git/info/exclude** works in a linked worktree (it edits the
  repository's shared `info/exclude`), and **Sparse Working Copy › Edit
  patterns** shows the worktree's own current patterns. Both looked
  for a `.git` folder that a linked worktree doesn't have: the first
  couldn't save, the second started empty.
- **Edit URL** (Repository › Remote repositories) changes only the
  remote's URL, as `git remote set-url` does. It used to remove the
  remote and add it back, which also dropped every branch's upstream
  link to it (Pull then failed, saying no branch was given), its
  remote-tracking branches, and its push URL, refspecs and options.
  A remote defined outside the repository's own config (in your global
  git config, a file a config includes, or a worktree's own
  config.worktree) is refused before anything changes, with a note
  saying where it is defined; before, Edit URL failed on such a remote
  only after dropping its remote-tracking branches and every branch's
  upstream link to it. A remote with several URLs (`git remote set-url
  --add`) is refused with a note that lists them and says where they
  are, or how to change them.

### Security

- **Security policy**: SECURITY.md says how to report a vulnerability
  privately, through GitHub's private vulnerability reporting, who
  reviews reports, and what to expect.

## [0.9.6] — 2026-10-16

Commits made in GitBolt now go through git, so your hooks and commit
signing apply; Fetch removes branches deleted on the server; and the
dark theme gets dark diffs. Also: a fix for a crash on Windows when
.git/config changed, git commands that no longer freeze the window, a
.deb that installs on Debian 13 and newer Ubuntu releases, and fixes
to amend, rebase, cherry-pick, launching and crash reports.

### Added

- **Cherry-picks and reverts of several commits**: a `git cherry-pick`
  or `git revert` of several commits, started in a terminal, can now
  be carried on from the in-progress bar. Once the commit it stopped
  at is committed, the bar offers Continue, which picks (or reverts)
  the rest. Before, the bar offered only Commit…, and once that commit
  was made the bar went away, with the rest of the commits still to
  come. Skip, offered where git can skip, leaves out the commit it
  stopped at. Continue asks first where git never picked that commit,
  Skip refuses where it would discard staged changes and Abort warns
  where it would, and Abort says when the commits made so far stay on
  the branch (git doesn't rewind past a commit made during the
  sequence).
- **Waiting for a busy GitBolt**: a second launch (Open in GitBolt
  from Explorer or Nautilus, or `gitbolt <path>`) that is still
  waiting for a busy GitBolt after 10 seconds shows a "GitBolt is
  busy" window. Its Cancel stops the wait, and the busy GitBolt then
  doesn't open that path.
- **Commit dialog**: when the commit concludes a merge, cherry-pick or
  revert, the message starts as the one git prepared for it (such as
  "Merge branch 'topic'"), without git's note of the conflicts. A
  merge's or a revert's comment lines go too, as `git commit` drops
  them in a terminal (core.commentChar and core.commentString are
  followed); a cherry-pick keeps the picked commit's message whole, so
  a subject such as "#42 Fix login" stays. While the dialog is open,
  the message follows the repository: it changes at the operation's
  next stop and clears once the operation ends. A message you have
  typed is kept.

### Changed

- **Commits** made in GitBolt now run `git commit`. They were written
  through libgit2, which ran no hooks and signed nothing. The
  repository's pre-commit, prepare-commit-msg, commit-msg and
  post-commit hooks now run (core.hooksPath is honoured),
  commit.gpgSign signs the commit, and a hook that turns the commit
  down shows what it said in the Commit dialog and the status bar.
  While git and its hooks run, the Commit button reads "Committing…",
  and staging, the toolbar, the sidebar's branch commands and the
  repository menus are off until it is done. A commit may take up to
  five minutes, for slow hooks; quitting stops git and its hooks. A
  commit that changes nothing is now refused, as git refuses it,
  where GitBolt used to make an empty commit.
- **PATH for git and its hooks**: on macOS, and on Linux when GitBolt
  is started from the desktop rather than a terminal, the git
  commands GitBolt runs, and the hooks they run, get the PATH your
  login shell sets up, ahead of the session's. Started from the Dock
  or Finder, GitBolt had only the system's PATH, so a hook that needs
  node or another tool from Homebrew, nvm or volta, such as a husky
  pre-push hook, failed with "command not found", as the commit hooks
  that now run (lint-staged's, say) would have.
- **Fetch** now removes the remote-tracking branches of branches
  deleted on the server, as `git fetch --prune` does, so they leave
  the sidebar too. This applies to Fetch and to the periodic
  background fetch, not to Pull. Where `fetch.prune` or
  `remote.<name>.prune` is set, that setting decides, as in git. Without
  either, GitBolt prunes only where nothing but remote-tracking
  branches could go: not where a remote fetches into refs outside
  refs/remotes/ (tags, or a mirror's branches) or pruneTags is set, in
  the repository or in any of its initialized submodules, nor where a
  submodule sets fetch.prune itself.
- **Periodic background fetch** never asks for anything: git runs with
  its prompts off and ssh's questions are answered no at once, without
  a window, so a fetch that needs a password, a passphrase or a new
  host key confirmed fails instead of showing GitBolt's always-on-top
  prompt at every interval. Credential helpers still answer, and a
  Fetch, Pull or Push you start still asks, unless a Fetch you click
  joins a periodic fetch already running.
- **New SSH host keys**: ssh's question whether to trust a server's
  host key is now a Yes/No box, No by default, with the fingerprint in
  plain text. It was a password field, so "yes" had to be typed
  blind.
- **Stopping git**: when a git command is stopped, by quitting, by
  Cancel or by its time limit, GitBolt now first interrupts it as
  Ctrl-C in a terminal would (on Windows, a Ctrl-C to git's console),
  so git removes its lock files, such as index.lock, and a hook can
  clean up. Whatever git started that is still running a few seconds
  later is then ended along with it. Before, a time limit on a local
  git command ended git alone, which could leave its hooks or filters
  running and index.lock behind, and on Windows a cancel ended
  everything at once.
- **Dark theme**: diffs (their line backgrounds and text), the
  conflict resolver's panes and headings, commit details on the Commit
  tab, file status colours in file lists, the toolbar's ✓ and ✗
  results, the home screen's card descriptions, the status colours in
  the Submodules and Worktrees windows, the File Tree's heading, the
  Clone dialog's status and secondary text such as Settings' notes now
  use dark-theme colours, with GitHub dark's greens, reds and blues.
  Most had the light theme's: pale green, red and blue bands across
  diffs, which left their text as faint as 1.2:1 contrast.
- **Light theme**: secondary text (Settings' notes, the home screen's
  card descriptions, the Commit dialog's ⓘ legend), commit details'
  links, dates, notes and +/− counts, the toolbar's ✓ and ✗ results,
  the Clone dialog's status and the conflict resolver's headings now
  have 4.5:1 contrast or more. Some, in the palette's border grey,
  read at 1.7:1.
- **Crash Detected**: the box now comes up over the main window once
  it is open, not before it, and says that the summary of the report
  "is being sent": it goes out at once, so turning usage statistics
  off afterwards deletes only what hasn't been sent yet.
- **Linux packages**: libgit2 (1.9.7) is now built into GitBolt, as on
  macOS and Windows. The .deb no longer depends on Ubuntu 24.04's
  libgit2-1.7 package, which kept it off Debian 13 and later Ubuntu
  releases; CI now installs and starts it on stock Ubuntu 24.04 and
  26.04 and Debian 13. The AppImage still needs GCC 13's C++ runtime
  or newer.
- **Building from source**: the English plural forms ("1 file", "2
  files") need Qt's LinguistTools and Qt 6.7 or later. Without them
  GitBolt still builds, and those texts read "1 file(s)";
  `-DGITBOLT_REQUIRE_ENGLISH_PLURALS=ON` makes the configure fail
  instead. docs/BUILDING.md lists the Linux packages that add them;
  Homebrew's Qt and the Qt installer's default install already have
  them.
- **Documentation**: the README's Download table lists the .deb for
  Ubuntu 24.04 or later and Debian 13, and its feature list no longer
  claims a cherry-pick of several selected commits: GitBolt picks one
  commit at a time, and carries on a multi-commit cherry-pick started
  in a terminal; nor does it list a macOS Finder extension, which was
  never shipped. PRIVACY.md says that a crash summary is sent as soon
  as the window is open, before other events, that turning usage
  statistics off deletes a summary not yet sent, and that a try that
  fails before reaching the server, as while offline, doesn't count
  towards an event's 5 tries.

### Fixed

- **Crash when .git/config changed**: GitBolt could crash when a
  repository's `.git/config` changed and then couldn't be read again.
  On Windows that could happen whenever `git config`, `git push -u` or
  a remote added in a terminal rewrote the file while GitBolt
  refreshed. The libgit2 built into every GitBolt download is now
  patched against it. libgit2 itself isn't fixed yet, so a GitBolt
  built from source against a system libgit2 doesn't have the fix.
  After such a failed read, GitBolt also went on using the old
  settings, its list of remotes among them, until `.git/config`
  changed again; it now reads the file again the next time it looks.
- **Amend** ("Amend last commit" in the Commit dialog) made you the
  author of the amended commit, dated at the amend. It now keeps the
  commit's author and author date, as git does. Amend is also off,
  with the reason as its tooltip, while a merge, cherry-pick or revert
  waits for its commit: amending there flattened the merge, or folded
  the pick or revert into the commit before it, and left the
  operation in progress.
- **Merging with merge.autoStash**: when a Merge, or a Pull that
  merges, on a working tree with uncommitted changes stopped on
  conflicts, concluding it with Commit… never put those changes back,
  and they were in neither the working tree nor the stash list. The
  commit now applies them ("Applied autostash."); if that conflicts,
  they stay in the stash and GitBolt says so.
- **Cherry-pick** is refused while a merge, rebase, revert or another
  cherry-pick is in progress. libgit2 wrote its pick over that
  operation's state, and a pick that then failed deleted it: an
  earlier pick was lost, and its commit went in as an ordinary one.
  Picking a commit whose changes are already on the branch now says so
  and leaves nothing in progress, where it reported success and left
  an empty pick for Commit….
- **Interactive rebase rewords**: a reword whose new message a hook
  turned down was dropped without a word on Continue, which said
  "Rebase complete." with the old message in place. Continue now tries
  the new message again; while the hook still refuses it, the rebase
  stays where it is and shows what the hook said. A reword stopped by
  a conflict and carried on in a terminal (`git rebase --continue` or
  `git commit`) now gets the new message too. In a repository whose
  i18n.commitEncoding is not UTF-8, reworded messages were labelled
  with that encoding though written in UTF-8, which garbled their
  accented letters; rewords GitBolt carries out are now stored as
  UTF-8.
- **Rebase Skip, Abort and quitting**: at a stop whose commit is
  already made (an `edit` stop, or a reword a hook turned down), Skip
  ran git's `--skip`, which resets the working tree there, and threw
  away your uncommitted changes. GitBolt now refuses and says to
  commit or stash them first, and its Skip question there says the
  commit stays in the branch. Abort's question there now says when
  uncommitted changes would be discarded too. Quitting during a rebase
  step waited for the step, up to five minutes, with no way to cancel,
  and a launch meanwhile said GitBolt wasn't responding; it now stops
  git as Ctrl-C would, and the rebase stays in progress.
- **Staging**: during a merge or cherry-pick, Unstage All ended the
  operation, and the merge then went in as an ordinary commit. It now
  unstages without ending it. When another git held the index lock, a
  Stage or Unstage that failed showed the file as staged (or
  unstaged) anyway, until something else changed the index, and an
  Unstage All that failed said nothing. The lists now stay right, and
  the failure is reported.
- **Commit & Push**: when the push couldn't start, the commit was made
  and the push skipped, with no more than a passing note that another
  remote operation was running, or none: only "ahead 1" showed that
  nothing was pushed. A periodic fetch running in the background now
  gives way, so the push goes ahead; otherwise the status bar says the
  commit wasn't pushed.
- **Commands that ask first** (Delete Branch, the Commit dialog's
  Discard, Undo Last Commit, Merge, Delete Tag, Clean, the stash
  manager's Drop, Delete Obsolete Branches, the sidebar's Rename and
  Set Upstream, and others) acted on whichever repository was open
  when you answered. If a second launch, such as Open in GitBolt from
  Explorer or Nautilus, or `gitbolt <path>` in a terminal, opened
  another repository while the question was up, that one lost the
  branch, the tag or its uncommitted changes. Such a command now does
  nothing then, and the status bar says "Another repository was
  opened; …".
- **Window freezes**: many commands ran git on the main thread, which
  froze the window until git finished or its time limit, 30 seconds
  for most, ran out; the limit then killed git and reported as failed
  a merge that had gone through. They now run in the background, one
  at a time (a fetch, pull or push can still run alongside), with the
  status bar saying what is running, and get five minutes: Merge,
  Checkout as local branch, the Reflog's Checkout and Reset, Undo Last
  Commit, Reset, Clean, Bisect, Archive, Format Patch, Apply Patch,
  Delete Obsolete Branches, Find Large Files, the sidebar's Rename and
  Set Upstream, and most submodule and sparse-checkout commands.
  Pushing a newly created tag now runs in the background too, as Push
  does. File History (Show History in the File Tree) also loads in the
  background, with a busy cursor and a note in the status bar.
- **Launching GitBolt while it is busy**: on macOS and Linux, a second
  launch gave up after 10 seconds and left its path with the busy
  GitBolt, which lost it if it then quit or crashed. It now waits, as
  on Windows, and if the busy GitBolt quits it opens in its place with
  that path. A Crash Detected box at startup no longer holds up a
  second launch: GitBolt answers it while the box is up, where the
  launch said "not responding" after 10 seconds and dropped its path.
  On Windows, Open in GitBolt in a drive's root folder (such as `E:\`)
  passed GitBolt a path ending in a quote, which it couldn't open.
- **Switching repositories**: the previous repository's commits could
  appear for a moment in the new one's history, and the open time sent
  with usage statistics came out too long. A rebase step's, a
  refresh's or a cherry-pick's result for the previous repository, or
  its commit's details on the Commit tab, could also show up in the
  new one.
- **Operation feedback**: a periodic fetch that showed on the toolbar
  (as after a Fetch clicked while it ran) and ended while another
  repository was opening could leave its label there, and a checkout
  or tag push that failed while a fetch, pull or push ran made that
  operation report a failure. Pushing a new tag while a fetch, pull or
  push runs is now turned down with the usual note; the tag is still
  created, to push later.
- **Repository › Close**: after closing a repository, a failed open
  turned Refresh, Fetch, Pull, Push, Commit and the repository menus
  back on, all acting on the closed repository.
- **Recent repositories**: File › Recent Repositories now follows the
  home screen's Clear Recent and Forget this repository at once. It
  kept the old list until the next open, and clicking a removed entry
  added it back. The home screen's recent list could also show one
  repository's branch, changes, ahead/behind counts and last-commit
  time in another's row.
- **File Tree**: Expand All or Collapse All could crash GitBolt when,
  while its menu was open, a second launch opened another repository
  or the history refreshed.
- **Sidebar**: "New Branch from 'X'..." created the branch at HEAD,
  not at X. It now starts at X, and its prompt says so.
- **Diffs**: a removed line starting with "--" (such as Markdown or
  YAML's "---", or an SQL comment) or an added line starting with "++"
  (such as "++i;") showed without its red or green, as if unchanged,
  and in bold in the unified view.
- **Counts** now take the English singular or plural, "1 file" and "2
  files" instead of "1 file(s)": the in-progress bar's conflict
  counts, File History, Delete Obsolete Branches, Find Large Files,
  the Git Command Log's replayed commands, the history's "days ago"
  and "yrs ago", the blame view's dates (which read "1 hours ago") and
  the File Tree's counts ("1 files, 1 directories", now "1 file, 1
  folder").
- **Terminal** (macOS and Linux): each read of the shell's output
  started again at the end of the text, so git's progress meters piled
  up on one line and editing in the middle of a command line showed
  the wrong text. The terminal now keeps the shell's cursor between
  reads and follows the cursor moves, erases, inserts and deletes
  within a line that shells and progress meters use; bash command
  lines longer than the terminal stay whole, and a window-title
  sequence split between two reads no longer prints as text. Lines
  still don't wrap, so a command line longer than the terminal can
  still show wrongly while it is edited: in zsh, and in bash when the
  cursor moves back into an earlier row.
- **About GitBolt** could cut off its build details and trademark text
  when dragged short, or when opened on a short screen (such as
  1366×768 at 125%). Its minimum height now fits its text.
- **Wayland**: GitBolt's windows now match its desktop entry, so GNOME
  and KDE show its icon and group its windows with its launcher and a
  pinned favourite, instead of a generic icon.
- **Signed tags**: a signed tag's tooltip in the sidebar showed its
  signature block (PGP, X.509 or SSH) under the message. It now leaves
  the block out.
- **Crash reports**: a crash that came back within seconds of every
  start was never reported, as the next crash replaced the summary
  before it was sent. The summary now goes first, as soon as the
  window is open. On macOS the function that crashed could be missing
  from the report's stack, and on Windows the summary of an unhandled
  C++ exception started in the C runtime instead of at the throw. A
  crash of GitBolt run by tests or the test bridge no longer replaces
  the report waiting from your own session: it goes to
  `~/.gitbolt/crash-test.log`.
- **Usage statistics**: turning them off in a second GitBolt process
  didn't stop the one that held the queue, which went on sending; that
  one now checks the setting before each request and at quit, and
  then deletes the queue. A queue lock left by another machine (a home
  folder shared between machines) or left half-written by a crash
  stopped usage statistics from being recorded or sent, for good, and
  kept the queue from being deleted; it is now cleared. A try that
  fails before reaching the server (the server's name not found, the
  connection refused, no route to it) no longer counts towards the 5
  tries after which an event is dropped, so being offline no longer
  uses up an event's tries; events still expire after 14 days.
- **Building from source on Linux** with the bundled libgit2
  (`-DGITBOLT_BUNDLED_LIBGIT2=ON`, or no libgit2 installed) and CMake 4
  stopped with "GIT_USE_NSEC defined but unknown struct stat nanosecond
  type". It now builds in a fresh build directory; in one that failed
  before, configure now says to start afresh.

## [0.9.5] — 2026-10-12

Usage statistics and crash reports, which show the maintainers which
features are used and what goes wrong, and a history that loads past
its first 256 commits.

### Added

- **Usage statistics and crash reports**: official GitBolt builds
  (releases, and CI's builds of `main`) now send usage statistics, and
  summaries of crashes and errors, to the GitBolt maintainers' own
  servers. They are on by default, with no pop-up, and
  [PRIVACY.md](https://github.com/ThePieMonster/GitBolt/blob/main/PRIVACY.md),
  linked from the About GitBolt dialog, the README and Settings, says
  exactly what is sent.
  - **Usage statistics**, under a random install ID and a device ID
    derived from it: each start (with the version, OS, CPU
    architecture, Qt version and system language), which menu and
    toolbar commands are used and how often, and how long repositories
    take to open and to show the first page of their history, with a
    rough count of that page's commits (0, 1-99, 100-255 or 256+).
  - **Crash and error reports**, without your install ID (each carries
    only a random report ID): which Git operation failed (such as
    fetch or push), that a commit failed, or the error code of a
    repository that would not open (such as not_found); and, after a
    crash, its kind (such as SIGSEGV) and its call stack, reduced to
    positions in GitBolt, Qt, libgit2 and the system's own libraries,
    with function names for those libraries only. Any other library
    shows as `<other>`.
  - **Never sent**: repository names or paths, branch, tag or remote
    names, remote URLs, commit messages or hashes, the names or
    contents of your files, email addresses and user names.
- **Tools › Settings › General › Usage statistics**, in official
  builds, turns them off: sending stops at once and anything still
  waiting is deleted. It also shows your install ID, with a Copy
  button, for deletion requests, and links to what is collected.
  `DO_NOT_TRACK=1` or `GITBOLT_TELEMETRY=0` turn them off too. Builds
  from source and forks contain no server addresses and send nothing,
  and GitBolt sends nothing when run by tests or in CI.

### Changed

- **Crash Detected**: when a summary of the crash report will be sent,
  the dialog says so, and that it can be turned off in Settings. The
  report GitBolt saves now names the version that crashed instead of
  "unknown".
- **Linux packages**: the .deb now depends on OpenSSL 3 (libssl3t64,
  or libssl3 on older releases), which GitBolt needs to send usage
  statistics over HTTPS, and its AppStream content rating now declares
  that GitBolt sends usage statistics under a persistent install ID
  (OARS social-info: intense).
- **README**: the Project Status section is gone, and a Privacy and
  usage statistics section is new.

### Fixed

- **History**: scrolling past a repository's first 256 commits added
  those same 256 again, so older commits never appeared. Each page now
  continues where the last one stopped.

## [0.9.4] — 2026-10-10

Tighter spacing around the toolbar's Commit and Stash buttons in dark
mode, the project's website in the app and its packages, and a
refreshed README.

### Changed

- **Toolbar**: in dark mode (the Dark theme, or System with a dark
  appearance), Commit and Stash, the two toolbar buttons that show
  their text, had more space after their text than the icon buttons
  have around theirs, as light mode did before 0.9.3. On macOS the gap
  from Commit's text to the Stash icon was 25.5pt, against 16 to 18.5pt
  between the icon buttons; it is now 17.5pt. Light mode is unchanged.
- **Website**: GitBolt's website, [www.gitbolt.com](https://www.gitbolt.com),
  is linked from the About GitBolt dialog, and is now the homepage the
  Windows installer registers and the .deb and Linux AppStream metadata
  list, in place of the GitHub repository. The AppStream metadata also
  lists the GitHub issue tracker and repository.
- **Packaging**: the .deb package's maintainer is now
  `GitBolt Team <support@gitbolt.com>`, replacing team@gitbolt.dev,
  an address at a domain the project does not own. GitBolt's Qt
  organization domain also changed from gitbolt.dev to gitbolt.com,
  and its AppStream developer ID from dev.gitbolt to com.gitbolt;
  settings stay where they are.
- **Documentation**: the README opens with a banner, status badges, a
  screenshot and links to the website, downloads, build guide,
  changelog and contributing guide, and gains a Download section
  listing each platform's installer and what it runs on.
  TRADEMARK.md names the website and the GitHub repository as the
  official project.

### Fixed

- **About GitBolt**, on Windows and Linux, could cut off the end of the
  OS line in its build details when dragged narrower than it opened;
  Windows 11 and most Linux distributions report a name long enough to
  hit this. Its minimum width now fits its contents, and is still at
  least 520 pixels.

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
