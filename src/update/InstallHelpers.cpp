#include "update/InstallHelpers.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace gitbolt::update {

namespace {

QString shellLine(const char* name, const QString& value) {
    return QLatin1String(name) + u'=' + shellQuote(value) + u'\n';
}

// The start every Unix helper shares: its values, `say` and `report`.
// All output goes to the log, and stdin is /dev/null, so no step can
// wait on it.
QString shellPreamble(const char* kind, const HelperInputs& in, const QString& values) {
    return QStringLiteral("#!/bin/sh\n"
                          "# GitBolt's update helper (%1), written by GitBolt\n"
                          "# (src/update/InstallHelpers.cpp) and started just before it\n"
                          "# quits. Plain POSIX sh: macOS runs it with bash in sh mode,\n"
                          "# Debian and Ubuntu with dash.\n")
               .arg(QLatin1String(kind))
           + QStringLiteral("pid=%1\nwait_seconds=%2\n").arg(in.pid).arg(in.waitSeconds) + values
           + shellLine("new_version", in.newVersion) + shellLine("result_file", in.resultFile)
           + shellLine("log", in.logFile)
           + QStringLiteral("\n"
                            "exec >>\"$log\" 2>&1 </dev/null\n"
                            "\n"
                            "say() {\n"
                            "    printf '%s %s\\n' \"$(date '+%Y-%m-%d %H:%M:%S')\" \"$*\"\n"
                            "}\n"
                            "\n"
                            "# The install result, for the GitBolt that starts next: one line,\n"
                            "# \"<status> <version> <reason>\", status ok, declined, failed or\n"
                            "# gave-up (InstallHelpers.h). Written once, just before GitBolt is\n"
                            "# started again or the helper gives up.\n"
                            "report() {\n"
                            "    [ -n \"$result_file\" ] || return 0\n"
                            "    rm -f \"$result_file\"\n"
                            "    printf '%s %s %s\\n' \"$1\" \"$new_version\" \"$2\" >\"$result_file\"\n"
                            "}\n"
                            "\n");
}

// Waits for GitBolt to exit, at most wait_seconds, else runs `giveUp`,
// reports gave-up and exits. A zombie counts as gone (kill -0 alone
// would wait for it); where sleep 0.2 fails (not POSIX), it sleeps 1.
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
                          "        report gave-up \"GitBolt didn't quit within $wait_seconds seconds\"\n"
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

QString windowsArgument(const QString& text) {
    QString quoted = QStringLiteral("\"");
    qsizetype backslashes = 0;
    for (const QChar c : text) {
        if (c == u'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes count only before a quote: there each is doubled,
        // and the quote escaped.
        quoted += QString(c == u'"' ? 2 * backslashes + 1 : backslashes, u'\\');
        quoted += c;
        backslashes = 0;
    }
    return quoted + QString(2 * backslashes, u'\\') + u'"';
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

QString powerShellParameters(const QString& script) {
    return powerShellArguments(script).join(u' ');
}

Failure installResultFailure(const QString& status, const QString& reason) {
    if (status == QLatin1String("ok"))
        return Failure::None;
    if (status == QLatin1String("gave-up"))
        return Failure::QuitTimeout;
    if (status == QLatin1String("declined"))
        return Failure::PermissionDeclined;
    if (status != QLatin1String("failed"))
        return Failure::Other;
    // The reasons the helpers below write (report, Write-Result).
    static const struct {
        const char* start;
        Failure failure;
    } kReasons[] = {
        {"the new copy was missing", Failure::NewCopyMissing},
        {"the old copy couldn't be moved aside", Failure::MoveAsideFailed},
        {"the new copy couldn't be moved into place, so the old one was put back", Failure::MoveIntoPlaceFailed},
        {"the new copy couldn't be moved into place, nor the old one back", Failure::RollbackFailed},
        {"pkexec couldn't authenticate", Failure::ElevationFailed},
        {"apt-get failed with status", Failure::AptFailed},
        {"another GitBolt window was still open", Failure::InUse},
        {"another program was using", Failure::FileInUse},
        {"the installer could not be started", Failure::InstallerNotStarted},
        {"the installer ran, but GitBolt.exe is not version", Failure::WrongVersion},
        {"the administrator part of the update failed", Failure::ElevatedError},
        {"the installer exited with code", Failure::InstallerExit},
        {"the administrator part of the update did not finish within", Failure::ElevatedTimeout},
        {"the administrator part of the update ended without a result", Failure::ElevatedNoResult},
        {"the installer ran, but Uninstall.exe is missing", Failure::UninstallerMissing},
    };
    for (const auto& known : kReasons) {
        if (reason.startsWith(QLatin1String(known.start)))
            return known.failure;
    }
    return Failure::Other;
}

QString macHelperScript(const HelperInputs& in) {
    const QString folder = QFileInfo(QDir::cleanPath(in.target)).path();
    const QString oldName = in.oldVersion.isEmpty() ? QStringLiteral("GitBolt")
                                                    : QStringLiteral("GitBolt-") + in.oldVersion;
    const QString values = shellLine("target", in.target) + shellLine("staged", in.staged)
                           + shellLine("aside", folder + QStringLiteral("/.") + oldName + QStringLiteral(".app.old"))
                           + shellLine("trash", in.trashDir) + shellLine("trash_name", oldName)
                           + shellLine("binary", in.executable) + shellLine("reopen", in.reopen)
                           + QStringLiteral("exec_directly=%1\n").arg(in.execDirectly ? 1 : 0);
    return shellPreamble("macOS", in, values)
           + QStringLiteral(
               "# Starts GitBolt from the bundle $1, which ends the helper: with\n"
               "# the repository it had open, and marked as the helper's (open\n"
               "# passes no environment on, but by then the result is there).\n"
               "start() {\n"
               "    say \"starting $1\"\n"
               "    app=$1\n"
               "    set --\n"
               "    [ -z \"$reopen\" ] || set -- \"$reopen\"\n"
               "    GITBOLT_UPDATE_RELAUNCH=1\n"
               "    export GITBOLT_UPDATE_RELAUNCH\n"
               "    if [ \"$exec_directly\" = 1 ]; then\n"
               "        exec \"$app/$binary\" \"$@\"\n"
               "    fi\n"
               "    [ $# -eq 0 ] || set -- --args \"$@\"\n"
               "    # -n: a copy turned away during the update may not have\n"
               "    # exited yet, and open alone would bring that one forward.\n"
               "    exec /usr/bin/open -n \"$app\" \"$@\"\n"
               "}\n"
               "\n"
               "say \"GitBolt update helper: $target, to be replaced by $staged\"\n")
           + shellWait(QStringLiteral("rm -rf \"$staged\""))
           + QStringLiteral(
               "if [ ! -d \"$staged\" ]; then\n"
               "    say \"the new copy, $staged, is missing: nothing was changed\"\n"
               "    report failed \"the new copy was missing\"\n"
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
               "    report failed \"the old copy couldn't be moved aside\"\n"
               "    start \"$target\"\n"
               "fi\n"
               "if [ -e \"$target\" ] || ! mv \"$staged\" \"$target\"; then\n"
               "    say \"couldn't move the new copy into place: putting the old one back\"\n"
               "    if [ ! -e \"$target\" ] && mv \"$aside\" \"$target\"; then\n"
               "        rm -rf \"$staged\"\n"
               "        report failed \"the new copy couldn't be moved into place, so the old one was put back\"\n"
               "        start \"$target\"\n"
               "    fi\n"
               "    say \"couldn't put the old copy back: it is $aside, and the new one $staged\"\n"
               "    report failed \"the new copy couldn't be moved into place, nor the old one back\"\n"
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
               "report ok \"\"\n"
               "start \"$target\"\n");
}

QString debHelperScript(const HelperInputs& in) {
    const QString values = shellLine("deb", in.staged) + shellLine("pkexec", in.pkexec)
                           + shellLine("relaunch", in.executable) + shellLine("reopen", in.reopen);
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
               "# pkexec's own: 126, its dialog was dismissed; 127, it couldn't\n"
               "# authenticate (no polkit agent runs in this session: a bare\n"
               "# window manager, a remote X session; or polkit doesn't allow\n"
               "# this user, or isn't set up), or another error of its own,\n"
               "# whose line is in the log above. apt-get's failures are 100.\n"
               "case $status in\n"
               "0)\n"
               "    say \"installed $deb\"\n"
               "    report ok \"\"\n"
               "    ;;\n"
               "126)\n"
               "    say \"pkexec wasn't given permission (status 126): the old version stays\"\n"
               "    report declined \"the administrator password prompt was dismissed\"\n"
               "    ;;\n"
               "127)\n"
               "    say \"pkexec couldn't authenticate (status 127): no polkit authentication agent, or polkit isn't set up to allow it. The old version stays\"\n"
               "    report failed \"pkexec couldn't authenticate (no polkit authentication agent is running, or polkit isn't set up to allow it)\"\n"
               "    ;;\n"
               "*)\n"
               "    say \"apt-get (or pkexec) failed with status $status: the old version stays\"\n"
               "    report failed \"apt-get failed with status $status\"\n"
               "    ;;\n"
               "esac\n"
               "# With the repository it had open, marked as the helper's.\n"
               "say \"starting $relaunch\"\n"
               "GITBOLT_UPDATE_RELAUNCH=1\n"
               "export GITBOLT_UPDATE_RELAUNCH\n"
               "if [ -n \"$reopen\" ]; then\n"
               "    exec \"$relaunch\" \"$reopen\"\n"
               "fi\n"
               "exec \"$relaunch\"\n");
}

namespace {

// The administrator part must give up, and write its result, before the
// helper gives up: its first line, give-up line and flush before the
// result can each wait one patience.
static_assert(kWindowsLogPatienceSeconds > 0 && 3 * kWindowsLogPatienceSeconds < kWindowsHelperExtraSeconds,
              "the administrator part's first line, give-up line and flush before its result must fit in the "
              "helper's extra wait");

// Shared start of both Windows parts: their values, Write-Log and
// Write-Result, cmdlets and core types only (Constrained Language).
// Write-Log uses Out-File, not Add-Content, whose 5.1 retry leaks a
// handle that blocks later writes (PowerShell#27947). A refused line
// is retried for up to $logPatience s, then kept for the next call.
// onlyFirst: Write-Result writes only if there is no result yet.
QStringList windowsPreambleLines(const HelperInputs& in, bool onlyFirst) {
    const QString patience = QString::number(kWindowsLogPatienceSeconds);
    return {
        QStringLiteral("$ErrorActionPreference = 'Stop'"),
        QStringLiteral("$log = ") + powerShellQuote(windowsPath(in.logFile)),
        QStringLiteral("$result = ") + powerShellQuote(windowsPath(in.resultFile)),
        QStringLiteral("$version = ") + powerShellQuote(in.newVersion),
        // $pid is PowerShell's own (read-only): this process's id.
        QStringLiteral("$appPid = %1").arg(in.pid),
        QStringLiteral("$waitSeconds = %1").arg(in.waitSeconds),
        // script: here and in Write-Log, so both share them in any scope.
        QStringLiteral("$script:logPending = ''"),
        QStringLiteral("$script:logPatience = ") + patience,
        QStringLiteral("function Write-Log([string]$text) {"),
        QStringLiteral("    $patience = ") + patience,
        QStringLiteral("    if ($text -ne '') {"),
        QStringLiteral("        $script:logPending += (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' ' + $text + [char]13 + [char]10"),
        QStringLiteral("        $patience = $script:logPatience"),
        QStringLiteral("    }"),
        QStringLiteral("    $until = (Get-Date).AddSeconds($patience)"),
        QStringLiteral("    while ($script:logPending) {"),
        QStringLiteral("        try {"),
        QStringLiteral("            Out-File -LiteralPath $log -Append -Encoding UTF8 -NoNewline -InputObject $script:logPending"),
        QStringLiteral("            $script:logPending = ''"),
        QStringLiteral("            $script:logPatience = ") + patience,
        QStringLiteral("        } catch {"),
        QStringLiteral("            if ((Get-Date) -ge $until) {"),
        QStringLiteral("                $script:logPatience = 0"),
        QStringLiteral("                try {"),
        QStringLiteral("                    Write-Warning ('install.log: ' + $_.Exception.Message)"),
        QStringLiteral("                } catch {"),
        QStringLiteral("                }"),
        QStringLiteral("                return"),
        QStringLiteral("            }"),
        QStringLiteral("            Start-Sleep -Milliseconds (Get-Random -Minimum 10 -Maximum 100)"),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        QStringLiteral("}"),
        QStringLiteral("function Write-Result([string]$status, [string]$reason) {"),
        onlyFirst ? QStringLiteral("    if ($result -eq '' -or (Test-Path -LiteralPath $result)) {")
                  : QStringLiteral("    if ($result -eq '') {"),
        QStringLiteral("        return"),
        QStringLiteral("    }"),
        QStringLiteral("    try {"),
        // Ascii for no BOM, so every reason must be ASCII (else '?').
        QStringLiteral("        Set-Content -LiteralPath $result -Encoding Ascii -NoNewline -Value "
                       "($status + ' ' + $version + ' ' + ($reason -replace '[\\r\\n]+', ' ') + [char]10)"),
        QStringLiteral("    } catch {"),
        QStringLiteral("        Write-Log ('could not write the install result: ' + $_.Exception.Message)"),
        QStringLiteral("    }"),
        QStringLiteral("}"),
    };
}

// The install folder's GitBolt.exe, in Windows form; a folder at a
// drive's root keeps one backslash.
QString windowsApp(const QString& dir) {
    return (dir.endsWith(u'\\') ? dir : dir + u'\\') + QStringLiteral("bin\\GitBolt.exe");
}

} // namespace

QString windowsScriptPreamble(const HelperInputs& in, bool onlyFirst) {
    return windowsPreambleLines(in, onlyFirst).join(u'\n');
}

QString windowsElevatedScript(const HelperInputs& in) {
    const QString dir = windowsPath(in.target);
    const QString prefix = dir.endsWith(u'\\') ? dir : dir + u'\\';
    // No comments (each character costs ~3 of the command line's 32767)
    // and no double quotes, so nothing in it is ever expanded.
    QStringList lines = windowsPreambleLines(in, false);
    lines << QStringList{
        QStringLiteral("$prefix = ") + powerShellQuote(prefix),
        QStringLiteral("$app = ") + powerShellQuote(windowsApp(dir)),
        QStringLiteral("$installer = ") + powerShellQuote(windowsPath(in.staged)),
        QStringLiteral("Write-Log ('administrator part: waiting for GitBolt (pid ' + $appPid + ') to quit')"),
        QStringLiteral("$code = 0"),
        QStringLiteral("try {"),
        // Polls the process list, not Wait-Process: an over-the-shoulder
        // administrator may not be allowed to open the user's GitBolt.
        QStringLiteral("    $deadline = (Get-Date).AddSeconds($waitSeconds)"),
        QStringLiteral("    while (Get-Process -Id $appPid -ErrorAction SilentlyContinue) {"),
        QStringLiteral("        if ((Get-Date) -ge $deadline) {"),
        QStringLiteral("            $code = %1").arg(kWindowsExitGaveUp),
        QStringLiteral("            break"),
        QStringLiteral("        }"),
        QStringLiteral("        Start-Sleep -Milliseconds 250"),
        QStringLiteral("    }"),
        QStringLiteral("    if ($code -eq 0) {"),
        QStringLiteral("        $deadline = (Get-Date).AddSeconds(%1)").arg(in.inUseSeconds),
        QStringLiteral("        $said = ''"),
        QStringLiteral("        while ($true) {"),
        QStringLiteral("            $busy = ''"),
        // A case-insensitive prefix by -eq: Constrained Language refuses
        // [StringComparison].
        QStringLiteral("            $inUse = @(Get-Process -Name GitBolt -ErrorAction SilentlyContinue | Where-Object {"),
        QStringLiteral("                try { $_.Id -ne $appPid -and $_.Path -and $_.Path.Length -ge $prefix.Length -and "
                       "$_.Path.Substring(0, $prefix.Length) -eq $prefix } catch { $false }"),
        QStringLiteral("            }).Count -gt 0"),
        // GitBolt's files: the folder's own, and all of bin and plugins
        // (never a whole drive, should it be installed at a root).
        QStringLiteral("            if (-not $inUse) {"),
        QStringLiteral("                $files = @(Get-ChildItem -LiteralPath $prefix -File -Force -ErrorAction SilentlyContinue) + "
                       "@(Get-ChildItem -LiteralPath ($prefix + 'bin'), ($prefix + 'plugins') -Recurse -File -Force "
                       "-ErrorAction SilentlyContinue)"),
        QStringLiteral("                foreach ($file in $files) {"),
        // Opened as NSIS's File opens it (write, sharing reads only), so
        // a loaded DLL counts, a reader that shares writing doesn't. In use
        // only on a sharing or lock violation (32, 33); where Constrained
        // Language refuses [IO.File], the process list and the installer's
        // own check decide.
        QStringLiteral("                    try {"),
        QStringLiteral("                        [IO.File]::Open($file.FullName, 'Open', 'Write', 'Read').Dispose()"),
        QStringLiteral("                    } catch {"),
        QStringLiteral("                        $e = $_.Exception"),
        QStringLiteral("                        while ($e.InnerException) {"),
        QStringLiteral("                            $e = $e.InnerException"),
        QStringLiteral("                        }"),
        QStringLiteral("                        if ($busy -eq '' -and @(32, 33) -contains ($e.HResult -band 0xFFFF)) {"),
        QStringLiteral("                            $busy = $file.FullName.Replace($prefix, '')"),
        QStringLiteral("                        }"),
        QStringLiteral("                    }"),
        QStringLiteral("                }"),
        QStringLiteral("            }"),
        QStringLiteral("            if (-not $inUse -and $busy -eq '') {"),
        QStringLiteral("                break"),
        QStringLiteral("            }"),
        // What it waits for, logged when that changes.
        QStringLiteral("            $now = 'another GitBolt window to close'"),
        QStringLiteral("            if ($busy) {"),
        QStringLiteral("                $now = 'another program to close ' + $busy"),
        QStringLiteral("            }"),
        QStringLiteral("            if ($now -ne $said) {"),
        QStringLiteral("                Write-Log ('waiting for ' + $now)"),
        QStringLiteral("                $said = $now"),
        QStringLiteral("            }"),
        QStringLiteral("            if ((Get-Date) -ge $deadline) {"),
        QStringLiteral("                $code = %1").arg(kWindowsExitInUse),
        QStringLiteral("                if ($busy) {"),
        QStringLiteral("                    $code = %1").arg(kWindowsExitFileInUse),
        QStringLiteral("                }"),
        QStringLiteral("                break"),
        QStringLiteral("            }"),
        QStringLiteral("            Start-Sleep -Seconds 1"),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        // Run twice at most: again if Uninstall.exe, which the uninstall
        // key runs, is missing after a run that went well or gave up (2).
        QStringLiteral("    foreach ($run in 1, 2) {"),
        // /D= last and unquoted: NSIS takes the rest as the folder.
        QStringLiteral("        if ($code -eq 0) {"),
        QStringLiteral("            Write-Log ('running ' + $installer)"),
        QStringLiteral("            try {"),
        QStringLiteral("                $setup = Start-Process -FilePath $installer -ArgumentList ")
            + powerShellQuote(QStringLiteral("/S /D=") + dir) + QStringLiteral(" -Wait -PassThru"),
        QStringLiteral("            } catch {"),
        QStringLiteral("                $code = %1").arg(kWindowsExitNoInstaller),
        QStringLiteral("                Write-Log ('the installer could not be started: ' + $_.Exception.Message)"),
        QStringLiteral("            }"),
        QStringLiteral("        }"),
        // No exit code at all: the version below decides.
        QStringLiteral("        if ($code -eq 0) {"),
        QStringLiteral("            $setupCode = $setup.ExitCode"),
        QStringLiteral("            if ($null -ne $setupCode -and $setupCode -ne 0) {"),
        QStringLiteral("                $code = $setupCode"),
        QStringLiteral("            }"),
        QStringLiteral("        }"),
        QStringLiteral("        if ($code -eq 0) {"),
        QStringLiteral("            $found = [string](Get-Item -LiteralPath $app).VersionInfo.ProductVersion"),
        QStringLiteral("            if ($found.Trim() -ne $version) {"),
        QStringLiteral("                $code = %1").arg(kWindowsExitWrongVersion),
        QStringLiteral("            }"),
        QStringLiteral("        }"),
        // After a second run, whatever stopped it: the first one installed.
        QStringLiteral("        if (($code -eq 0 -or $code -eq 2 -or $run -eq 2) -and "
                       "-not (Test-Path -LiteralPath ($prefix + 'Uninstall.exe'))) {"),
        QStringLiteral("            if ($code -ne 0) {"),
        QStringLiteral("                Write-Log ('Uninstall.exe is missing, and the run ended with code ' + $code)"),
        QStringLiteral("            }"),
        QStringLiteral("            $code = %1").arg(kWindowsExitNoUninstaller),
        QStringLiteral("        }"),
        QStringLiteral("        if ($code -ne %1 -or $run -eq 2) {").arg(kWindowsExitNoUninstaller),
        QStringLiteral("            break"),
        QStringLiteral("        }"),
        QStringLiteral("        $code = 0"),
        QStringLiteral("        Write-Log 'the installer ran, but Uninstall.exe is missing: running it again'"),
        QStringLiteral("    }"),
        QStringLiteral("} catch {"),
        QStringLiteral("    $code = %1").arg(kWindowsExitError),
        QStringLiteral("    Write-Log ('the administrator part failed: ' + $_.Exception.Message)"),
        QStringLiteral("}"),
        // What the code means, for the log and the install result.
        QStringLiteral("$status = 'failed'"),
        QStringLiteral("$reason = ''"),
        QStringLiteral("if ($code -eq 0) {"),
        QStringLiteral("    $status = 'ok'"),
        QStringLiteral("    Write-Log ('the installer exited with code 0, and GitBolt.exe is now version ' + $version)"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitGaveUp),
        QStringLiteral("    $status = 'gave-up'"),
        QStringLiteral("    $reason = 'GitBolt did not quit within ' + $waitSeconds + ' seconds'"),
        QStringLiteral("    Write-Log ('GitBolt did not quit within ' + $waitSeconds + ' seconds: giving up, nothing was changed')"),
        QStringLiteral("    Remove-Item -LiteralPath $installer -Force -ErrorAction SilentlyContinue"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitInUse),
        QStringLiteral("    $reason = 'another GitBolt window was still open'"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitFileInUse),
        QStringLiteral("    $reason = 'another program was using ' + $busy"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsInstallerExitInUse),
        QStringLiteral("    $reason = 'another program was using GitBolt files'"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitNoInstaller),
        QStringLiteral("    $reason = 'the installer could not be started'"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitWrongVersion),
        QStringLiteral("    $reason = 'the installer ran, but GitBolt.exe is not version ' + $version"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitNoUninstaller),
        QStringLiteral("    $reason = 'the installer ran, but Uninstall.exe is missing'"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitError),
        QStringLiteral("    $reason = 'the administrator part of the update failed'"),
        QStringLiteral("} else {"),
        QStringLiteral("    $reason = 'the installer exited with code ' + $code"),
        QStringLiteral("}"),
        // Nothing was installed only if the installer never ran, or stopped
        // before changing anything; else, as UpdateController says,
        // GitBolt may be incomplete.
        QStringLiteral("if (@(%1, %2, %3, %4) -contains $code) {")
            .arg(kWindowsExitInUse)
            .arg(kWindowsExitFileInUse)
            .arg(kWindowsInstallerExitInUse)
            .arg(kWindowsExitNoInstaller),
        QStringLiteral("    Write-Log ('nothing was installed (code ' + $code + '): ' + $reason)"),
        QStringLiteral("} elseif ($status -eq 'failed') {"),
        QStringLiteral("    Write-Log ('GitBolt may be incomplete (code ' + $code + '): ' + $reason)"),
        QStringLiteral("}"),
        // Flush before the result: once it exists, the helper starts GitBolt.
        QStringLiteral("Write-Log ''"),
        QStringLiteral("Write-Result $status $reason"),
        // And what Write-Result may have logged.
        QStringLiteral("Write-Log ''"),
        QStringLiteral("exit $code"),
    };
    return lines.join(u'\n');
}

QString windowsHelperScript(const HelperInputs& in) {
    // 5.1's Start-Process passes ArgumentList's one string on as it is.
    const QString start = in.reopen.isEmpty()
                              ? QStringLiteral("    Start-Process -FilePath $app")
                              : QStringLiteral("    Start-Process -FilePath $app -ArgumentList ")
                                    + powerShellQuote(windowsArgument(windowsPath(in.reopen)));
    QStringList lines = {
        QStringLiteral("# GitBolt's update helper (Windows), written by GitBolt"),
        QStringLiteral("# (src/update/InstallHelpers.cpp) and started hidden just before it quits, once"),
        QStringLiteral("# Windows let the administrator part (windowsElevatedScript) run. It starts"),
        QStringLiteral("# GitBolt again when that part is done, not elevated."),
    };
    lines << windowsPreambleLines(in, true);
    lines << QStringList{
        QStringLiteral("$elevatedPid = %1").arg(in.elevatedPid),
        QStringLiteral("$installSeconds = %1").arg(in.installSeconds),
        QStringLiteral("$app = ") + powerShellQuote(windowsApp(windowsPath(in.target))),
        QStringLiteral("Write-Log ('GitBolt update helper: waiting for GitBolt (pid ' + $appPid + ') to quit')"),
        // Longer than the administrator part waits: whatever that part
        // decided, a GitBolt that quits in time is started again.
        QStringLiteral("$deadline = (Get-Date).AddSeconds($waitSeconds + %1)").arg(kWindowsHelperExtraSeconds),
        QStringLiteral("while (Get-Process -Id $appPid -ErrorAction SilentlyContinue) {"),
        QStringLiteral("    if ((Get-Date) -ge $deadline) {"),
        QStringLiteral("        Write-Log ('GitBolt did not quit within ' + $waitSeconds + ' seconds: it is not started again')"),
        QStringLiteral("        Write-Result 'gave-up' ('GitBolt did not quit within ' + $waitSeconds + ' seconds')"),
        QStringLiteral("        Write-Log ''"),
        QStringLiteral("        exit 1"),
        QStringLiteral("    }"),
        QStringLiteral("    Start-Sleep -Milliseconds 250"),
        QStringLiteral("}"),
        QStringLiteral("Write-Log ('GitBolt has quit: waiting for the administrator part (pid ' + $elevatedPid + "
                       "') to install GitBolt ' + $version)"),
        // 5 s of grace once the administrator part has gone: its result
        // is its last step but a flush, so it can come just as it ends.
        QStringLiteral("$deadline = (Get-Date).AddSeconds($installSeconds)"),
        QStringLiteral("$endedBy = $null"),
        QStringLiteral("while (-not ($result -ne '' -and (Test-Path -LiteralPath $result))) {"),
        QStringLiteral("    if ((Get-Date) -ge $deadline) {"),
        QStringLiteral("        Write-Log ('the administrator part did not finish within ' + $installSeconds + ' seconds')"),
        QStringLiteral("        Write-Result 'failed' ('the administrator part of the update did not finish within ' + "
                       "$installSeconds + ' seconds')"),
        QStringLiteral("        break"),
        QStringLiteral("    }"),
        QStringLiteral("    if ($elevatedPid -gt 0 -and -not (Get-Process -Id $elevatedPid -ErrorAction SilentlyContinue)) {"),
        QStringLiteral("        if ($null -eq $endedBy) {"),
        QStringLiteral("            $endedBy = (Get-Date).AddSeconds(5)"),
        QStringLiteral("        } elseif ((Get-Date) -ge $endedBy) {"),
        QStringLiteral("            Write-Log ('the administrator part (pid ' + $elevatedPid + ') ended without an install result')"),
        QStringLiteral("            Write-Result 'failed' 'the administrator part of the update ended without a result'"),
        QStringLiteral("            break"),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        QStringLiteral("    Start-Sleep -Milliseconds 250"),
        QStringLiteral("}"),
        // Either way: the new copy, or the old one if it didn't install;
        // with the repository it had open, marked as the helper's.
        QStringLiteral("$env:%1 = '1'").arg(QLatin1String(kRelaunchEnv)),
        QStringLiteral("Write-Log ('starting ' + $app)"),
        QStringLiteral("try {"),
        start,
        QStringLiteral("} catch {"),
        QStringLiteral("    Write-Log ('could not start GitBolt: ' + $_.Exception.Message)"),
        QStringLiteral("}"),
        // Anything the log refused so far.
        QStringLiteral("Write-Log ''"),
    };
    return lines.join(u'\n') + u'\n';
}

QString windowsNoticeScript(const QString& text, int seconds) {
    // WScript.Shell's Popup is MessageBox: 64 an information icon, 4096
    // system modal, which keeps it in front.
    return QStringLiteral("(New-Object -ComObject WScript.Shell).Popup(%1, %2, 'GitBolt', 4160) | Out-Null\n")
        .arg(powerShellQuote(text), QString::number(seconds));
}

} // namespace gitbolt::update
