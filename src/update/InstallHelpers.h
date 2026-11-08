#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace gitbolt::update {

/// What a helper script needs to finish an update after GitBolt quits.
/// The helpers are text built from these values alone, so a test can
/// read each one and, on Unix, run it against scratch folders.
///
/// Every helper waits for `pid` to exit (at most `waitSeconds`, then
/// it gives up, removes `staged` and changes nothing), appends what it
/// does to `logFile`, writes the install result (below) to
/// `resultFile`, and starts GitBolt again whether the install worked or
/// not: the new copy, or the old one. GitBolt may only start
/// once the old process is gone, because the single-instance lock dies
/// with it (a copy started earlier is folded into the old one and
/// exits). The helper inherits GitBolt's environment and passes it on,
/// so GITBOLT_INSTANCE_NAME and the test bridge's variables reach the
/// relaunched copy.
struct HelperInputs {
    qint64 pid = 0;
    int waitSeconds = 600;
    QString logFile;
    /// What is replaced. MacBundle: the bundle (…/GitBolt.app);
    /// AppImage: the .AppImage file ($APPIMAGE); WindowsInstall: the
    /// install folder (NSIS's /D=). Unused for Deb.
    QString target;
    /// The new copy. MacBundle: the checked bundle staged beside the
    /// target (<folder>/.GitBolt-<v>.app); AppImage: the checked file
    /// beside the old one; WindowsInstall: the installer .exe; Deb: the
    /// .deb, by absolute path (apt-get takes a path only with a slash).
    QString staged;
    /// MacBundle: the running version, which names the old copy in the
    /// Trash (GitBolt-<old>.app) and beside the target while it is
    /// swapped (<folder>/.GitBolt-<old>.app.old).
    QString oldVersion;
    /// MacBundle: <HOME>/.Trash. The old copy goes there, as Finder
    /// would put it; if the Trash refuses it (missing, another volume,
    /// or a name taken twice) it is deleted instead.
    QString trashDir;
    /// What is started afterwards. MacBundle: the binary inside the
    /// bundle, relative to it (Contents/MacOS/GitBolt), used when
    /// `execDirectly`; Deb: /usr/bin/gitbolt. Unused for AppImage (the
    /// target is started) and WindowsInstall (<target>\bin\GitBolt.exe).
    QString executable;
    /// MacBundle: exec the binary itself, keeping the environment, as
    /// test sessions (GITBOLT_INSTANCE_NAME set) need; otherwise
    /// /usr/bin/open the bundle, as Finder would, so LaunchServices
    /// starts it (and its environment is a normal launch's).
    bool execDirectly = false;
    /// Deb: pkexec's absolute path, which asks for root through the
    /// desktop's polkit agent.
    QString pkexec;
    /// The version being installed, x.y.z: the install result names it,
    /// and on Windows the installed GitBolt.exe must say it afterwards.
    QString newVersion;
    /// Where the helper writes the install result (Installer::resultPath,
    /// <workDir>/install-result): see "The install result" below. Empty:
    /// none is written.
    QString resultFile;
    /// AppImage: this GitBolt runs extracted (the runtime's
    /// --appimage-extract-and-run, for want of FUSE), which the runtime
    /// doesn't pass on to the next start; the new one is started with
    /// APPIMAGE_EXTRACT_AND_RUN=1 so it comes up the same way. See
    /// appImageRunsExtracted.
    bool extractAndRun = false;
    /// Windows: the administrator part's pid, as ShellExecuteExW gave
    /// it, or 0 if it didn't. The helper stops waiting for that part's
    /// install result a moment after the process is gone (PowerShell
    /// couldn't run there, say), rather than for `installSeconds`.
    qint64 elevatedPid = 0;
    /// Windows: how long the helper waits, once GitBolt has quit, for
    /// the administrator part's install result. Then it writes failed
    /// itself and starts GitBolt anyway.
    int installSeconds = 15 * 60;
};

