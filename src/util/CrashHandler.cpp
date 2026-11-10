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
#include <iterator>

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <crt_externs.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/ucontext.h>
#elif defined(Q_OS_LINUX)
#include <elf.h>
#include <link.h>
#endif
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
// silent deadlock with no report AND no core dump. Windows' handlers
// keep to the same rule: CreateFileW/WriteFile, stack buffers, and
// the mapped images' own headers.
//
// Everything the handler needs is therefore pre-rendered at install()
// time, in a normal context, into the static buffers below.
// ---------------------------------------------------------------------------
namespace {

char   g_header[2048] = {};
size_t g_headerLen    = 0;

constexpr int kMaxFrames = 64;

#if defined(Q_OS_WIN)
using ReportFile = HANDLE;
// Wide: the profile folder can have letters the ANSI code page lacks.
wchar_t g_crashPathW[1024] = {};
#else
using ReportFile = int;
char g_crashPath[1024] = {};
#endif

void writeRaw(ReportFile file, const char* data, size_t len) {
    while (len > 0) {
#if defined(Q_OS_WIN)
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 1u << 20));
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0)
            return;
        const size_t n = written;
#else
        const ssize_t written = ::write(file, data, len);
        if (written <= 0)
            return;  // EINTR/disk-full — nothing safe left to do
        const size_t n = static_cast<size_t>(written);
#endif
        data += n;
        len -= n;
    }
}

void writeStr(ReportFile file, const char* s) {
    writeRaw(file, s, std::strlen(s));
}

// Minimal decimal printer — printf is not async-signal-safe.
void writeDec(ReportFile file, long long value) {
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
    writeRaw(file, p, static_cast<size_t>(buf + sizeof(buf) - p));
}

// Lower-case hex without a prefix, at least `digits` long, into `out`
// (16 bytes are always enough); returns the length.
size_t putHex(char* out, unsigned long long value, int digits = 1) {
    char buf[16];
    char* p = buf + sizeof(buf);
    do {
        *--p = "0123456789abcdef"[value & 0xf];
        value >>= 4;
        --digits;
    } while ((value != 0 || digits > 0) && p > buf);
    const size_t n = static_cast<size_t>(buf + sizeof(buf) - p);
    std::memcpy(out, p, n);
    return n;
}

void writeHex(ReportFile file, unsigned long long value, int digits = 1) {
    char buf[16];
    writeRaw(file, buf, putHex(buf, value, digits));
}

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

// "Signal: <name> (<n>)\n"
void writeSignal(ReportFile file, int signal) {
    writeStr(file, "Signal: ");
    writeStr(file, signalName(signal));
    writeStr(file, " (");
    writeDec(file, signal);
    writeStr(file, ")\n");
}

#if defined(Q_OS_UNIX) || defined(Q_OS_MACOS)

// An image path's file name only: Linux paths name the user's home
// (/home/<user>/…), and the parser keys on the name. Blanks become
// '_' so the frame line keeps its fields. Returns the length copied
// into `out` (not terminated).
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
        writeSignal(fd, signal);
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
        void* frames[kMaxFrames];
        const int frameCount = ::backtrace(frames, kMaxFrames);
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

#if defined(Q_OS_WIN)

// The executable's base, listed first under "--- Modules ---".
const void* g_exeBase = nullptr;

// A mapped image's NT headers, or null. The loader maps an image's
// first page whole, which the bounds keep to.
const IMAGE_NT_HEADERS* ntHeaders(const unsigned char* base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE
        || dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER))
        || dos->e_lfanew > static_cast<LONG>(0x1000 - sizeof(IMAGE_NT_HEADERS)))
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE
        || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
        return nullptr;
    return nt;
}

// An image's CodeView RSDS record: the symbol server's key (GUID, then
// age, lower-case hex) and the PDB's file name (`pdb`, not terminated).
// Read from the mapped image, bounds-checked against SizeOfImage, with
// no allocation and no lock: the Build ID header and the crash-time
// module lines both use it.
struct CodeView {
    char id[48] = {};
    size_t idLen = 0;
    const char* pdb = nullptr;
    size_t pdbLen = 0;
};

