#include "update/InstallHelpers.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace gitbolt::update {

namespace {

QString shellLine(const char* name, const QString& value) {
    return QLatin1String(name) + u'=' + shellQuote(value) + u'\n';
}

// The start every Unix helper shares: its values, its log, and `say`.
// Everything the helper and the shell print goes to the log, whose
// folder GitBolt created; stdin is nothing, so no step can wait on it.
QString shellPreamble(const char* kind, const HelperInputs& in, const QString& values) {
    return QStringLiteral("#!/bin/sh\n"
                          "# GitBolt's update helper (%1), written by GitBolt\n"
                          "# (src/update/InstallHelpers.cpp) and started just before it\n"
                          "# quits. Plain POSIX sh: macOS runs it with bash in sh mode,\n"
                          "# Debian and Ubuntu with dash.\n")
               .arg(QLatin1String(kind))
           + QStringLiteral("pid=%1\nwait_seconds=%2\n").arg(in.pid).arg(in.waitSeconds) + values
           + shellLine("log", in.logFile)
           + QStringLiteral("\n"
                            "exec >>\"$log\" 2>&1 </dev/null\n"
                            "\n"
                            "say() {\n"
                            "    printf '%s %s\\n' \"$(date '+%Y-%m-%d %H:%M:%S')\" \"$*\"\n"
                            "}\n"
                            "\n");
}

// Waits for GitBolt to exit, at most wait_seconds; then runs
// `giveUp` and ends the helper with nothing changed. A zombie has
// exited (its parent hasn't reaped it yet), so it counts as gone:
// kill -0 alone would wait the full time for it. A fractional sleep
// isn't POSIX, but every sh this runs under has one; where it fails
// the loop polls every second.
QString shellWait(const QString& giveUp) {
    return QStringLiteral("say \"waiting for GitBolt (pid $pid) to quit\"\n"
                          "deadline=$(( $(date +%s) + $wait_seconds ))\n"
                          "while kill -0 \"$pid\" 2>/dev/null; do\n"
                          "    case $(ps -o stat= -p \"$pid\" 2>/dev/null) in\n"
                          "    Z*) break ;;\n"
                          "    esac\n"
                          "    if [ \"$(date +%s)\" -ge \"$deadline\" ]; then\n"
                          "        say \"GitBolt didn't quit within $wait_seconds seconds: giving up, nothing was changed\"\n"
                          "        %1\n"
                          "        exit 1\n"
                          "    fi\n"
                          "    sleep 0.2 2>/dev/null || sleep 1\n"
                          "done\n"
                          "say \"GitBolt has quit\"\n"
                          "\n")
        .arg(giveUp);
}

// Windows paths for NSIS and PowerShell, whatever the platform the
// script is built on (the tests build it everywhere).
QString windowsPath(QString path) {
    path.replace(u'/', u'\\');
    return path;
}

} // namespace

QString shellQuote(const QString& text) {
    QString quoted = text;
    quoted.replace(u'\'', QStringLiteral("'\\''"));
    return u'\'' + quoted + u'\'';
}

QString powerShellQuote(const QString& text) {
    QString quoted;
    quoted.reserve(text.size() + 2);
    quoted += u'\'';
    for (const QChar c : text) {
        quoted += c;
        const char16_t u = c.unicode();
        if (u == u'\'' || u == u'‘' || u == u'’' || u == u'‚' || u == u'‛')
            quoted += c;
    }
    quoted += u'\'';
    return quoted;
}

QByteArray powerShellEncodedCommand(const QString& script) {
    QByteArray utf16le;
    utf16le.reserve(script.size() * 2);
    for (const QChar c : script) {
        utf16le.append(static_cast<char>(c.unicode() & 0xff));
        utf16le.append(static_cast<char>(c.unicode() >> 8));
    }
    return utf16le.toBase64();
}

QStringList powerShellArguments(const QString& script) {
    return {QStringLiteral("-NoProfile"),   QStringLiteral("-NonInteractive"),
            QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
            QStringLiteral("-EncodedCommand"), QString::fromLatin1(powerShellEncodedCommand(script))};
}

