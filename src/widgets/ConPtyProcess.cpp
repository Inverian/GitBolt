#include "widgets/ConPtyProcess.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>   // _beginthreadex

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace gitbolt::widgets {

namespace {

// How long a hung-up child gets to exit before it is terminated, and
// how long conhost gets to exit (breaking the output pipe) before the
// blocked read is cancelled instead.
constexpr DWORD kGraceMs = 2000;

// Newer conhost builds open a session by asking the terminal for its
// device attributes (DA1) and stall for up to 3 s until the answer
// arrives; older ones never ask. The reply claims a VT100-class
// terminal with no extensions, which is honest for TerminalWidget.
// The query precedes all real output, so only the start is searched.
constexpr std::string_view kDa1Query     = "\x1b[c";
constexpr std::string_view kDa1QueryZero = "\x1b[0c";
constexpr std::string_view kDa1Reply     = "\x1b[?61c";
constexpr std::size_t      kDa1Window    = 256;

void closeHandle(HANDLE& handle)
{
    if (handle) {
        CloseHandle(handle);
        handle = nullptr;
    }
}

std::wstring errorMessage(const wchar_t* call, DWORD code)
{
    std::wstring message = std::wstring(call) + L" failed";
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    if (length > 0 && text) {
        std::wstring_view view(text, length);
        while (!view.empty() && (view.back() == L'\r' || view.back() == L'\n'))
            view.remove_suffix(1);
        message.append(L": ").append(view);
    } else {
        message += L" (error " + std::to_wstring(code) + L')';
    }
    if (text)
        LocalFree(text);
    return message;
}

// CreatePseudoConsole reports HRESULTs; FormatMessage knows the plain
// Win32 code behind most of them.
DWORD win32Code(HRESULT result)
{
    return HRESULT_FACILITY(result) == FACILITY_WIN32
               ? static_cast<DWORD>(HRESULT_CODE(result))
               : static_cast<DWORD>(result);
}

SHORT cells(int count)
{
    return static_cast<SHORT>(std::clamp(count, 1, static_cast<int>(SHRT_MAX)));
}

// Length of the longest prefix of `data` that doesn't end partway
// through a UTF-8 sequence.
std::size_t completeUtf8(const char* data, std::size_t size)
{
    for (std::size_t back = 1; back <= std::min<std::size_t>(size, 3); ++back) {
        const auto byte = static_cast<unsigned char>(data[size - back]);
        if ((byte & 0xC0) == 0x80)
            continue;  // continuation byte: keep looking for the lead
        const std::size_t length = byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3
                                 : byte >= 0xC0 ? 2 : 1;
        return length > back ? size - back : size;
    }
    return size;  // no lead byte in reach: not UTF-8, pass it on
}

bool writeAll(HANDLE pipe, std::string_view bytes)
{
    while (!bytes.empty()) {
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size(), MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(pipe, bytes.data(), chunk, &written, nullptr) || written == 0)
            return false;
        bytes.remove_prefix(written);
    }
    return true;
}

// Name of a NAME=value entry. cmd's per-drive cwd entries ("=C:=C:\x")
// start with '=', and that '=' is part of the name.
std::wstring_view nameOf(std::wstring_view entry)
{
    return entry.substr(0, entry.find(L'=', 1));
}

int compareNames(std::wstring_view a, std::wstring_view b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE);
}

// Our environment with `overrides` layered on top, as the block
// CreateProcessW takes: sorted by name, case-insensitively, each entry
// NUL-terminated and the whole block terminated by one more NUL.
std::wstring environmentBlock(
    const std::vector<std::pair<std::wstring, std::wstring>>& overrides)
{
    std::vector<std::wstring> entries;
    if (wchar_t* inherited = GetEnvironmentStringsW()) {
        for (const wchar_t* p = inherited; *p; p += std::wcslen(p) + 1) {
            const std::wstring_view entry(p);
            const bool overridden = std::any_of(
                overrides.begin(), overrides.end(), [&](const auto& o) {
                    return compareNames(nameOf(entry), o.first) == CSTR_EQUAL;
                });
            if (!overridden)
                entries.emplace_back(entry);
        }
        FreeEnvironmentStringsW(inherited);
    }
    for (const auto& [name, value] : overrides)
        entries.push_back(name + L'=' + value);

    std::stable_sort(entries.begin(), entries.end(),
                     [](const std::wstring& a, const std::wstring& b) {
                         return compareNames(nameOf(a), nameOf(b)) == CSTR_LESS_THAN;
                     });
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry;
        block += L'\0';
    }
    block += L'\0';  // an empty environment is two NULs: this + c_str()'s
    return block;
}

} // namespace

