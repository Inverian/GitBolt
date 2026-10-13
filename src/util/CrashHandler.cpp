#include "util/CrashHandler.h"

#include <QDir>
#include <QFile>
#include <QSysInfo>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <pthread.h>
#include <sys/ucontext.h>
#endif
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
// signal-safe calls — open/write/close, backtrace, dladdr, time,
// raise. (dladdr is not on POSIX's list, but backtrace_symbols_fd,
// which the handler called before, is built on it: no new hazard.)
// Anything that allocates (QString, fopen/fprintf, backtrace_symbols,
// mkpath) or takes a lock (localtime) can convert a crash into a
// silent deadlock with no report AND no core dump.
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

// Lower-case hex without a prefix, for the same reason.
void writeHex(int fd, unsigned long long value) {
    char buf[16];
    char* p = buf + sizeof(buf);
    do {
        *--p = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value != 0);
    writeRaw(fd, p, static_cast<size_t>(buf + sizeof(buf) - p));
}

// An image path's file name only: Linux paths name the user's home
// (/home/<user>/…, an AppImage mount), and the parser keys on the
// name. Blanks become '_' so the frame line keeps its fields. Returns
// the length copied into `out` (not terminated).
size_t copyModuleName(const char* path, char* out, size_t size) {
    const char* slash = std::strrchr(path, '/');
    const char* name = slash ? slash + 1 : path;
    size_t n = 0;
    for (; n < size && name[n] != '\0'; ++n)
        out[n] = (name[n] == ' ' || name[n] == '\t') ? '_' : name[n];
    return n;
}

void writeModuleName(int fd, const char* path) {
    char buf[256];
    const size_t n = copyModuleName(path, buf, sizeof(buf));
    if (n == 0)
        writeStr(fd, "?");
    else
        writeRaw(fd, buf, n);
}

// The main executable: where it is mapped, and the file name its
// frames are written under (install() explains why it isn't dladdr's).
const void* g_selfBase       = nullptr;
char        g_selfName[256]  = {};
size_t      g_selfNameLen    = 0;

// One "#<n> <module> +0x<offset> <symbol>+0x<offset>" line per frame
// (format in CrashHandler.h).
void writeFrame(int fd, int index, void* frame) {
    const auto pc = reinterpret_cast<std::uintptr_t>(frame);
    writeStr(fd, "#");
    writeDec(fd, index);
    writeStr(fd, " ");
    Dl_info info{};
    if (::dladdr(frame, &info) == 0 || !info.dli_fname || !info.dli_fbase) {
        writeStr(fd, "? 0x");
        writeHex(fd, pc);
        writeStr(fd, "\n");
        return;
    }
    if (g_selfNameLen > 0 && info.dli_fbase == g_selfBase)
        writeRaw(fd, g_selfName, g_selfNameLen);
    else
        writeModuleName(fd, info.dli_fname);
    writeStr(fd, " +0x");
    writeHex(fd, pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase));
    writeStr(fd, " ");
    if (info.dli_sname && info.dli_saddr) {
        writeStr(fd, info.dli_sname);
        writeStr(fd, "+0x");
        writeHex(fd, pc - reinterpret_cast<std::uintptr_t>(info.dli_saddr));
    } else {
        writeStr(fd, "?");
    }
    writeStr(fd, "\n");
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

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)

// The PC the signal interrupted, from the context SA_SIGINFO hands the
// handler; 0 where it isn't read. Darwin's backtrace() walks frame
// pointers, and _sigtramp's frame links to the interrupted function's
// caller: the function that faulted (or __pthread_kill, for raise())
// is never a frame, only the context has it. glibc unwinds through the
// signal frame by its CFI, so Linux has that frame already. A plain
// read of the context struct: async-signal-safe.
std::uintptr_t interruptedPc(const void* context) {
#if defined(Q_OS_MACOS) && (defined(__arm64__) || defined(__aarch64__) || defined(__x86_64__))
    const auto* uc = static_cast<const ucontext_t*>(context);
    if (!uc || !uc->uc_mcontext)
        return 0;
#if defined(__x86_64__)
    return static_cast<std::uintptr_t>(uc->uc_mcontext->__ss.__rip);
#else
    return static_cast<std::uintptr_t>(
        __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
#endif
#else
    Q_UNUSED(context);
    return 0;
#endif
}

// The handler, installed as sa_sigaction itself: a wrapper around it
// would be a frame of its own, and the parser drops #0 and #1 by
// position.
void onSignal(int signal, siginfo_t*, void* context) {
    // Async-signal-safe path: open/write/close + backtrace + dladdr
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

        // Captured here, in the handler, so frame #0 is the handler
        // and #1 the signal trampoline; the parser drops those two.
        // Then the interrupted PC where backtrace() lacks it (macOS),
        // and backtrace()'s frames from #2 on, numbered from there.
        void* frames[64];
        const int frameCount = ::backtrace(frames, 64);
        const std::uintptr_t pc = interruptedPc(context);
        int index = 0;
        for (int i = 0; i < frameCount; ++i) {
            writeFrame(fd, index++, frames[i]);
            if (i == 1 && pc != 0
                && (frameCount == 2 || reinterpret_cast<std::uintptr_t>(frames[2]) != pc))
                writeFrame(fd, index++, reinterpret_cast<void*>(pc));
        }

        writeStr(fd, "\n=== End of Report ===\n");
        ::close(fd);
    }
    // SA_RESETHAND already restored the default disposition.
    ::raise(signal);
}

