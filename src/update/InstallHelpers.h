#pragma once

#include "update/Failure.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace gitbolt::update {

/// What a helper script needs to finish an update after GitBolt quits.
/// Every helper waits up to `waitSeconds` for `pid` to exit (else it
/// removes `staged` and changes nothing), logs to `logFile`, writes the
/// install result, then starts GitBolt again: not sooner, or the new copy
/// would hand over to the old one's single-instance lock and exit. It
/// passes GitBolt's environment on (macOS: with `execDirectly`).
struct HelperInputs {
    qint64 pid = 0;
    int waitSeconds = 600;
    QString logFile;
    /// What is replaced. MacBundle: the bundle (…/GitBolt.app);
    /// WindowsInstall: the install folder (NSIS's /D=). Unused for Deb.
    QString target;
    /// The new copy. MacBundle: the checked bundle staged beside the
    /// target (<folder>/.GitBolt-<v>.app); WindowsInstall: the
    /// installer .exe; Deb: the .deb, by absolute path (apt-get takes a
    /// path only with a slash).
    QString staged;
    /// MacBundle: the running version, which names the old copy in the
    /// Trash (GitBolt-<old>.app) and beside the target while it is
    /// swapped (<folder>/.GitBolt-<old>.app.old).
    QString oldVersion;
    /// MacBundle: <HOME>/.Trash, where the old copy goes. If the Trash is
    /// missing, on another volume, or has its name even with the time
    /// added, the old copy is deleted instead.
    QString trashDir;
    /// What is started afterwards. MacBundle: the binary inside the
    /// bundle, relative to it (Contents/MacOS/GitBolt), used when
    /// `execDirectly`; Deb: /usr/bin/gitbolt. Unused for WindowsInstall
    /// (<target>\bin\GitBolt.exe).
    QString executable;
    /// MacBundle: exec the binary itself, keeping the environment, as
    /// test sessions (GITBOLT_INSTANCE_NAME set) need; otherwise
    /// /usr/bin/open the bundle, so LaunchServices starts it as usual.
    bool execDirectly = false;
    /// Deb: pkexec's absolute path, which asks for root through the
    /// desktop's polkit agent.
    QString pkexec;
    /// The version being installed, x.y.z: the install result names it,
    /// and on Windows the installed GitBolt.exe must say it afterwards.
    QString newVersion;
    /// Where the install result (below) goes: Installer::resultPath.
    /// Empty: none is written.
    QString resultFile;
    /// Windows: the administrator part's pid from ShellExecuteExW, or 0.
    /// Once that process has been gone a few seconds with no install
    /// result, the helper stops waiting for one.
    qint64 elevatedPid = 0;
    /// Windows: how long the helper waits, once GitBolt has quit, for
    /// the administrator part's install result. Then it writes failed
    /// itself and starts GitBolt anyway.
    int installSeconds = 15 * 60;
};

/// The install result: `resultFile`, one UTF-8 line written once, just
/// before GitBolt starts again or the helper gives up:
/// "<status> <newVersion> <reason>\n". `reason` is short plain English,
/// maybe empty, with no newline, path or tool output. `status`: ok,
/// declined (pkexec's dialog dismissed), failed (the new version isn't in
/// place) or gave-up (GitBolt didn't quit in time). The next GitBolt reads
/// and deletes it, and reports anything but ok for a newer version.

/// The fixed code for an install result: by status (ok: None, gave-up:
/// QuitTimeout, declined: PermissionDeclined), and for failed by the
/// start of the helpers' own reason (Other when none matches). A failed
/// result is read by the version that wrote it (the old copy starts
/// again), so the helpers' words need no other format.
Failure installResultFailure(const QString& status, const QString& reason);