bool readCodeView(const unsigned char* base, CodeView& out) {
    const IMAGE_NT_HEADERS* nt = ntHeaders(base);
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
        return false;
    const DWORD size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress == 0 || dir.VirtualAddress >= size
        || dir.Size > size - dir.VirtualAddress)
        return false;
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
    for (DWORD i = 0; i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        const IMAGE_DEBUG_DIRECTORY& entry = entries[i];
        const DWORD rva = entry.AddressOfRawData;
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || rva == 0 || rva >= size
            || entry.SizeOfData < 24 || entry.SizeOfData > size - rva)
            continue;
        const unsigned char* record = base + rva;
        if (std::memcmp(record, "RSDS", 4) != 0)
            continue;
        GUID guid;
        DWORD age = 0;
        std::memcpy(&guid, record + 4, sizeof(guid));
        std::memcpy(&age, record + 20, sizeof(age));
        char* p = out.id;
        p += putHex(p, guid.Data1, 8);
        p += putHex(p, guid.Data2, 4);
        p += putHex(p, guid.Data3, 4);
        for (const unsigned char byte : guid.Data4)
            p += putHex(p, byte, 2);
        p += putHex(p, age);
        out.idLen = static_cast<size_t>(p - out.id);
        // The PDB's path as linked (the build machine's): its name only.
        const char* path = reinterpret_cast<const char*>(record + 24);
        const size_t max = entry.SizeOfData - 24;
        out.pdb = path;
        size_t n = 0;
        for (; n < max && path[n] != '\0'; ++n) {
            if (path[n] == '\\' || path[n] == '/')
                out.pdb = path + n + 1;
        }
        out.pdbLen = static_cast<size_t>(path + n - out.pdb);
        return true;
    }
    return false;
}

// A module's file name in UTF-8, blanks made '_'; 0 when unknown.
size_t moduleName(const void* base, char* out, size_t size) {
    wchar_t path[512];
    const DWORD n = GetModuleFileNameW(static_cast<HMODULE>(const_cast<void*>(base)), path, 512);
    if (n == 0)
        return 0;
    const wchar_t* name = path;
    for (DWORD i = 0; i < n; ++i) {
        if (path[i] == L'\\' || path[i] == L'/')
            name = path + i + 1;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(path + n - name),
                                          out, static_cast<int>(size), nullptr, nullptr);
    if (bytes <= 0)
        return 0;
    for (int i = 0; i < bytes; ++i) {
        if (out[i] == ' ' || out[i] == '\t')
            out[i] = '_';
    }
    return static_cast<size_t>(bytes);
}

// The modules the frames ran in, for the "--- Modules ---" list.
struct SeenModules {
    const void* base[32] = {};
    int count = 0;
    void add(const void* image) {
        for (int i = 0; i < count; ++i) {
            if (base[i] == image)
                return;
        }
        if (image && count < 32)
            base[count++] = image;
    }
};

// One frame line (format in CrashHandler.h), with no symbol: without
// PDBs, a name could only be the nearest export, and wrong.
void writeFrame(HANDLE file, int index, DWORD64 pc, SeenModules& seen) {
    writeStr(file, "#");
    writeDec(file, index);
    writeStr(file, " ");
    PVOID base = nullptr;
    char name[256];
    size_t length = 0;
    if (pc != 0 && RtlPcToFileHeader(reinterpret_cast<PVOID>(pc), &base) && base)
        length = moduleName(base, name, sizeof(name));
    if (length == 0) {
        writeStr(file, "? 0x");
        writeHex(file, pc);
        writeStr(file, "\n");
        return;
    }
    writeRaw(file, name, length);
    writeStr(file, " +0x");
    writeHex(file, pc - reinterpret_cast<DWORD64>(base));
    writeStr(file, " ?\n");
    seen.add(base);
}

