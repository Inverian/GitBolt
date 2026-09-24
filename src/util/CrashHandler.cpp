#include "util/CrashHandler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSysInfo>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <dbghelp.h>
#include <share.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace gitbolt::util {

// ---------------------------------------------------------------------------
// Signal-handler contract: SIGSEGV/SIGABRT usually fire from inside a
// corrupted heap or mid-malloc, so the handler may only use async-
// signal-safe calls — open/write/close, backtrace, backtrace_symbols_fd,
// time, raise. Anything that allocates (QString, fopen/fprintf,
// backtrace_symbols, mkpath) or takes a lock (localtime) can convert a
// crash into a silent deadlock with no report AND no core dump.
//
// Everything the handler needs is therefore pre-rendered at install()
// time, in a normal context, into the static buffers below.
// ---------------------------------------------------------------------------
namespace {

char   g_crashPath[1024] = {};
char   g_header[2048]    = {};
size_t g_headerLen       = 0;

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)

void writeRaw(int fd, const char* data, size_t len) {
    while (len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if (n <= 0)
            return;  // EINTR/disk-full — nothing safe left to do
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void writeStr(int fd, const char* s) {
    writeRaw(fd, s, std::strlen(s));
}

// Minimal decimal printer — printf is not async-signal-safe.
void writeDec(int fd, long long value) {
    char buf[24];
    char* p = buf + sizeof(buf);
    const bool neg = value < 0;
    unsigned long long v = neg
        ? ~static_cast<unsigned long long>(value) + 1ULL
        : static_cast<unsigned long long>(value);
    do {
        *--p = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    if (neg)
        *--p = '-';
    writeRaw(fd, p, static_cast<size_t>(buf + sizeof(buf) - p));
}

// Dedicated stack for the handler so a stack-overflow SIGSEGV can
// still run it (without SA_ONSTACK the faulting thread has no stack
// left to handle anything on).
char g_altStack[64 * 1024];

#endif // unix

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGFPE:  return "SIGFPE";
#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    case SIGBUS:  return "SIGBUS";
#endif
    default:      return "UNKNOWN";
    }
}

} // namespace

void CrashHandler::install() {
    // Pre-render the path (this also creates ~/.gitbolt — mkpath is
    // NOT signal-safe, so it must happen here) and the static system
    // header the handler will write verbatim.
    const QByteArray pathBytes = QFile::encodeName(crashLogPath());
    std::snprintf(g_crashPath, sizeof(g_crashPath), "%s",
                  pathBytes.constData());

    const QByteArray header =
        (QStringLiteral("=== GitBolt Crash Report ===\n")
         + QStringLiteral("App version: %1\n")
               .arg(qApp ? qApp->applicationVersion()
                         : QStringLiteral("unknown"))
         + QStringLiteral("Qt version: %1 (runtime), %2 (compile)\n")
               .arg(QString::fromLatin1(qVersion()),
                    QStringLiteral(QT_VERSION_STR))
         + QStringLiteral("OS: %1 %2\n")
               .arg(QSysInfo::productType(), QSysInfo::productVersion())
         + QStringLiteral("Kernel: %1 %2\n")
               .arg(QSysInfo::kernelType(), QSysInfo::kernelVersion())
         + QStringLiteral("CPU Arch: %1\n")
               .arg(QSysInfo::currentCpuArchitecture()))
            .toUtf8();
    g_headerLen = std::min<size_t>(static_cast<size_t>(header.size()),
                                   sizeof(g_header));
    std::memcpy(g_header, header.constData(), g_headerLen);

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    stack_t ss{};
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    // SA_ONSTACK: run on the alternate stack (stack overflows).
    // SA_RESETHAND: default disposition restored on entry, so the
    // re-raise at the end of the handler dumps core normally and a
    // crash INSIDE the handler can't recurse.
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
#else
    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGABRT, signalHandler);
    std::signal(SIGFPE,  signalHandler);
#endif
}

bool CrashHandler::hasPendingCrashReport() {
    return QFile::exists(crashLogPath());
}

QString CrashHandler::readCrashReport() {
    QFile file(crashLogPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

void CrashHandler::clearCrashReport() {
    QFile::remove(crashLogPath());
}

QString CrashHandler::crashLogPath() {
    QString dir = QDir::homePath() + QStringLiteral("/.gitbolt");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/crash.log");
}

void CrashHandler::signalHandler(int signal) {
#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    // Async-signal-safe path: open/write/close + backtrace_symbols_fd
    // only. No allocation, no locks, no stdio.
    const int fd = ::open(g_crashPath,
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        writeRaw(fd, g_header, g_headerLen);
        writeStr(fd, "Signal: ");
        writeStr(fd, signalName(signal));
        writeStr(fd, " (");
        writeDec(fd, signal);
        writeStr(fd, ")\n");
        // Raw epoch — localtime/strftime take locks. The reader can
        // format it; the crash dialog shows the report verbatim and
        // an epoch is still unambiguous.
        writeStr(fd, "Epoch: ");
        writeDec(fd, static_cast<long long>(::time(nullptr)));
        writeStr(fd, "\n\n--- Stack Trace ---\n");

        void* frames[64];
        const int frameCount = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, frameCount, fd);

        writeStr(fd, "\n=== End of Report ===\n");
        ::close(fd);
    }
    // SA_RESETHAND already restored the default disposition.
    ::raise(signal);
#else
    std::signal(signal, SIG_DFL);
    writeStackTrace(signal);
    std::raise(signal);
#endif
}

// Windows-only since the POSIX path moved into the handler itself
// (the constraints differ: no fork-style signal-safety list, and the
// dbghelp calls below have no fd-based equivalents).
void CrashHandler::writeStackTrace(int signal) {
#if defined(Q_OS_WIN)
    // Same as fopen (which the CRT implements as _fsopen with
    // _SH_DENYNO) without MSVC's C4996; fopen_s would open unshared.
    FILE* fp = _fsopen(g_crashPath, "w", _SH_DENYNO);
    if (!fp) return;

    std::fwrite(g_header, 1, g_headerLen, fp);
    std::fprintf(fp, "Signal: %s (%d)\n", signalName(signal), signal);
    std::fprintf(fp, "Epoch: %lld\n",
                 static_cast<long long>(std::time(nullptr)));
    std::fprintf(fp, "\n--- Stack Trace ---\n");

    static constexpr int MAX_FRAMES = 64;
    void* frames[MAX_FRAMES];
    USHORT frameCount = CaptureStackBackTrace(0, MAX_FRAMES, frames, nullptr);

    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    char symbolBuffer[sizeof(SYMBOL_INFO) + 256 * sizeof(char)];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (USHORT i = 0; i < frameCount; ++i) {
        DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
        if (SymFromAddr(process, address, nullptr, symbol)) {
            std::fprintf(fp, "  [%d] %s - 0x%0llX\n",
                         i, symbol->Name, symbol->Address);
        } else {
            std::fprintf(fp, "  [%d] 0x%0llX\n", i, address);
        }
    }

    SymCleanup(process);
    std::fprintf(fp, "\n=== End of Report ===\n");
    std::fclose(fp);
#else
    Q_UNUSED(signal);
#endif
}

} // namespace gitbolt::util