/// macOS: after GitBolt quits, rename the old bundle aside in its folder
/// and the staged one into its place, move the old one to the Trash (or
/// delete it) and start the new one. If the new one can't be put in
/// place, the old one is moved back and started.
QString macHelperScript(const HelperInputs& in);
/// Deb: after GitBolt quits, `pkexec /usr/bin/apt-get install -y` the
/// .deb (apt-get, not dpkg -i, so new dependencies are resolved), then
/// start /usr/bin/gitbolt either way. pkexec's 126 (dialog dismissed)
/// is "declined"; 127 (couldn't authenticate) and anything else, "failed".
QString debHelperScript(const HelperInputs& in);
/// Windows: the unelevated part, which starts GitBolt again. It waits up
/// to waitSeconds + kWindowsHelperExtraSeconds for `pid` to quit (else it
/// writes gave-up and starts nothing), then up to `installSeconds` for the
/// administrator part's result (failed if none comes, or that part ends
/// without one), then starts <target>\bin\GitBolt.exe, new or old. Cmdlets
/// and core types only outside a try, so Constrained Language mode works.
QString windowsHelperScript(const HelperInputs& in);
/// Windows: the administrator part. It waits up to waitSeconds for `pid`
/// to quit (else deletes the installer and writes gave-up), then up to
/// kWindowsInUseSeconds until no GitBolt runs from the install folder, so
/// NSIS replaces no file in use (only elevated does it see other sessions'
/// GitBolts and open protected files). Then it runs '/S /D=<dir>' (/D= last
/// and unquoted, as NSIS requires), checks bin\GitBolt.exe's version and
/// writes the result. Exits 0, a kWindowsExit* code, or the installer's.
QString windowsElevatedScript(const HelperInputs& in);

/// The start of both Windows parts: shared values, Write-Log and
/// Write-Result. Write-Log '<text>' appends a timed line via Out-File (one
/// writer at a time; readers never block it). A refused line is retried
/// for up to kWindowsLogPatienceSeconds, then kept for the next call;
/// Write-Log '' flushes what is kept (lost if that fails too). With
/// `onlyFirst`, Write-Result writes only if there is no result yet.
QString windowsScriptPreamble(const HelperInputs& in, bool onlyFirst);

/// The longest one Write-Log waits for the log. After a wait in vain,
/// lines get one try each until one goes in; a flush (Write-Log '') always
/// gets the full wait. A busy log thus costs the helper up to 2x this and
/// the administrator part 3x, which must stay under
/// kWindowsHelperExtraSeconds so that part's result comes first.
inline constexpr int kWindowsLogPatienceSeconds = 3;

/// How long the administrator part waits for the install folder to be
/// free before it gives up and installs nothing.
inline constexpr int kWindowsInUseSeconds = 60;
/// How much longer than waitSeconds the Windows helper waits for
/// GitBolt to quit: the administrator part, started first, has decided
/// by then whether it gave up.
inline constexpr int kWindowsHelperExtraSeconds = 15;
/// The administrator part's exit codes besides 0 (installed and checked)
/// and the installer's own (NSIS: 1 cancelled, 2 aborted).
inline constexpr int kWindowsExitInUse = 1001;         ///< still in use after kWindowsInUseSeconds
inline constexpr int kWindowsExitNoInstaller = 1002;   ///< the installer didn't start
inline constexpr int kWindowsExitWrongVersion = 1003;  ///< installed, but GitBolt.exe isn't newVersion
inline constexpr int kWindowsExitError = 1004;         ///< the script itself failed
inline constexpr int kWindowsExitGaveUp = 1005;        ///< GitBolt didn't quit within waitSeconds

/// `text` as one POSIX shell word: in single quotes, each ' written as
/// '\''. Safe for any text, newlines included.
QString shellQuote(const QString& text);
/// `text` as a PowerShell single-quoted string. PowerShell ends one at
/// any of ' ‘ ’ ‚ ‛ (U+0027, U+2018-U+201B), so each of them is doubled;
/// nothing else is special inside one.
QString powerShellQuote(const QString& text);
/// What powershell.exe -EncodedCommand takes: the script as UTF-16LE, in
/// base64. Not a file, so the execution policy doesn't apply, and no
/// quoting of the command line can break it.
QByteArray powerShellEncodedCommand(const QString& script);
/// powershell.exe's arguments for a Windows part: no profile, no
/// prompts, no window, and the script by -EncodedCommand.
QStringList powerShellArguments(const QString& script);
/// The same arguments as one string, for ShellExecuteExW's lpParameters
/// (the administrator part): none of them needs quoting.
QString powerShellParameters(const QString& script);
/// What CreateProcess allows on a command line, the program included.
inline constexpr int kWindowsCommandLineLimit = 32767;

} // namespace gitbolt::update