/// The install result. Every helper writes `resultFile` once, just
/// before it starts GitBolt again or gives up (Windows: the
/// administrator part does, as its last step, and the helper only if
/// that part didn't): one UTF-8 line,
///   <status> <version> <reason>
/// and a newline. `version` is HelperInputs::newVersion; `reason` is
/// short plain English, possibly empty, never with a newline (it never
/// quotes a path or a tool's output). `status` is one of:
///   - ok: the new version is in place;
///   - declined: the user refused the administrator prompt (pkexec's
///     polkit dialog dismissed; Windows' UAC is asked while GitBolt
///     still runs, which hears a No itself);
///   - failed: anything else that left the old version in place (the
///     installer or apt-get failed, the files couldn't be swapped and
///     were put back, the installed version isn't the new one, pkexec
///     couldn't authenticate);
///   - gave-up: GitBolt didn't quit within waitSeconds.
/// The GitBolt that starts next reads it and deletes it, and tells the
/// user when status isn't ok and the version is newer than itself.

/// macOS: after GitBolt quits, move the old bundle aside within its
/// folder, rename the staged one into its place (both on one volume,
/// so each step is a rename), then move the old one to the Trash (or
/// delete it) and start the new one. If the new one can't be put in
/// place, the old one is moved back and started.
QString macHelperScript(const HelperInputs& in);
/// AppImage: after GitBolt quits, rename the new file over $APPIMAGE
/// (same path, so desktop entries and launchers keep working; the
/// running runtime held the old file open, so this was safe all along)
/// and start it, extracted again if this one was (`extractAndRun`).
QString appImageHelperScript(const HelperInputs& in);
/// Deb: after GitBolt quits, `pkexec /usr/bin/apt-get install -y` the
/// .deb (apt-get, not dpkg -i, so new dependencies are resolved), log
/// apt's status, and start /usr/bin/gitbolt either way. pkexec's 126
/// (its dialog dismissed) is "declined"; its 127 (it couldn't
/// authenticate: no polkit agent to ask with, polkit doesn't allow it
/// or isn't set up; or another error of pkexec's) and anything else is
/// "failed".
QString debHelperScript(const HelperInputs& in);
/// Windows has two parts, both PowerShell run by -EncodedCommand, and
/// GitBolt starts both while it still runs (launchWindowsUpdate in
/// Installer.h): first windowsElevatedScript, as an administrator,
/// through ShellExecuteExW's runas with GitBolt's window in front, so
/// that UAC's prompt comes up over it (not as a taskbar button after
/// GitBolt has gone) and a No leaves GitBolt running; then this one,
/// not elevated, which starts GitBolt again afterwards. GitBolt starts
/// it only once Windows said yes. It waits for GitBolt (`pid`) to quit,
/// at most waitSeconds and a little more than the administrator part
/// does: if GitBolt is still there then, it starts nothing (the
/// administrator part gave up and said so; if it never did, this one
/// writes gave-up). Then it waits, at most `installSeconds`, until the
/// administrator part has written the install result (the last thing
/// it does), or until that part's process (`elevatedPid`) has been
/// gone a few seconds without one; with no result by then it writes
/// failed itself. Either way it starts <target>\bin\GitBolt.exe, not
/// elevated: the new copy, or the old one if nothing was installed.
/// Both parts use cmdlets and core types only outside a try, so in
/// PowerShell's Constrained Language mode they still report and the
/// helper still starts GitBolt again. Both append to the one log
/// (logFile) through windowsScriptPreamble's Write-Log.
QString windowsHelperScript(const HelperInputs& in);
/// The administrator part of a Windows update. It waits for GitBolt
/// (`pid`) to quit, at most waitSeconds; then it gives up, deletes the
/// installer and writes gave-up. Elevated, it can see every session's
/// processes and open files a filtered token can't, so only here can it
/// tell whether the install is in use: another user's GitBolt (fast
/// user switching, RDS), or one reopened since. For up to
/// kWindowsInUseSeconds it waits until no GitBolt but `pid` runs from
/// the install folder and bin\GitBolt.exe can be opened with no sharing
/// (a running image can't be); the NSIS installer would otherwise
/// uninstall around the locked files and install a mix. Then it runs
/// the installer silently over the folder, already elevated,
///   Start-Process -FilePath $installer -ArgumentList '/S /D=<dir>' -Wait -PassThru
/// with /D= last and unquoted, as NSIS requires (it takes the rest of
/// the command line as the folder, spaces and all), checks that
/// bin\GitBolt.exe's ProductVersion (the version resource's, x.y.z) is
/// now `newVersion`, logs what happened, and writes the install result,
/// last. Its exit code is 0 for ok, or one of the kWindowsExit* codes,
/// or the installer's own when it failed.
QString windowsElevatedScript(const HelperInputs& in);