// Shared by the owner and both threads and freed by whichever lets go
// last, so stop() never has to wait for the threads.
struct ConPtyProcess::Session {
    HPCON  console   = nullptr;  // consoleMutex once the threads run
    COORD  size{};               // consoleMutex
    HANDLE input     = nullptr;  // our end of the pipe conhost reads
    HANDLE output    = nullptr;  // our end of the pipe conhost writes
    HANDLE process   = nullptr;
    HANDLE reader    = nullptr;  // the reader thread
    HANDLE hangup    = nullptr;  // manual reset: stop() was called
    HANDLE finished  = nullptr;  // manual reset: child gone, reader done
    DWORD  processId = 0;

    std::mutex    consoleMutex;
    std::mutex    handlerMutex;
    OutputHandler onOutput;      // handlerMutex
    ExitHandler   onExit;        // handlerMutex

    // Output read but not taken yet. The reader waits on outputRoom
    // while there is too much of it, unless the owner is blocked in
    // write() or has hung up; each of those notifies it.
    std::mutex              outputMutex;
    std::condition_variable outputRoom;
    std::string             pending;          // outputMutex
    bool                    writing = false;  // outputMutex
    bool                    hungUp  = false;  // outputMutex: drop output

    ~Session()
    {
        // Only a failed start() gets here with the console still open.
        // Break the output pipe first, so conhost can't block writing
        // to it while ClosePseudoConsole waits for conhost to exit.
        closeHandle(output);
        if (console)
            ClosePseudoConsole(console);
        closeHandle(input);
        closeHandle(process);
        closeHandle(reader);
        closeHandle(hangup);
        closeHandle(finished);
    }

    // Start `body` on a thread that holds its own reference to the
    // session. Returns the thread handle, or null.
    static HANDLE spawn(unsigned (__stdcall* body)(void*),
                        const std::shared_ptr<Session>& session)
    {
        auto* ref = new std::shared_ptr<Session>(session);
        const std::uintptr_t thread = _beginthreadex(nullptr, 0, body, ref, 0, nullptr);
        if (thread == 0) {
            delete ref;
            return nullptr;
        }
        return reinterpret_cast<HANDLE>(thread);
    }

    // Starts the reader, waits for the child to exit or for stop(),
    // then hangs up and reports the exit once the output is drained.
    static unsigned __stdcall supervise(void* param)
    {
        const std::unique_ptr<std::shared_ptr<Session>> ref(
            static_cast<std::shared_ptr<Session>*>(param));
        Session& s = **ref;

        s.reader = spawn(readOutput, *ref);
        if (s.reader) {
            const HANDLE events[] = {s.process, s.hangup};
            WaitForMultipleObjects(2, events, FALSE, INFINITE);
        } else {
            // Nothing would drain the output: hang up at once, with the
            // pipe broken so conhost can't block writing to it.
            closeHandle(s.output);
        }

        // Closing the pseudo console is the hang-up: conhost sends
        // CTRL_CLOSE_EVENT to everything attached (the shell and all it
        // started) and exits, which breaks the output pipe and ends the
        // reader. On Windows 10 ClosePseudoConsole returns only once
        // conhost has exited, and conhost can't exit while its last
        // frame sits unread in the pipe; the reader keeps draining
        // meanwhile (as the owner takes the output, or freely once it
        // has hung up), which is why this runs here and not in stop().
        HPCON console = nullptr;
        {
            const std::lock_guard lock(s.consoleMutex);
            console = std::exchange(s.console, nullptr);
        }
        ClosePseudoConsole(console);

        if (WaitForSingleObject(s.process, kGraceMs) == WAIT_TIMEOUT) {
            TerminateProcess(s.process, 1);
            WaitForSingleObject(s.process, kGraceMs);
        }
        // Normally done already. Should conhost linger, cancel the
        // blocked read rather than wait on it forever.
        if (s.reader) {
            while (WaitForSingleObject(s.reader, kGraceMs) == WAIT_TIMEOUT)
                CancelSynchronousIo(s.reader);
        }

        DWORD exitCode = 0;
        GetExitCodeProcess(s.process, &exitCode);
        SetEvent(s.finished);

        const std::lock_guard lock(s.handlerMutex);
        if (s.onExit)
            s.onExit(exitCode);
        return 0;
    }

