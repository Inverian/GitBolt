#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gitbolt::widgets {

/// A child process attached to a Windows pseudo console (ConPTY,
/// Windows 10 1809+): the Win32 half of TerminalWidget's Windows
/// backend. Deliberately Qt-free, and built as its own target with no
/// Qt on the include path, so the Win32 code compiles and tests on its
/// own; TerminalWidget is the thin Qt adapter on top.
///
/// Threads: output is read on a background thread into a buffer that
/// the owner empties with takeOutput(). The OutputHandler runs on that
/// thread when output arrives and none was waiting, as the cue to take
/// it; whatever follows before then joins the wait without another
/// cue. The ExitHandler runs once, on another background thread, after
/// the last output has been read.
/// Handlers never run concurrently with each other and never after
/// stop() returns, so an adapter can post from them to its own thread
/// and be destroyed right after stop().
///
/// Flow control: no more than kMaxPendingOutput bytes wait to be
/// taken. When the next read would not fit, the reader waits; conhost
/// then blocks writing its output and the child blocks writing to
/// conhost, the way a full PTY holds back a Unix child. An owner
/// slower than the child so never falls behind without bound.
///
/// No call blocks for long. Closing a pseudo console can block until
/// conhost exits, which on Windows 10 means until its output pipe has
/// been drained, so the hang-up runs on a background thread while the
/// reader keeps draining.
class ConPtyProcess {
public:
    using OutputHandler = std::function<void()>;
    using ExitHandler   = std::function<void(unsigned long exitCode)>;

    /// How much output may wait for takeOutput() before the reader
    /// stops reading (except while write() blocks; see there). Small on
    /// purpose: it is also the most an owner gets to draw at once, and
    /// TerminalWidget's walker takes some 10 microseconds a byte (Qt
    /// lays the row out again on every character), so 16 KB keeps its
    /// GUI thread busy for a fifth of a second or so. Throughput is the
    /// owner's either way.
    static constexpr std::size_t kMaxPendingOutput = 16 * 1024;

    struct StartInfo {
        /// Handed to CreateProcessW as is, so quote the program path.
        std::wstring commandLine;
        /// Initial cwd; empty inherits ours.
        std::wstring workingDirectory;
        /// NAME/value pairs layered over our own environment. Names
        /// match case-insensitively, as Windows compares them.
        std::vector<std::pair<std::wstring, std::wstring>> environment;
        int columns = 80;
        int rows    = 25;
    };

    ConPtyProcess() = default;
    ~ConPtyProcess();
    ConPtyProcess(const ConPtyProcess&) = delete;
    ConPtyProcess& operator=(const ConPtyProcess&) = delete;

    /// Create the pseudo console and start the child in it, stopping
    /// any previous child first. On failure returns false, and
    /// errorString() names the call that failed and why.
    bool start(const StartInfo& info, OutputHandler onOutput,
               ExitHandler onExit);

    /// The output read since the last call, as UTF-8 that never ends
    /// partway through a character; empty if there is none. Taking it
    /// lets a reader stopped by kMaxPendingOutput read on.
    std::string takeOutput();

    /// Send terminal input: UTF-8 text and VT key sequences ("\r" is
    /// Enter, a bare "\n" is not). Blocks only while conhost catches up
    /// on its input pipe; meanwhile the reader reads past
    /// kMaxPendingOutput, since conhost may take no input until its
    /// pending output has gone out.
    bool write(std::string_view bytes);

    /// Resize the pseudo console. A no-op when the size is unchanged:
    /// conhost repaints the whole screen on every resize.
    bool resize(int columns, int rows);

    /// Hang up: close the pseudo console, which sends every process
    /// attached to it CTRL_CLOSE_EVENT; a child still alive after a
    /// grace period is terminated. Handlers are detached before this
    /// returns, and output not yet taken is dropped. Returns true if the
    /// child has exited and its output is drained within timeoutMs;
    /// otherwise the teardown finishes on its own in the background.
    bool stop(unsigned long timeoutMs);

    /// True until the child exits.
    bool isRunning() const;
    unsigned long processId() const;
    const std::wstring& errorString() const { return error_; }

private:
    struct Session;
    std::shared_ptr<Session> session_;
    std::wstring error_;
};

} // namespace gitbolt::widgets