// "<name> base=0x… size=0x… time=0x…[ pdb=<file> id=<hex>]": what
// matches a frame's module to its symbols. Never starts with '#'.
void writeModule(HANDLE file, const void* base) {
    const IMAGE_NT_HEADERS* nt = ntHeaders(static_cast<const unsigned char*>(base));
    char name[256];
    const size_t length = nt ? moduleName(base, name, sizeof(name)) : 0;
    if (length == 0)
        return;
    writeRaw(file, name, length);
    writeStr(file, " base=0x");
    writeHex(file, reinterpret_cast<std::uintptr_t>(base));
    writeStr(file, " size=0x");
    writeHex(file, nt->OptionalHeader.SizeOfImage);
    writeStr(file, " time=0x");
    writeHex(file, nt->FileHeader.TimeDateStamp, 8);
    CodeView cv;
    if (readCodeView(static_cast<const unsigned char*>(base), cv)) {
        writeStr(file, " pdb=");
        for (size_t i = 0; i < cv.pdbLen; ++i)
            writeRaw(file, cv.pdb[i] == ' ' || cv.pdb[i] == '\t' ? "_" : cv.pdb + i, 1);
        if (cv.pdbLen == 0)
            writeStr(file, "?");
        writeStr(file, " id=");
        writeRaw(file, cv.id, cv.idLen);
    }
    writeStr(file, "\n");
}

#if defined(_M_X64)
// The faulting thread's stack from the exception context: #0 the
// faulting instruction, then return addresses. A function without
// unwind data is a leaf (or a call through a null pointer): its return
// address is at Rsp. Reads only inside this thread's stack.
void writeContextFrames(HANDLE file, const CONTEXT* context, SeenModules& seen) {
    CONTEXT c = *context;
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    for (int n = 0; n < kMaxFrames; ++n) {
        const DWORD64 pc = c.Rip;
        if (pc == 0 && n > 0)
            break;
        writeFrame(file, n, pc, seen);
        const DWORD64 sp = c.Rsp;
        if (sp < low || sp >= high - 8)
            break;
        DWORD64 imageBase = 0;
        if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, nullptr)) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &c, &handlerData,
                             &establisher, nullptr);
        } else {
            c.Rip = *reinterpret_cast<const DWORD64*>(sp);
            c.Rsp = sp + 8;
        }
        if (c.Rsp <= sp)
            break;
    }
}
#endif

// One report per process, by the first thread that crashes. The same
// thread crashing again (inside the writer) gets false at once; another
// thread waits for the first to end the process.
volatile LONG g_writer = 0;

bool claimWriter() {
    const auto self = static_cast<LONG>(GetCurrentThreadId());
    const LONG owner = InterlockedCompareExchange(&g_writer, self, 0);
    if (owner == 0)
        return true;
    if (owner != self)
        Sleep(10000);
    return false;
}