    static unsigned __stdcall readOutput(void* param)
    {
        const std::unique_ptr<std::shared_ptr<Session>> ref(
            static_cast<std::shared_ptr<Session>*>(param));
        Session& s = **ref;

        std::string head;  // output so far, while a DA1 query may come
        bool awaitingDa1 = true;
        // ConPTY output is UTF-8. A sequence cut off at the end of a
        // read is held back (at most 3 bytes) and completed by the next
        // one, so the output queued never ends partway through one.
        constexpr DWORD kReadSize = 4096;
        char buffer[kReadSize + 3];
        std::size_t carried = 0;
        for (;;) {
            DWORD count = 0;
            // Fails with ERROR_BROKEN_PIPE once conhost has exited, or
            // ERROR_OPERATION_ABORTED when supervise() cancels it.
            if (!ReadFile(s.output, buffer + carried, kReadSize, &count, nullptr))
                break;
            const std::size_t size = carried + count;
            const std::size_t complete = completeUtf8(buffer, size);
            const std::string_view bytes(buffer, complete);

            if (awaitingDa1) {
                head.append(bytes);
                if (head.find(kDa1Query) != std::string::npos
                    || head.find(kDa1QueryZero) != std::string::npos) {
                    writeAll(s.input, kDa1Reply);
                    awaitingDa1 = false;
                } else if (head.size() >= kDa1Window) {
                    awaitingDa1 = false;
                }
                if (!awaitingDa1)
                    std::string().swap(head);
            }

            if (!bytes.empty() && s.queueOutput(bytes)) {
                const std::lock_guard lock(s.handlerMutex);
                if (s.onOutput)
                    s.onOutput();
            }
            carried = size - complete;
            std::memmove(buffer, buffer + complete, carried);
        }
        return 0;
    }

    // Queue output for takeOutput(), first waiting for room while too
    // much is queued already: the flow control. Returns true if the
    // queue was empty, so the owner needs a cue.
    bool queueOutput(std::string_view bytes)
    {
        std::unique_lock lock(outputMutex);
        outputRoom.wait(lock, [&] {
            return pending.size() + bytes.size() <= kMaxPendingOutput
                   || writing || hungUp;
        });
        if (hungUp)
            return false;  // no one takes it any more; keep draining
        pending.append(bytes);
        return pending.size() == bytes.size();
    }
};

ConPtyProcess::~ConPtyProcess()
{
    stop(0);
}

