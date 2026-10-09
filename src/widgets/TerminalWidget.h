#pragma once

#include <QPlainTextEdit>
#include <QSize>
#include <QString>

#ifdef Q_OS_WIN
#include <QTextCursor>

#include <memory>
#else
#include <sys/types.h>  // pid_t
#endif

class QSocketNotifier;

namespace gitbolt::widgets {

#ifdef Q_OS_WIN
class ConPtyProcess;
#endif

/// Interactive terminal panel — forks a real shell under a PTY
/// (pseudo-terminal) and pipes its I/O through Qt so the user
/// can run any command line tool, exactly like in iTerm/Terminal.app.
///
/// The widget is intentionally minimal: it strips ANSI escape
/// sequences (no color rendering yet), forwards keystrokes to the
/// child shell, and resizes the PTY when the widget is resized.
/// Full ANSI color, fullscreen TUI support (vim/htop), and a real
/// SGR parser are intentional follow-ups.
///
/// Lifecycle:
///   - Constructed in the disconnected state.
///   - start(cwd) opens a PTY pair, forks, and execs the user's
///     login shell (`$SHELL`, falling back to /bin/sh) with the
///     given working directory.
///   - changeDirectory(path) sends `cd <path>\n` to the running
///     shell so the cwd updates without restarting it.
///   - The destructor SIGHUPs the child and closes the PTY.
///
/// Windows: the same widget over a ConPTY pseudo console (Windows 10
/// 1809+, see ConPtyProcess) running %COMSPEC% (cmd.exe), which
/// changeDirectory() addresses with `cd /d`. Closing the pseudo console
/// is the hang-up. ConPTY paints a screen with absolute cursor moves
/// rather than streaming lines, so there the walker also keeps a
/// screen model (cursor addressing, erase, wrap, scroll; still no
/// colors). Output is drawn in batches as the GUI thread gets to it,
/// and ConPtyProcess stops reading while a full batch waits, so a
/// flood holds the shell back instead of queueing up without bound.
/// Pasted line breaks go in as CR, the Enter key. A start() while
/// hidden waits for the next show, since ConPTY lays its screen out
/// at the size it is created with. Ctrl+C copies only a selection;
/// otherwise it interrupts.
class TerminalWidget : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit TerminalWidget(QWidget* parent = nullptr);
    ~TerminalWidget() override;

    /// Spawn a shell child under a fresh PTY. If a shell is
    /// already running it's stopped first. workingDirectory is
    /// the initial cwd for the child; pass an empty string to
    /// inherit the parent's cwd.
    void start(const QString& workingDirectory = {});

    /// Send `cd <path>\n` to the running shell. No-op if the
    /// shell is not running. Path is shell-quoted and prefixed
    /// with a leading space so the entry is excluded from history
    /// when HISTCONTROL=ignorespace is set.
    void changeDirectory(const QString& path);

    /// Returns true if the child shell is currently running.
    bool isRunning() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onPtyReadable();
    void onPtyWritable();

private:
    void stopShell();
    /// The character grid the viewport fits; invalid until the font
    /// has metrics.
    QSize gridSize() const;
    void writeToPty(const QByteArray& bytes);
    /// Write as much as the master fd accepts right now; returns the
    /// byte count written (stops at EAGAIN without dropping).
    int writeRaw(const QByteArray& bytes);
    void appendOutput(const QByteArray& bytes);
    void updatePtySize();
#ifdef Q_OS_WIN
    /// Draw the output ConPtyProcess has read since the last call.
    void takeConPtyOutput();
    // The screen ConPTY paints (see appendOutput).
    void startScreen();
    void trimBlankRows();
    void screenMoveTo(QTextCursor& cur, int row, int column);
    void screenLineFeed(QTextCursor& cur);
    void screenCsi(QTextCursor& cur, ushort finalByte, QStringView params);
    void resizeScreen(const QSize& grid);
#endif

    int              masterFd_     = -1;   // stays -1 on Windows
#ifdef Q_OS_WIN
    /// The running shell's session; null when none runs.
    std::unique_ptr<ConPtyProcess> conpty_;
    /// Bumped whenever a session starts or stops: output cues and exit
    /// notices a stopped shell had already queued are then dropped.
    quint64          conptyGeneration_ = 0;
    /// ConPTY's screen: its rows are the blocks from screenTop_ down
    /// (anything above is scrollback), ConPTY's cursor is at
    /// screenCursor_, and the size is the one ConPTY was last given.
    QTextCursor      screenTop_;
    QTextCursor      screenCursor_;
    int              screenColumns_ = 80;
    int              screenRows_    = 25;
#else
    pid_t            childPid_     = -1;
#endif
    QSocketNotifier* readNotifier_ = nullptr;
    QSocketNotifier* writeNotifier_ = nullptr;
    /// Bytes accepted by writeToPty but not yet written — the PTY
    /// master buffer is only ~1-4 KB, so a paste overflows it
    /// easily. Drained via writeNotifier_ as the fd becomes
    /// writable again (the old code silently dropped the tail).
    QByteArray writeQueue_;
    /// Tail of an escape sequence split across a 4096-byte read()
    /// boundary, re-prepended to the next chunk. Colored output
    /// splits like this constantly; without the carry the tail
    /// ("[0m", "[K", …) printed as literal text.
    QByteArray pendingOutput_;

    // The directory the shell should cd into the next time it
    // starts. Cached so RepositoryView can call setInitialPath
    // before the user ever opens the Console tab.
    QString          pendingCwd_;
    bool             autoStarted_  = false;
};

} // namespace gitbolt::widgets