// `code`: the exception's, or 0 (abort). `context`: the faulting
// thread's state (the filter), or null: the stack is then captured
// here, and #0 is the handler that called this.
Q_NEVER_INLINE void writeReport(int signal, DWORD code, const CONTEXT* context) {
    if (!claimWriter() || g_crashPathW[0] == L'\0')
        return;
    const HANDLE file = CreateFileW(g_crashPathW, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    writeRaw(file, g_header, g_headerLen);
    writeSignal(file, signal);
    if (code != 0) {
        writeStr(file, "Exception: 0x");
        writeHex(file, code, 8);
        writeStr(file, "\n");
    }
    bool walked = false;
#if defined(_M_X64)
    if (context)
        writeStr(file, "Stack: exception\n");
#endif
    // Raw epoch, as on POSIX: no CRT time functions.
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long ticks =
        (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    writeStr(file, "Epoch: ");
    writeDec(file, static_cast<long long>((ticks - 116444736000000000ULL) / 10000000ULL));
    writeStr(file, "\n\n--- Stack Trace ---\n");

    SeenModules seen;
    seen.add(g_exeBase);
#if defined(_M_X64)
    if (context) {
        writeContextFrames(file, context, seen);
        walked = true;
    }
#else
    Q_UNUSED(context);
#endif
    if (!walked) {
        void* frames[kMaxFrames];
        const USHORT count = RtlCaptureStackBackTrace(1, kMaxFrames, frames, nullptr);
        for (USHORT i = 0; i < count; ++i)
            writeFrame(file, i, reinterpret_cast<DWORD64>(frames[i]), seen);
    }

    writeStr(file, "\n--- Modules ---\n");
    for (int i = 0; i < seen.count; ++i)
        writeModule(file, seen.base[i]);
    writeStr(file, "\n=== End of Report ===\n");
    CloseHandle(file);
}

int signalFor(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_GUARD_PAGE:
        return SIGSEGV;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW:
        return SIGFPE;
    case 0xE06D7363:   // an MSVC C++ throw nothing caught
        return SIGABRT;
    default:
        return 0;   // "UNKNOWN (0)"
    }
}

// Every thread's unhandled exception (SetUnhandledExceptionFilter is
// process-wide). Ends the process with the exception's code: QProcess
// still sees a crash, and neither Windows Error Reporting nor a second
// report follows.
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    writeReport(signalFor(code), code, info ? info->ContextRecord : nullptr);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), std::terminate() and pure calls. Exit code 3, as the CRT's
// own SIGABRT; TerminateProcess runs no DLL detach code, which a lock
// the crashed thread held could hang.
void __cdecl onAbort(int) {
    writeReport(SIGABRT, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

// The CRT's default __fastfail()s past every other handler.
void __cdecl onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                uintptr_t) {
    constexpr DWORD kInvalidCrtParameter = 0xC0000417;   // STATUS_INVALID_CRUNTIME_PARAMETER
    writeReport(SIGABRT, kInvalidCrtParameter, nullptr);
    TerminateProcess(GetCurrentProcess(), kInvalidCrtParameter);
}

#endif // Q_OS_WIN

#if defined(Q_OS_LINUX)
// dl_iterate_phdr's first object is the main program: its
// NT_GNU_BUILD_ID note ("GNU"), from the PT_NOTE segments.
int readGnuBuildId(dl_phdr_info* info, size_t, void* data) {
    auto* out = static_cast<QByteArray*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& segment = info->dlpi_phdr[i];
        if (segment.p_type != PT_NOTE)
            continue;
        const size_t align = segment.p_align == 8 ? 8 : 4;
        const auto pad = [align](size_t n) { return (n + align - 1) & ~(align - 1); };
        const char* p = reinterpret_cast<const char*>(info->dlpi_addr + segment.p_vaddr);
        const char* end = p + segment.p_memsz;
        while (static_cast<size_t>(end - p) >= sizeof(ElfW(Nhdr))) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const char* name = p + sizeof(ElfW(Nhdr));
            const char* desc = name + pad(note->n_namesz);
            const char* next = desc + pad(note->n_descsz);
            if (next > end || next <= p)
                break;
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4
                && std::memcmp(name, "GNU", 4) == 0) {
                *out = QByteArray(desc, static_cast<qsizetype>(note->n_descsz)).toHex();
                return 1;
            }
            p = next;
        }
    }
    return 1;
}
#endif

QString reportFolder() {
    return QDir::homePath() + QStringLiteral("/.gitbolt");
}

} // namespace

