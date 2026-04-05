#include "util/CrashHandler.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

#include <csignal>
#include <cstdio>
#include <cstdlib>

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
#include <execinfo.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace gitbolt::util {

void CrashHandler::install() {
    std::signal(SIGSEGV, signalHandler);
    std::signal(SIGABRT, signalHandler);
#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    std::signal(SIGBUS, signalHandler);
#endif
    std::signal(SIGFPE, signalHandler);
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
    // Re-install default handler so that if writeStackTrace itself crashes,
    // the OS produces a core dump as usual.
    std::signal(signal, SIG_DFL);

    writeStackTrace(signal);

    // Re-raise to get the default behavior (core dump / termination)
    std::raise(signal);
}

void CrashHandler::writeStackTrace(int signal) {
    const char* signalName = "UNKNOWN";
    switch (signal) {
    case SIGSEGV: signalName = "SIGSEGV"; break;
    case SIGABRT: signalName = "SIGABRT"; break;
    case SIGFPE:  signalName = "SIGFPE";  break;
#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    case SIGBUS:  signalName = "SIGBUS";  break;
#endif
    }

    // Open crash log using low-level I/O (signal-safe on most platforms)
    QString path = crashLogPath();
    QByteArray pathBytes = path.toUtf8();
    FILE* fp = std::fopen(pathBytes.constData(), "w");
    if (!fp) return;

    // Header
    std::fprintf(fp, "=== GitBolt Crash Report ===\n");
    std::fprintf(fp, "Signal: %s (%d)\n", signalName, signal);

    // Timestamp — use a simple approach (time_t is async-signal-safe on most systems)
    std::time_t now = std::time(nullptr);
    char timeBuf[64] = {};
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(fp, "Time: %s\n", timeBuf);

    // Application info
    std::fprintf(fp, "App version: %s\n",
                 qApp ? qPrintable(qApp->applicationVersion()) : "unknown");
    std::fprintf(fp, "Qt version: %s (runtime), %s (compile)\n",
                 qVersion(), QT_VERSION_STR);
    std::fprintf(fp, "OS: %s %s\n",
                 qPrintable(QSysInfo::productType()),
                 qPrintable(QSysInfo::productVersion()));
    std::fprintf(fp, "Kernel: %s %s\n",
                 qPrintable(QSysInfo::kernelType()),
                 qPrintable(QSysInfo::kernelVersion()));
    std::fprintf(fp, "CPU Arch: %s\n",
                 qPrintable(QSysInfo::currentCpuArchitecture()));

    // Stack trace
    std::fprintf(fp, "\n--- Stack Trace ---\n");

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    static constexpr int MAX_FRAMES = 64;
    void* frames[MAX_FRAMES];
    int frameCount = backtrace(frames, MAX_FRAMES);

    // backtrace_symbols_fd writes directly to a file descriptor (signal-safe)
    char** symbols = backtrace_symbols(frames, frameCount);
    if (symbols) {
        for (int i = 0; i < frameCount; ++i) {
            std::fprintf(fp, "  [%d] %s\n", i, symbols[i]);
        }
        std::free(symbols);
    } else {
        std::fprintf(fp, "  (unable to resolve symbols)\n");
        // Fall back to fd-based output
        std::fflush(fp);
        backtrace_symbols_fd(frames, frameCount, fileno(fp));
    }

#elif defined(Q_OS_WIN)
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
#else
    std::fprintf(fp, "  (stack trace not available on this platform)\n");
#endif

    std::fprintf(fp, "\n=== End of Report ===\n");
    std::fclose(fp);
}

} // namespace gitbolt::util