QString macHelperScript(const HelperInputs& in) {
    const QString folder = QFileInfo(QDir::cleanPath(in.target)).path();
    const QString oldName = in.oldVersion.isEmpty() ? QStringLiteral("GitBolt")
                                                    : QStringLiteral("GitBolt-") + in.oldVersion;
    const QString values = shellLine("target", in.target) + shellLine("staged", in.staged)
                           + shellLine("aside", folder + QStringLiteral("/.") + oldName + QStringLiteral(".app.old"))
                           + shellLine("trash", in.trashDir) + shellLine("trash_name", oldName)
                           + shellLine("binary", in.executable)
                           + QStringLiteral("exec_directly=%1\n").arg(in.execDirectly ? 1 : 0);
    return shellPreamble("macOS", in, values)
           + QStringLiteral(
               "# Starts GitBolt from the bundle $1, which ends the helper.\n"
               "start() {\n"
               "    say \"starting $1\"\n"
               "    if [ \"$exec_directly\" = 1 ]; then\n"
               "        exec \"$1/$binary\"\n"
               "    fi\n"
               "    exec /usr/bin/open \"$1\"\n"
               "}\n"
               "\n"
               "say \"GitBolt update helper: $target, to be replaced by $staged\"\n")
           + shellWait(QStringLiteral("rm -rf \"$staged\""))
           + QStringLiteral(
               "if [ ! -d \"$staged\" ]; then\n"
               "    say \"the new copy, $staged, is missing: nothing was changed\"\n"
               "    start \"$target\"\n"
               "fi\n"
               "\n"
               "# The old copy aside, within its folder: a rename, which the\n"
               "# rollback can undo. mv into an existing folder would move it\n"
               "# inside, hence the checks that each destination is free.\n"
               "rm -rf \"$aside\"\n"
               "if [ -e \"$aside\" ] || ! mv \"$target\" \"$aside\"; then\n"
               "    say \"couldn't move $target aside: nothing was changed\"\n"
               "    rm -rf \"$staged\"\n"
               "    start \"$target\"\n"
               "fi\n"
               "if [ -e \"$target\" ] || ! mv \"$staged\" \"$target\"; then\n"
               "    say \"couldn't move the new copy into place: putting the old one back\"\n"
               "    if [ ! -e \"$target\" ] && mv \"$aside\" \"$target\"; then\n"
               "        rm -rf \"$staged\"\n"
               "        start \"$target\"\n"
               "    fi\n"
               "    say \"couldn't put the old copy back: it is $aside, and the new one $staged\"\n"
               "    exit 1\n"
               "fi\n"
               "say \"installed the new copy as $target\"\n"
               "\n"
               "# The old copy to the Trash, as Finder would put it there. If the\n"
               "# Trash is missing, on another volume (mv would copy the bundle),\n"
               "# or has the name even with the time added, it is deleted.\n"
               "dest=\"$trash/$trash_name.app\"\n"
               "if [ -e \"$dest\" ] || [ -L \"$dest\" ]; then\n"
               "    dest=\"$trash/$trash_name $(date '+%H.%M.%S').app\"\n"
               "fi\n"
               "if [ -d \"$trash\" ] && [ ! -e \"$dest\" ] && [ ! -L \"$dest\" ] \\\n"
               "    && [ \"$(stat -f %d \"$trash\")\" = \"$(stat -f %d \"$aside\")\" ] \\\n"
               "    && mv \"$aside\" \"$dest\"; then\n"
               "    say \"moved the old copy to $dest\"\n"
               "elif rm -rf \"$aside\"; then\n"
               "    say \"deleted the old copy: the Trash didn't take it\"\n"
               "else\n"
               "    say \"couldn't delete the old copy, $aside\"\n"
               "fi\n"
               "start \"$target\"\n");
}

QString appImageHelperScript(const HelperInputs& in) {
    const QString values = shellLine("target", in.target) + shellLine("staged", in.staged);
    return shellPreamble("AppImage", in, values)
           + QStringLiteral("say \"GitBolt update helper: $target, to be replaced by $staged\"\n")
           + shellWait(QStringLiteral("rm -f \"$staged\""))
           + QStringLiteral(
               "# A rename within one folder: the new file takes the old one's\n"
               "# path in one step (the old one, which its runtime no longer\n"
               "# holds open, goes with it).\n"
               "if [ -f \"$staged\" ] && [ ! -d \"$target\" ] && mv -f \"$staged\" \"$target\"; then\n"
               "    say \"installed the new AppImage as $target\"\n"
               "else\n"
               "    say \"couldn't move the new AppImage over $target: keeping the old one\"\n"
               "    rm -f \"$staged\"\n"
               "fi\n"
               "say \"starting $target\"\n"
               "exec \"$target\"\n");
}