QByteArray CrashHandler::renderHeader(const char* appVersion,
                                      bool officialBuild, bool envOptOut,
                                      bool testSession, QByteArrayView buildId) {
    // Format 2 adds the version and the three lines CrashLogParser
    // needs to refuse a report (format 1 always said "App version:
    // unknown": qApp did not exist yet when install() ran). One
    // ~/.gitbolt/crash.log is shared by every build and session under
    // a HOME (test sessions write crash-test.log now; older builds
    // didn't), so the report has to say where it came from. The build
    // ID comes last: a header rendered without one is still a prefix.
    QByteArray header =
        (QStringLiteral("=== GitBolt Crash Report ===\n")
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
    if (!buildId.isEmpty())
        header += "Build ID: " + buildId.toByteArray() + '\n';
    return header;
}

QByteArray CrashHandler::executableBuildId() {
#if defined(Q_OS_MACOS)
    // LC_UUID, as dwarfdump --uuid prints it, without the dashes.
    const auto* header = reinterpret_cast<const mach_header_64*>(_NSGetMachExecuteHeader());
    if (!header || header->magic != MH_MAGIC_64)
        return {};
    const auto* command = reinterpret_cast<const load_command*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_UUID) {
            const auto* uuid = reinterpret_cast<const uuid_command*>(command);
            return QByteArray(reinterpret_cast<const char*>(uuid->uuid), sizeof(uuid->uuid))
                .toHex();
        }
        command = reinterpret_cast<const load_command*>(
            reinterpret_cast<const char*>(command) + command->cmdsize);
    }
    return {};
#elif defined(Q_OS_LINUX)
    QByteArray id;
    dl_iterate_phdr(readGnuBuildId, &id);
    return id;
#elif defined(Q_OS_WIN)
    CodeView cv;
    if (readCodeView(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)), cv))
        return QByteArray(cv.id, static_cast<qsizetype>(cv.idLen));
    return {};
#else
    return {};
#endif
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
                           bool envOptOut, bool testSession,
                           const QString& reportDir) {
#if defined(Q_OS_MACOS)
    if (testSession)
        keepCrashReporterOut();
#endif
    // Pre-render the path (this also creates the folder — mkpath is
    // NOT signal-safe, so it must happen here) and the static system
    // header the handler will write verbatim. A test session writes a
    // file of its own: a bridge run outside the e2e harness (which gives
    // each app a HOME of its own) keeps the real HOME, and the handler
    // truncates what it writes, so a report of the user's,
    // still waiting for their next launch, would be lost to a test's.
    const QString folder = reportDir.isEmpty() ? reportFolder() : reportDir;
    QDir().mkpath(folder);
    const QString path =
        folder + (testSession ? QStringLiteral("/crash-test.log") : QStringLiteral("/crash.log"));
#if defined(Q_OS_WIN)
    const QString native = QDir::toNativeSeparators(path);
    g_crashPathW[0] = L'\0';
    if (native.size() < static_cast<qsizetype>(std::size(g_crashPathW)))
        g_crashPathW[native.toWCharArray(g_crashPathW)] = L'\0';
#else
    const QByteArray pathBytes = QFile::encodeName(path);
    std::snprintf(g_crashPath, sizeof(g_crashPath), "%s",
                  pathBytes.constData());
#endif

    const QByteArray header = renderHeader(appVersion, officialBuild, envOptOut,
                                           testSession, executableBuildId());
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
    // started through a link, that is the link's name, and the parser
    // would count none of those frames as GitBolt's. Linux's
    // /proc/self/exe is the file itself; where it doesn't exist
    // (macOS), dladdr's path with symlinks resolved.
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
#elif defined(Q_OS_WIN)
    // std::signal(SIGSEGV) is per thread in the UCRT: only the main
    // thread's faults reached it. The filter is process-wide; SIGABRT
    // is global. Room on the main thread's stack for the filter after
    // a stack overflow.
    g_exeBase = GetModuleHandleW(nullptr);
    SetUnhandledExceptionFilter(onUnhandledException);
    std::signal(SIGABRT, onAbort);
    _set_invalid_parameter_handler(onInvalidParameter);
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
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
    const QString dir = reportFolder();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/crash.log");
}

QString CrashHandler::testCrashLogPath() {
    const QString dir = reportFolder();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/crash-test.log");
}

} // namespace gitbolt::util
