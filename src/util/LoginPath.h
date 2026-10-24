#pragma once

#include <QString>

namespace gitbolt::util {

/// The PATH of the user's login shell, for the processes GitBolt starts.
///
/// An app started from the Dock, Finder or a desktop menu gets the
/// PATH of the session (launchd's /usr/bin:/bin:/usr/sbin:/sbin on
/// macOS), not the one the user's shell sets up: Homebrew's, nvm's,
/// volta's. git runs the repository's hooks with it, and a husky or
/// lint-staged hook ("node: command not found") turned every commit
/// down. libgit2's commits ran no hooks, so this never showed before
/// commits went through git.
///
/// startLoginPathLookup() asks the login shell once, in the background
/// (`$SHELL -ilc`, in a session of its own, with no terminal and no
/// input, for at most kLoginPathTimeoutMs). pathForChildren() then puts
/// its entries ahead of the process's own.

/// How long the login shell may take to say.
inline constexpr int kLoginPathTimeoutMs = 5000;

/// Starts the lookup with `shell` (empty: $SHELL, else the user's
/// shell in the password database). Once per process; later calls do
/// nothing. Nothing happens on Windows.
void startLoginPathLookup(const QString& shell = {});

/// Whether this launch should look the PATH up: on macOS always (a
/// GUI app gets launchd's), elsewhere on Unix when stdin isn't a
/// terminal (started from a desktop, not a shell); never on Windows.
bool shouldLookUpLoginPath();

/// `own` with the login shell's entries ahead of it (mergePathLists()),
/// or `own` as it is when no lookup was started or it found nothing.
/// Waits for a lookup still running (at most its timeout).
QString pathForChildren(const QString& own);

/// `first`'s entries, then those of `second` that `first` lacks, each
/// once, in order, empty ones dropped (':'-separated; ';' on Windows).
QString mergePathLists(const QString& first, const QString& second);

/// The first `name` in an absolute folder of `pathList` (split as
/// mergePathLists() splits it) that is an executable file, by its full
/// path; empty if there is none. A relative folder ("." among them) is
/// passed over: it is GitBolt's current folder or one below it, which
/// on Windows, after Explorer's "Open in GitBolt", was the repository.
/// Nothing listed finds nothing, not what GitBolt's own PATH has. On
/// Windows a `name` without an extension is looked for as name.exe
/// only, not as the .bat or .cmd PATHEXT allows (cmd.exe runs those).
QString findExecutableOnPath(const QString& name, const QString& pathList);

/// Runs `shell -ilc` in a session of its own, with no terminal and
/// stdin and stderr on /dev/null, and returns the PATH it set up. Empty
/// when it couldn't run, said nothing usable (an entry that isn't an
/// absolute path), or took longer than `timeoutMs` (it's killed, with
/// whatever it started). Unix only.
QString queryLoginShellPath(const QString& shell, int timeoutMs);

} // namespace gitbolt::util