/// What both Windows parts start with, one PowerShell line each: their
/// shared values ($log, $result, $version, $appPid, $waitSeconds), then
/// Write-Log and Write-Result. Write-Log '<text>' appends a line, with
/// the time, to the log, which both parts write to at once: through
/// Out-File, so one writer has it at a time and readers (GitBolt's
/// logTail) never stand in the way. A line that meets the other part's,
/// or anything else holding the log, is tried again for at most
/// kWindowsLogPatienceSeconds and, if it still can't go in, kept and
/// written with the next one; Write-Log '' writes only what is kept,
/// with the full patience (each part's last steps; what it still can't
/// write is lost). Write-Result writes the install result,
/// with `onlyFirst` (the helper's) only if there is none yet. Exposed
/// so that the tests can run Write-Log for real.
QString windowsScriptPreamble(const HelperInputs& in, bool onlyFirst);

/// How long one Write-Log in a Windows part waits, at most, for the log
/// to take its line. Once one has waited that long in vain, the next
/// ones with a line try once only, until a line goes in again; Write-Log
/// '' (a part's flush of what is kept) always waits this long while
/// anything is kept. So a log that stays busy costs the helper at most
/// twice this (its first line, its last flush) and the administrator
/// part three times (its first line, its flushes before and after its
/// result), not this much a line. Three times it is under
/// kWindowsHelperExtraSeconds, so that the administrator part still
/// gives up, and writes its result, before the helper gives up.
inline constexpr int kWindowsLogPatienceSeconds = 3;

/// How long the elevated Windows helper waits for the install to be
/// free before it gives up and installs nothing.
inline constexpr int kWindowsInUseSeconds = 60;
/// How much longer than waitSeconds the Windows helper waits for
/// GitBolt to quit: the administrator part, started first, has decided
/// by then whether it gave up.
inline constexpr int kWindowsHelperExtraSeconds = 15;
/// The elevated Windows helper's exit codes beside 0 (installed and
/// checked) and the installer's own (NSIS: 1 cancelled, 2 aborted).
inline constexpr int kWindowsExitInUse = 1001;         ///< still in use after kWindowsInUseSeconds
inline constexpr int kWindowsExitNoInstaller = 1002;   ///< the installer didn't start
inline constexpr int kWindowsExitWrongVersion = 1003;  ///< installed, but GitBolt.exe isn't newVersion
inline constexpr int kWindowsExitError = 1004;         ///< the script itself failed
inline constexpr int kWindowsExitGaveUp = 1005;        ///< GitBolt didn't quit within waitSeconds

/// Whether an AppImage runs extracted rather than from its FUSE mount:
/// the runtime was asked to (APPIMAGE_EXTRACT_AND_RUN set, any value,
/// as the runtime's own test is), or $APPDIR (`appDir`) is an existing
/// folder that isn't a mount point (the runtime's
/// <tmp>/appimage_extracted_<hash>), as after --appimage-extract-and-run,
/// which the runtime consumes without setting the variable. False for
/// an empty or missing `appDir`.
bool appImageRunsExtracted(const QString& appDir, bool extractAndRunSet);

/// `text` as one POSIX shell word: in single quotes, each ' written as
/// '\''. Safe for any text, newlines included.
QString shellQuote(const QString& text);
/// `text` as a PowerShell single-quoted string. PowerShell ends one at
/// any of ' ‘ ’ ‚ ‛ (U+0027, U+2018-U+201B), so each of them is doubled;
/// nothing else is special inside one.
QString powerShellQuote(const QString& text);
/// What powershell.exe -EncodedCommand takes: the script as UTF-16LE,
/// in base64. A script passed this way is not a file, so the execution
/// policy doesn't apply to it, and no quoting of the command line can
/// break it.
QByteArray powerShellEncodedCommand(const QString& script);
/// powershell.exe's arguments for the Windows helper: no profile, no
/// prompts, no window, and the script by -EncodedCommand.
QStringList powerShellArguments(const QString& script);
/// The same arguments as one string, for ShellExecuteExW's lpParameters
/// (the administrator part): none of them needs quoting.
QString powerShellParameters(const QString& script);
/// What CreateProcess allows on a command line, the program included.
inline constexpr int kWindowsCommandLineLimit = 32767;

} // namespace gitbolt::update