#endif // unix

} // namespace

QByteArray CrashHandler::renderHeader(const char* appVersion,
                                      bool officialBuild, bool envOptOut,
                                      bool testSession) {
    // Format 2 adds the version and the three lines CrashLogParser
    // needs to refuse a report (format 1 always said "App version:
    // unknown": qApp did not exist yet when install() ran). One
    // ~/.gitbolt/crash.log is shared by every build and session under
    // a HOME (test sessions write crash-test.log now; older builds
    // didn't), so the report has to say where it came from.
    return (QStringLiteral("=== GitBolt Crash Report ===\n")
            + QStringLiteral("Format: 2\n")
            + QStringLiteral("App version: %1\n")
                  .arg(appVersion && *appVersion
                           ? QString::fromUtf8(appVersion)
                           : QStringLiteral("unknown"))
            + QStringLiteral("Build: %1\n")
                  .arg(officialBuild ? QStringLiteral("official")
                                     : QStringLiteral("unofficial"))
            + QStringLiteral("Opt-out: %1\n")
                  .arg(envOptOut ? QStringLiteral("env")
                                 : QStringLiteral("none"))
            + QStringLiteral("Session: %1\n")
                  .arg(testSession ? QStringLiteral("test")
                                   : QStringLiteral("user"))
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
}

#if defined(Q_OS_MACOS)
namespace {
// Keeps macOS's crash reporter out of a test session's crash. With a
// task-level port of its own for EXC_CRASH (and corpse notifications),
// the kernel hands those to this dying task instead of ReportCrash: a
// test that crashes GitBolt on purpose (offscreen under the bridge, in
// CI) put the "GitBolt quit unexpectedly" box on the user's screen and
// left a GitBolt-*.ips in ~/Library/Logs/DiagnosticReports, though the
// handler had already written crash-test.log. The signal handler still
// runs first, and the process still dies of its signal. A user's
// session keeps the system's crash reporting.
void keepCrashReporterOut() {
    mach_port_t port = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS)
        return;
    if (mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND)
        != KERN_SUCCESS) {
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
        return;
    }
    if (task_set_exception_ports(mach_task_self(), EXC_MASK_CRASH | EXC_MASK_CORPSE_NOTIFY,
                                 port, EXCEPTION_DEFAULT, THREAD_STATE_NONE)
        != KERN_SUCCESS)
        return;
    // Not for the children. Task-level ports are inherited across fork
    // and exec, and nothing ever reads this one (it is there so that the
    // kernel hands GitBolt's own death to it, not to ReportCrash): a
    // git, a hook, ssh or a terminal's program that died of a signal
    // sent its EXC_CRASH there and waited for a reply, unkillable, until
    // GitBolt quit, and a commit waited out its five minutes. Each child
    // starts with the system's handling: QProcess and forkpty fork, and
    // the one posix_spawn (the login shell's PATH) sets its own
    // (util/LoginPath.cpp). GitBolt's own askpass child, a test
    // session's too, sets this one again.
    pthread_atfork(nullptr, nullptr, [] {
        task_set_exception_ports(mach_task_self(), EXC_MASK_CRASH | EXC_MASK_CORPSE_NOTIFY,
                                 MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    });
}
} // namespace
#endif