bool ConPtyProcess::start(const StartInfo& info, OutputHandler onOutput,
                          ExitHandler onExit)
{
    stop(0);
    error_.clear();
    const auto fail = [this](const wchar_t* call, DWORD code) {
        error_ = errorMessage(call, code);
        return false;
    };

    auto session = std::make_shared<Session>();
    session->hangup   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    session->finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!session->hangup || !session->finished)
        return fail(L"CreateEvent", GetLastError());

    // Two pipes: conhost reads the child's input from one and writes the
    // rendered screen to the other. CreatePseudoConsole duplicates the
    // far ends for conhost, so ours are closed right away; while we held
    // the output pipe's write end, reads could never see it break.
    HANDLE inputRead   = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&inputRead, &session->input, nullptr, 0))
        return fail(L"CreatePipe", GetLastError());
    if (!CreatePipe(&session->output, &outputWrite, nullptr, 0)) {
        const DWORD error = GetLastError();
        CloseHandle(inputRead);
        return fail(L"CreatePipe", error);
    }
    session->size = {cells(info.columns), cells(info.rows)};
    const HRESULT created = CreatePseudoConsole(session->size, inputRead,
                                                outputWrite, 0, &session->console);
    CloseHandle(inputRead);
    CloseHandle(outputWrite);
    if (FAILED(created)) {
        session->console = nullptr;
        return fail(L"CreatePseudoConsole", win32Code(created));
    }

    // The attribute list is what attaches the child to the console.
    SIZE_T listSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);  // sizing only
    std::vector<std::byte> listStorage(listSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listStorage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &listSize))
        return fail(L"InitializeProcThreadAttributeList", GetLastError());
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   session->console, sizeof(session->console),
                                   nullptr, nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(list);
        return fail(L"UpdateProcThreadAttribute", error);
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = static_cast<DWORD>(sizeof(startup));
    // Null std handles, explicitly: without STARTF_USESTDHANDLES a
    // console child inherits ours when they are redirected (a test
    // runner, GitBolt started from a script) and writes around the
    // pseudo console instead of into it.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = list;

    std::wstring commandLine = info.commandLine;  // CreateProcessW may write to it
    std::wstring environment = environmentBlock(info.environment);
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        environment.data(),
        info.workingDirectory.empty() ? nullptr : info.workingDirectory.c_str(),
        &startup.StartupInfo, &child);
    const DWORD startError = GetLastError();
    DeleteProcThreadAttributeList(list);
    if (!started)
        return fail(L"CreateProcess", startError);
    CloseHandle(child.hThread);
    session->process   = child.hProcess;
    session->processId = child.dwProcessId;
    session->onOutput  = std::move(onOutput);
    session->onExit    = std::move(onExit);

    HANDLE supervisor = Session::spawn(Session::supervise, session);
    if (!supervisor) {
        const DWORD error = GetLastError();
        session->onOutput = nullptr;
        session->onExit   = nullptr;
        TerminateProcess(session->process, 1);
        return fail(L"_beginthreadex", error);
    }
    CloseHandle(supervisor);
    session_ = std::move(session);
    return true;
}

std::string ConPtyProcess::takeOutput()
{
    std::string output;
    if (session_) {
        {
            const std::lock_guard lock(session_->outputMutex);
            output.swap(session_->pending);
        }
        session_->outputRoom.notify_one();
    }
    return output;
}

bool ConPtyProcess::write(std::string_view bytes)
{
    if (!session_)
        return false;
    Session& s = *session_;
    // While this blocks, nothing takes output. And conhost may take no
    // more input until its output has gone out: it can block writing
    // that while holding the lock its input thread needs. So the
    // reader reads past the cap meanwhile, or each side would wait on
    // the other for good.
    {
        const std::lock_guard lock(s.outputMutex);
        s.writing = true;
    }
    s.outputRoom.notify_one();
    const bool written = writeAll(s.input, bytes);
    const std::lock_guard lock(s.outputMutex);
    s.writing = false;
    return written;
}

bool ConPtyProcess::resize(int columns, int rows)
{
    if (!session_)
        return false;
    const COORD size{cells(columns), cells(rows)};
    const std::lock_guard lock(session_->consoleMutex);
    if (!session_->console)
        return false;  // hung up already
    if (size.X == session_->size.X && size.Y == session_->size.Y)
        return true;
    if (FAILED(ResizePseudoConsole(session_->console, size)))
        return false;
    session_->size = size;
    return true;
}

bool ConPtyProcess::stop(unsigned long timeoutMs)
{
    if (!session_)
        return true;
    const std::shared_ptr<Session> session = std::move(session_);
    {
        // Nothing calls into the owner from here on, and the owner may
        // be about to go away. A handler running right now finishes
        // first, since it holds the lock.
        const std::lock_guard lock(session->handlerMutex);
        session->onOutput = nullptr;
        session->onExit   = nullptr;
    }
    {
        // Nor does anything take output: the reader drops it from here
        // on (it reads on until conhost has gone), so it must not stay
        // stopped at the cap.
        const std::lock_guard lock(session->outputMutex);
        session->hungUp = true;
        std::string().swap(session->pending);
    }
    session->outputRoom.notify_one();
    SetEvent(session->hangup);
    return WaitForSingleObject(session->finished, timeoutMs) == WAIT_OBJECT_0;
}

bool ConPtyProcess::isRunning() const
{
    return session_ && WaitForSingleObject(session_->process, 0) == WAIT_TIMEOUT;
}

unsigned long ConPtyProcess::processId() const
{
    return session_ ? session_->processId : 0;
}

} // namespace gitbolt::widgets