QString debHelperScript(const HelperInputs& in) {
    const QString values = shellLine("deb", in.staged) + shellLine("pkexec", in.pkexec)
                           + shellLine("relaunch", in.executable);
    return shellPreamble("deb", in, values)
           + QStringLiteral("say \"GitBolt update helper: installing $deb\"\n")
           + shellWait(QStringLiteral("rm -f \"$deb\""))
           + QStringLiteral(
               "# apt-get rather than dpkg -i, so that new dependencies are\n"
               "# installed too. pkexec asks for an administrator's password\n"
               "# through the desktop's polkit agent. The lock timeout rides out\n"
               "# an unattended upgrade that holds dpkg's lock for a moment.\n"
               "say \"running apt-get through pkexec\"\n"
               "\"$pkexec\" /usr/bin/apt-get install -y -o DPkg::Lock::Timeout=60 \"$deb\"\n"
               "status=$?\n"
               "if [ \"$status\" -eq 0 ]; then\n"
               "    say \"installed $deb\"\n"
               "else\n"
               "    say \"apt-get (or pkexec) failed with status $status: the old version stays\"\n"
               "fi\n"
               "say \"starting $relaunch\"\n"
               "exec \"$relaunch\"\n");
}

QString windowsHelperScript(const HelperInputs& in) {
    const QString dir = windowsPath(in.target);
    const QString app = (dir.endsWith(u'\\') ? dir : dir + u'\\') + QStringLiteral("bin\\GitBolt.exe");
    QStringList lines = {
        QStringLiteral("# GitBolt's update helper (Windows), written by GitBolt"),
        QStringLiteral("# (src/update/InstallHelpers.cpp) and started hidden just before it quits."),
        QStringLiteral("$ErrorActionPreference = 'Stop'"),
        QStringLiteral("$log = ") + powerShellQuote(windowsPath(in.logFile)),
        // $pid is PowerShell's own (read-only): this process's id.
        QStringLiteral("$appPid = %1").arg(in.pid),
        QStringLiteral("$waitSeconds = %1").arg(in.waitSeconds),
        QStringLiteral("$installer = ") + powerShellQuote(windowsPath(in.staged)),
        QStringLiteral("$app = ") + powerShellQuote(app),
        QStringLiteral("function Write-Log([string]$text) {"),
        QStringLiteral("    try {"),
        QStringLiteral("        Add-Content -LiteralPath $log -Encoding UTF8 -Value ((Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' ' + $text)"),
        QStringLiteral("    } catch {"),
        QStringLiteral("    }"),
        QStringLiteral("}"),
        QStringLiteral("Write-Log ('GitBolt update helper: waiting for GitBolt (pid ' + $appPid + ') to quit')"),
        QStringLiteral("try {"),
        QStringLiteral("    Wait-Process -Id $appPid -Timeout $waitSeconds -ErrorAction SilentlyContinue"),
        QStringLiteral("} catch {"),
        QStringLiteral("}"),
        QStringLiteral("if (Get-Process -Id $appPid -ErrorAction SilentlyContinue) {"),
        QStringLiteral("    Write-Log ('GitBolt did not quit within ' + $waitSeconds + ' seconds: giving up, nothing was changed')"),
        QStringLiteral("    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue"),
        QStringLiteral("    exit 1"),
        QStringLiteral("}"),
        QStringLiteral("Write-Log ('running ' + $installer)"),
        // /D= last and unquoted: NSIS takes the rest of the command line
        // as the folder. Silent (/S), over the same folder, elevated: the
        // installer requires admin, which only ShellExecute's runas verb
        // can ask for.
        QStringLiteral("try {"),
        QStringLiteral("    $setup = Start-Process -FilePath $installer -ArgumentList ")
            + powerShellQuote(QStringLiteral("/S /D=") + dir)
            + QStringLiteral(" -Verb RunAs -Wait -PassThru"),
        QStringLiteral("    Write-Log ('the installer exited with code ' + $setup.ExitCode)"),
        QStringLiteral("} catch {"),
        QStringLiteral("    Write-Log ('the installer did not run: ' + $_.Exception.Message)"),
        QStringLiteral("}"),
        // Either way: the new copy, or the old one if it didn't install.
        QStringLiteral("Write-Log ('starting ' + $app)"),
        QStringLiteral("try {"),
        QStringLiteral("    Start-Process -FilePath $app"),
        QStringLiteral("} catch {"),
        QStringLiteral("    Write-Log ('could not start GitBolt: ' + $_.Exception.Message)"),
        QStringLiteral("}"),
    };
    return lines.join(u'\n') + u'\n';
}

} // namespace gitbolt::update
