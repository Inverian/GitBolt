#include "update/InstallHelpers.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
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
               "    report ok \"\"\n"
               "else\n"
               "    say \"couldn't move the new AppImage over $target: keeping the old one\"\n"
               "    rm -f \"$staged\"\n"
               "    report failed \"the new AppImage couldn't be moved over the old one\"\n"
               "fi\n")
           + (in.extractAndRun ? QStringLiteral(
                  "# This GitBolt ran extracted (--appimage-extract-and-run: no FUSE\n"
                  "# here), which the runtime doesn't pass on; so must the next one.\n"
                  "APPIMAGE_EXTRACT_AND_RUN=1\n"
                  "export APPIMAGE_EXTRACT_AND_RUN\n")
                               : QString())
           + QStringLiteral(
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
               "say \"starting $relaunch\"\n"
               "exec \"$relaunch\"\n");
}

namespace {

static_assert(kWindowsLogPatienceSeconds > 0 && 2 * kWindowsLogPatienceSeconds < kWindowsHelperExtraSeconds,
              "each part's first Write-Log may wait this long before its deadline starts");

// What both Windows parts start with: the values they share, the log,
// and the install result (windowsScriptPreamble in InstallHelpers.h).
// Cmdlets and core types only (Out-File, Set-Content, Test-Path,
// Get-Date, Get-Random, [char]), so they work in Constrained Language
// mode too, where a .NET static call outside a try would end the
// script.
//
// Both processes append to the one log, each opening it only for a
// line, through Out-File: Windows PowerShell 5.1 opens it once (append,
// write only, sharing reading), so the other part's Out-File is refused
// until this one is done and GitBolt's readers never are. Not
// Add-Content: it shares writing but not reading, and when its first
// open (read-write) meets another handle it opens again write-only and
// then fails ("Stream was not readable") with that handle left open
// until a garbage collection, which refuses every later line of both
// parts. A refused line waits in $logPending and is tried again, at a
// random 10-100 ms (two cold processes' first lines come together), for
// at most $logPatience seconds; then it stays queued for the next
// Write-Log, which writes everything queued in one go, in order and
// with each line's own time. After a wait in vain $logPatience is 0
// (one try a call) until a write goes in. Write-Log '' writes only the
// queue, always waiting the full patience: each part's last steps. A
// line that can't be written is never dropped unsaid: it stays queued,
// and each wait in vain warns on stderr. -NoNewline: the queue ends
// each line with CR LF itself.
//
// The result goes through Set-Content as ASCII (every reason is plain
// English, and the version x.y.z), so there is no BOM. `onlyFirst`:
// written only if there is none yet (the helper's, which never
// replaces what the administrator part said).
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
        // script: on both sides, so the function and the script share
        // them whichever scope -EncodedCommand runs the script in.
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
    // No comments (they would lengthen the command line, three bytes
    // of base64 for each character) and no double quotes, so nothing in
    // it is ever expanded.
    QStringList lines = windowsPreambleLines(in, false);
    lines << QStringList{
        QStringLiteral("$prefix = ") + powerShellQuote(prefix),
        QStringLiteral("$app = ") + powerShellQuote(windowsApp(dir)),
        QStringLiteral("$installer = ") + powerShellQuote(windowsPath(in.staged)),
        QStringLiteral("Write-Log ('administrator part: waiting for GitBolt (pid ' + $appPid + ') to quit')"),
        QStringLiteral("$code = 0"),
        QStringLiteral("try {"),
        // By polling the process list, which shows another user's
        // processes too: an over-the-shoulder administrator may not be
        // allowed to open the user's GitBolt, so Wait-Process could fail.
        QStringLiteral("    $deadline = (Get-Date).AddSeconds($waitSeconds)"),
        QStringLiteral("    while (Get-Process -Id $appPid -ErrorAction SilentlyContinue) {"),
        QStringLiteral("        if ((Get-Date) -ge $deadline) {"),
        QStringLiteral("            $code = %1").arg(kWindowsExitGaveUp),
        QStringLiteral("            break"),
        QStringLiteral("        }"),
        QStringLiteral("        Start-Sleep -Milliseconds 250"),
        QStringLiteral("    }"),
        QStringLiteral("    if ($code -eq 0) {"),
        QStringLiteral("        $deadline = (Get-Date).AddSeconds(%1)").arg(kWindowsInUseSeconds),
        QStringLiteral("        while ($true) {"),
        // A case-insensitive prefix by string methods and -eq alone
        // (no [StringComparison], which Constrained Language refuses).
        QStringLiteral("            $inUse = @(Get-Process -Name GitBolt -ErrorAction SilentlyContinue | Where-Object {"),
        QStringLiteral("                try { $_.Id -ne $appPid -and $_.Path -and $_.Path.Length -ge $prefix.Length -and "
                       "$_.Path.Substring(0, $prefix.Length) -eq $prefix } catch { $false }"),
        QStringLiteral("            }).Count -gt 0"),
        QStringLiteral("            if (-not $inUse -and (Test-Path -LiteralPath $app)) {"),
        // In use only for a sharing or lock violation (32, 33): a check
        // that can't run (Constrained Language refuses [IO.File]) leaves
        // the process list's answer.
        QStringLiteral("                try {"),
        QStringLiteral("                    [IO.File]::Open($app, 'Open', 'ReadWrite', 'None').Dispose()"),
        QStringLiteral("                } catch {"),
        QStringLiteral("                    $e = $_.Exception"),
        QStringLiteral("                    while ($e.InnerException) {"),
        QStringLiteral("                        $e = $e.InnerException"),
        QStringLiteral("                    }"),
        QStringLiteral("                    $inUse = @(32, 33) -contains ($e.HResult -band 0xFFFF)"),
        QStringLiteral("                }"),
        QStringLiteral("            }"),
        QStringLiteral("            if (-not $inUse) {"),
        QStringLiteral("                break"),
        QStringLiteral("            }"),
        QStringLiteral("            if ((Get-Date) -ge $deadline) {"),
        QStringLiteral("                $code = %1").arg(kWindowsExitInUse),
        QStringLiteral("                break"),
        QStringLiteral("            }"),
        QStringLiteral("            Start-Sleep -Seconds 1"),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        // /D= last and unquoted: NSIS takes the rest of the command line
        // as the folder. Silent (/S), over the same folder; elevated
        // already, so no second prompt.
        QStringLiteral("    if ($code -eq 0) {"),
        QStringLiteral("        Write-Log ('running ' + $installer)"),
        QStringLiteral("        try {"),
        QStringLiteral("            $setup = Start-Process -FilePath $installer -ArgumentList ")
            + powerShellQuote(QStringLiteral("/S /D=") + dir) + QStringLiteral(" -Wait -PassThru"),
        QStringLiteral("        } catch {"),
        QStringLiteral("            $code = %1").arg(kWindowsExitNoInstaller),
        QStringLiteral("            Write-Log ('the installer could not be started: ' + $_.Exception.Message)"),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        // No exit code at all: the version below decides.
        QStringLiteral("    if ($code -eq 0) {"),
        QStringLiteral("        $setupCode = $setup.ExitCode"),
        QStringLiteral("        if ($null -ne $setupCode -and $setupCode -ne 0) {"),
        QStringLiteral("            $code = $setupCode"),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        QStringLiteral("    if ($code -eq 0) {"),
        QStringLiteral("        $found = [string](Get-Item -LiteralPath $app).VersionInfo.ProductVersion"),
        QStringLiteral("        if ($found.Trim() -ne $version) {"),
        QStringLiteral("            $code = %1").arg(kWindowsExitWrongVersion),
        QStringLiteral("        }"),
        QStringLiteral("    }"),
        QStringLiteral("} catch {"),
        QStringLiteral("    $code = %1").arg(kWindowsExitError),
        QStringLiteral("    Write-Log ('the administrator part failed: ' + $_.Exception.Message)"),
        QStringLiteral("}"),
        // What the code means, for the log and the GitBolt that starts
        // next; the result last of all: the helper starts GitBolt once
        // it is there.
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
        QStringLiteral("    $reason = 'another GitBolt kept running from the install folder'"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitNoInstaller),
        QStringLiteral("    $reason = 'the installer could not be started'"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitWrongVersion),
        QStringLiteral("    $reason = 'the installer ran, but GitBolt.exe is not version ' + $version"),
        QStringLiteral("} elseif ($code -eq %1) {").arg(kWindowsExitError),
        QStringLiteral("    $reason = 'the administrator part of the update failed'"),
        QStringLiteral("} else {"),
        QStringLiteral("    $reason = 'the installer exited with code ' + $code"),
        QStringLiteral("}"),
        QStringLiteral("if ($status -eq 'failed') {"),
        QStringLiteral("    Write-Log ('nothing was installed (code ' + $code + '): ' + $reason)"),
        QStringLiteral("}"),
        // Anything the log refused so far, before the result: the
        // helper starts GitBolt once that is there.
        QStringLiteral("Write-Log ''"),
        QStringLiteral("Write-Result $status $reason"),
        // And what Write-Result may have logged.
        QStringLiteral("Write-Log ''"),
        QStringLiteral("exit $code"),
    };
    return lines.join(u'\n');
}

