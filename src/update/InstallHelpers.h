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
/// does to `logFile`, and starts GitBolt again whether the install
/// worked or not: the new copy, or the old one. GitBolt may only start
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
};

/// macOS: after GitBolt quits, move the old bundle aside within its
/// folder, rename the staged one into its place (both on one volume,
/// so each step is a rename), then move the old one to the Trash (or
/// delete it) and start the new one. If the new one can't be put in
/// place, the old one is moved back and started.
QString macHelperScript(const HelperInputs& in);
/// AppImage: after GitBolt quits, rename the new file over $APPIMAGE
/// (same path, so desktop entries and launchers keep working; the
/// running runtime held the old file open, so this was safe all along)
/// and start it.
QString appImageHelperScript(const HelperInputs& in);
/// Deb: after GitBolt quits, `pkexec /usr/bin/apt-get install -y` the
/// .deb (apt-get, not dpkg -i, so new dependencies are resolved), log
/// apt's status, and start /usr/bin/gitbolt either way.
QString debHelperScript(const HelperInputs& in);
/// Windows (PowerShell): after GitBolt quits, run the NSIS installer
/// elevated (UAC asks) and silently over the same install folder, wait
/// for it, log its exit code, and start <target>\bin\GitBolt.exe
/// either way. The installer is started as
///   Start-Process -FilePath '<exe>' -ArgumentList '/S /D=<dir>' -Verb RunAs -Wait -PassThru
/// with /D= last and unquoted, as NSIS requires: it takes the rest of
/// the command line as the folder, spaces and all.
QString windowsHelperScript(const HelperInputs& in);

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

} // namespace gitbolt::update