void CrashHandler::install(const char* appVersion, bool officialBuild,
                           bool envOptOut, bool testSession) {
#if defined(Q_OS_MACOS)
    if (testSession)
        keepCrashReporterOut();
#endif
    // Pre-render the path (this also creates ~/.gitbolt — mkpath is
    // NOT signal-safe, so it must happen here) and the static system
    // header the handler will write verbatim. A test session writes a
    // file of its own: e2e and bridge runs keep the real HOME, and the
    // handler truncates what it writes, so a report of the user's,
    // still waiting for their next launch, would be lost to a test's.
    const QByteArray pathBytes =
        QFile::encodeName(testSession ? testCrashLogPath() : crashLogPath());
    std::snprintf(g_crashPath, sizeof(g_crashPath), "%s",
                  pathBytes.constData());

    const QByteArray header =
        renderHeader(appVersion, officialBuild, envOptOut, testSession);
    g_headerLen = std::min<size_t>(static_cast<size_t>(header.size()),
                                   sizeof(g_header));
    std::memcpy(g_header, header.constData(), g_headerLen);

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    // glibc loads libgcc_s, which backtrace() unwinds with, on the
    // first call, and loading allocates. Make that first call here.
    void* warmUp[1];
    ::backtrace(warmUp, 1);

    // GitBolt's own frames go under the real file's name. glibc's
    // dladdr names the main program by argv[0] instead (elf/dl-addr.c):
    // in an AppImage that is the .AppImage or AppRun, through a symlink
    // the link's name, and the parser would count none of those frames
    // as GitBolt's. Linux's /proc/self/exe is the file itself; where it
    // doesn't exist (macOS), dladdr's path with symlinks resolved.
    // g_crashPath is in the main executable: gitbolt_util is static.
    Dl_info self{};
    if (::dladdr(g_crashPath, &self) != 0 && self.dli_fbase) {
        char exe[4096];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        bool found = false;
        if (n > 0) {
            exe[n] = '\0';
            // Replaced while running (a package upgrade): "<path> (deleted)".
            static constexpr char deleted[] = " (deleted)";
            const size_t length = static_cast<size_t>(n);
            const size_t tail = sizeof(deleted) - 1;
            if (length > tail && std::strcmp(exe + length - tail, deleted) == 0)
                exe[length - tail] = '\0';
            found = true;
        } else if (self.dli_fname && ::realpath(self.dli_fname, exe)) {
            found = true;
        }
        if (found) {
            g_selfNameLen = copyModuleName(exe, g_selfName, sizeof(g_selfName));
            g_selfBase = self.dli_fbase;
        }
    }

    stack_t ss{};
    ss.ss_sp = g_altStack;
    ss.ss_size = sizeof(g_altStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    // SA_SIGINFO: the handler gets the interrupted context, whose PC
    // macOS's backtrace() never reports (interruptedPc).
    // SA_ONSTACK: run on the alternate stack (stack overflows).
    // SA_RESETHAND: default disposition restored on entry, so the
    // re-raise at the end of the handler dumps core normally and a
    // crash INSIDE the handler can't recurse.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
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

QString CrashHandler::testCrashLogPath() {
    QString dir = QDir::homePath() + QStringLiteral("/.gitbolt");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/crash-test.log");
}

#if !defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
void CrashHandler::signalHandler(int signal) {
    std::signal(signal, SIG_DFL);
    writeStackTrace(signal);
    std::raise(signal);
}
#endif

// Windows-only since the POSIX path moved into its handler, onSignal
// (the constraints differ: no fork-style signal-safety list, and the
// dbghelp calls below have no fd-based equivalents).
Q_NEVER_INLINE void CrashHandler::writeStackTrace(int signal) {
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
    // Skip this function, so frame #0 is signalHandler as on POSIX
    // (Q_NEVER_INLINE below keeps it a frame of its own).
    const USHORT frameCount =
        CaptureStackBackTrace(1, MAX_FRAMES, frames, nullptr);

    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);

    char symbolBuffer[sizeof(SYMBOL_INFO) + 256 * sizeof(char)];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    // Same frame lines as POSIX (format in CrashHandler.h). The module
    // and its base come from the loader, so a frame keeps an offset
    // that survives ASLR even where SymFromAddr knows nothing (no PDB
    // ships, and GitBolt.exe exports nothing).
    for (USHORT i = 0; i < frameCount; ++i) {
        const DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
        HMODULE module = nullptr;
        char modulePath[MAX_PATH] = {};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(frames[i]), &module)
            || GetModuleFileNameA(module, modulePath, MAX_PATH) == 0) {
            std::fprintf(fp, "#%u ? 0x%llx\n", static_cast<unsigned>(i),
                         static_cast<unsigned long long>(address));
            continue;
        }
        // The file name only, blanks made '_' (see CrashHandler.h).
        char* name = modulePath;
        for (char* c = modulePath; *c != '\0'; ++c) {
            if (*c == '\\' || *c == '/')
                name = c + 1;
            else if (*c == ' ' || *c == '\t')
                *c = '_';
        }
        std::fprintf(fp, "#%u %s +0x%llx ", static_cast<unsigned>(i),
                     *name != '\0' ? name : "?",
                     static_cast<unsigned long long>(
                         address - reinterpret_cast<DWORD64>(module)));
        DWORD64 displacement = 0;
        if (SymFromAddr(process, address, &displacement, symbol))
            std::fprintf(fp, "%s+0x%llx\n", symbol->Name,
                         static_cast<unsigned long long>(displacement));
        else
            std::fprintf(fp, "?\n");
    }

    SymCleanup(process);
    std::fprintf(fp, "\n=== End of Report ===\n");
    std::fclose(fp);
#else
    Q_UNUSED(signal);
#endif
}

} // namespace gitbolt::util
