#include "update/Processes.h"

#include <QFile>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot, after <windows.h>

#include <vector>
#elif defined(Q_OS_MACOS)
#include <libproc.h>
#include <sys/proc.h>        // SZOMB
#include <sys/proc_info.h>
#include <unistd.h>
#elif defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gitbolt::update {

namespace {
constexpr char kWindowsProgram[] = "GitBolt.exe";
}

QList<RunningProgram> runningGitBolts() {
    QList<RunningProgram> found;
#if defined(Q_OS_WIN)
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return found;
    PROCESSENTRY32W entry = {};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    std::vector<wchar_t> path(32768);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        const QString name = QString::fromWCharArray(entry.szExeFile);
        if (name.compare(QLatin1String(kWindowsProgram), Qt::CaseInsensitive) != 0)
            continue;
        RunningProgram program{static_cast<qint64>(entry.th32ProcessID), name, {}};
        // Access denied: another user's, path unknown. Anything else: it
        // ended since the snapshot.
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
            if (GetLastError() == ERROR_ACCESS_DENIED)
                found.append(program);
            continue;
        }
        DWORD code = 0;
        const bool running = !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
        DWORD size = static_cast<DWORD>(path.size());
        if (running && QueryFullProcessImageNameW(process, 0, path.data(), &size))
            program.path = QString::fromWCharArray(path.data(), static_cast<qsizetype>(size));
        CloseHandle(process);
        if (running)
            found.append(program);
    }
    CloseHandle(snapshot);
#endif
    return found;
}

bool isInsideWindowsFolder(const QString& path, const QString& folder) {
    if (path.isEmpty() || folder.isEmpty())
        return false;
    QString prefix = folder;
    prefix.replace(u'/', u'\\');
    if (!prefix.endsWith(u'\\'))
        prefix += u'\\';
    QString file = path;
    file.replace(u'/', u'\\');
    return file.size() > prefix.size() && file.startsWith(prefix, Qt::CaseInsensitive);
}

FolderUsers gitBoltsUsing(const QList<RunningProgram>& programs, const QString& folder, qint64 self) {
    FolderUsers users;
    for (const RunningProgram& program : programs) {
        if (program.pid == self || program.name.compare(QLatin1String(kWindowsProgram), Qt::CaseInsensitive) != 0)
            continue;
        if (program.path.isEmpty())
            ++users.unknown;
        else if (isInsideWindowsFolder(program.path, folder))
            ++users.running;
    }
    return users;
}

qint64 linuxStartTime(const QByteArray& line, qint64 bootTime, qint64 ticksPerSecond) {
    // "<pid> (<name>) <state> …": the name may hold spaces and
    // parentheses, so the fields count from the last ')'. The state is
    // field 3, the start (ticks after boot) field 22.
    const qsizetype close = line.lastIndexOf(')');
    if (close < 0 || bootTime <= 0 || ticksPerSecond <= 0)
        return 0;
    const QList<QByteArray> fields = line.mid(close + 1).simplified().split(' ');
    if (fields.size() < 20 || fields.at(0) == "Z" || fields.at(0) == "X" || fields.at(0) == "x")
        return 0;
    bool ok = false;
    const qint64 ticks = fields.at(19).toLongLong(&ok);
    return ok && ticks >= 0 ? bootTime + ticks / ticksPerSecond : 0;
}

qint64 processStartTime(qint64 pid) {
    if (pid <= 0)
        return 0;
#if defined(Q_OS_WIN)
    if (pid > qint64(MAXDWORD))
        return 0;
    // Another user's can't be opened.
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return 0;
    qint64 started = 0;
    DWORD code = 0;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE
        && GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER since1601;   // in 100 ns
        since1601.LowPart = created.dwLowDateTime;
        since1601.HighPart = created.dwHighDateTime;
        started = static_cast<qint64>((since1601.QuadPart - 116444736000000000ULL) / 10000000ULL);
    }
    CloseHandle(process);
    return started;
#elif defined(Q_OS_MACOS)
    struct proc_bsdinfo info = {};
    if (proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, PROC_PIDTBSDINFO_SIZE)
        != PROC_PIDTBSDINFO_SIZE)
        return 0;
    if (info.pbi_status == SZOMB || info.pbi_uid != ::getuid())
        return 0;
    return static_cast<qint64>(info.pbi_start_tvsec);
#elif defined(Q_OS_UNIX)
    const QByteArray dir = "/proc/" + QByteArray::number(pid);
    struct stat st {};
    if (::stat(dir.constData(), &st) != 0 || st.st_uid != ::getuid())
        return 0;
    QFile statFile(QString::fromLatin1(dir + "/stat"));
    QFile boot(QStringLiteral("/proc/stat"));
    if (!statFile.open(QIODevice::ReadOnly) || !boot.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray line = statFile.readAll();
    qint64 bootTime = 0;
    for (const QByteArray& entry : boot.readAll().split('\n')) {
        if (entry.startsWith("btime "))
            bootTime = entry.mid(6).trimmed().toLongLong();
    }
    return linuxStartTime(line, bootTime, ::sysconf(_SC_CLK_TCK));
#else
    return 0;
#endif
}

} // namespace gitbolt::update