QString windowsHelperScript(const HelperInputs& in) {
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
        // A little longer than the administrator part, which started
        // first: whatever it decided, GitBolt that quits in time is
        // started again.
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
        // The administrator part writes the result last. No result, and
        // that part gone a few seconds (it may still be writing), or no
        // result in installSeconds: it says so itself.
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
        // Either way: the new copy, or the old one if it didn't install.
        QStringLiteral("Write-Log ('starting ' + $app)"),
        QStringLiteral("try {"),
        QStringLiteral("    Start-Process -FilePath $app"),
        QStringLiteral("} catch {"),
        QStringLiteral("    Write-Log ('could not start GitBolt: ' + $_.Exception.Message)"),
        QStringLiteral("}"),
        // Anything the log refused so far.
        QStringLiteral("Write-Log ''"),
    };
    return lines.join(u'\n') + u'\n';
}

bool appImageRunsExtracted(const QString& appDir, bool extractAndRunSet) {
    if (extractAndRunSet)
        return true;
    if (appDir.isEmpty())
        return false;
    const QString canonical = QFileInfo(appDir).canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isDir())
        return false;
    // The FUSE runtime mounts the image at $APPDIR (/tmp/.mount_X);
    // extracted, $APPDIR is a plain folder on some other mount
    // (<tmp>/appimage_extracted_<hash>).
    const QStorageInfo volume(canonical);
    return volume.isValid() && QFileInfo(volume.rootPath()).canonicalFilePath() != canonical;
}

} // namespace gitbolt::update
