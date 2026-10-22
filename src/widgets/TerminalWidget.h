#pragma once

#include <QPlainTextEdit>
#include <QSize>
#include <QString>
#include <QStringDecoder>
#include <QTextCursor>

#ifdef Q_OS_WIN
#include <memory>
#else
#include <sys/types.h>  // pid_t
#endif

class QSocketNotifier;
class QTimer;

namespace gitbolt::widgets {

#ifdef Q_OS_WIN
class ConPtyProcess;
#endif

/// Interactive terminal panel — forks a real shell under a PTY
/// (pseudo-terminal) and pipes its I/O through Qt so the user
/// can run any command line tool, exactly like in iTerm/Terminal.app.
///
/// The widget is intentionally minimal: it keeps the shell's cursor
/// between reads and honors the moves and erases within its row that
/// line editors and progress meters use, strips the rest of the ANSI
/// escape sequences (no color rendering yet), forwards keystrokes to
/// the child shell, and resizes the PTY when the widget is resized.
/// On macOS and Linux rows don't wrap and the moves between rows are
/// dropped (see appendOutput): a line longer than the terminal is one
/// row here, and only a CR knows where its screen rows start (so
/// readline's long command lines stay whole; a move up into a wrapped
/// line's earlier row is still lost). Full ANSI color, fullscreen TUI support
/// (vim/htop), and a real SGR parser are intentional follow-ups.
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
/// A flood of output (`yes`, a `cat` of a big file) is drawn a slice
/// at a time with the event loop turning in between, so the window
/// keeps repainting and Ctrl+C gets through. Meanwhile the full PTY
/// holds the shell back.
///
/// Ctrl+C interrupts. Outside macOS it is also the copy key, so there
/// it copies a selection instead, if there is one.
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
/// at the size it is created with.
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

    /// For a bare repository: GIT_DIR in the shell's environment, which
    /// git needs where safe.bareRepository is "explicit". Empty: no
    /// GIT_DIR at all (an inherited one is left out too, as are git's
    /// other repository variables: git::GitProcess::repositoryEnvVars).
    /// Takes effect at start(); changeRepository() changes it in a
    /// running shell.
    void setGitDir(const QString& gitDir) { gitDir_ = gitDir; }
    QString gitDir() const { return gitDir_; }

    /// Send `cd <path>\n` to the running shell. No-op if the
    /// shell is not running. Path is shell-quoted and prefixed
    /// with a leading space so the entry is excluded from history
    /// when HISTCONTROL=ignorespace is set.
    void changeDirectory(const QString& path);

    /// The repository the shell works in: `cd` there, as
    /// changeDirectory, with `gitDir` as setGitDir has it. A running
    /// shell whose GIT_DIR changes (a bare repository opened or left) is
    /// typed the change with the `cd`, in its own syntax (sh's `export`
    /// and `unset`, csh's `setenv` and `unsetenv`, fish's `set`), once a
    /// Ctrl+C has dropped whatever its line holds: at once at its
    /// prompt, but a moment after the command running in it ends if
    /// there is one. A shell that syntax isn't known for, and cmd on
    /// Windows, is started again there instead (see switchWhenIdle).
    void changeRepository(const QString& path, const QString& gitDir);

    /// Returns true if the child shell is currently running.
    bool isRunning() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

    /// Draw a read's worth of the shell's output. Protected rather
    /// than private so tests can feed it without a PTY.
    void appendOutput(const QByteArray& bytes);

    /// Each read of the shell's terminal (the PTY, or ConPTY), as it
    /// came, just before appendOutput() draws it. Does nothing; a test
    /// overrides it to tell what the terminal sent from what the widget
    /// made of it.
    virtual void outputRead(const QByteArray& bytes) { Q_UNUSED(bytes); }

    /// How much of the output so far waits for the next read: the start
    /// of an escape sequence a read cut short (see pendingOutput_).
    /// Protected so tests can tell it stays small.
    qsizetype pendingOutputSize() const { return pendingOutput_.size(); }

    /// How much of `run` (text that starts where a character does) goes
    /// on a screen row with `room` columns left, in UTF-16 units, which
    /// is how the Windows screen model counts columns: as much as fits
    /// and at least one unit, but never part of a character. A surrogate
    /// pair, or a letter and its combining marks, that starts in the
    /// room goes in whole. Only Windows wraps rows; this is built
    /// everywhere so the tests can reach it.
    static qsizetype rowChunk(QStringView run, qsizetype room);

    /// The character grid the viewport fits, the size the shell's
    /// terminal is given; invalid until the font has metrics. Protected
    /// so tests can write a row that ends at the terminal's last column.
    QSize gridSize() const;

private slots:
    void onPtyReadable();
    void onPtyWritable();

private:
    void stopShell();
    /// changeRepository's GIT_DIR and `cd` (or only its `cd`, back to the
    /// GIT_DIR the shell has, or a start again): now, unless a command
    /// the shell started is running, and then once it no longer is.
    void switchWhenIdle();
    /// Whether a command the shell started holds its terminal (on
    /// Windows, a console program cmd runs).
    bool commandRunning() const;
    /// What goes ahead of the `cd`, on its line, to give the shell
    /// gitDir_ as its GIT_DIR or to unset it; null where that can't be
    /// typed, and on Windows.
    QString gitDirCommand() const;
    /// The user sends the shell something: its line, if one was dropped
    /// for a GIT_DIR (lineDropped_), may hold something again.
    void noteTyped();
#ifndef Q_OS_WIN
    /// The name of the program the shell's process runs now.
    QString shellProgram() const;
    /// The shell's terminal holds a whole line it hasn't read yet.
    bool lineUnread() const;
#endif
    void writeToPty(const QByteArray& bytes);
    /// Write as much as the master fd accepts right now; returns the
    /// byte count written (stops at EAGAIN without dropping).
    int writeRaw(const QByteArray& bytes);
    void updatePtySize();
#ifdef Q_OS_WIN
    /// Draw the output ConPtyProcess has read since the last call.
    void takeConPtyOutput();
    // The screen ConPTY paints (see appendOutput).
    void startScreen();
    void trimBlankRows();
    void screenMoveTo(QTextCursor& cur, int row, int column);
    void screenLineFeed(QTextCursor& cur);
    void resizeScreen(const QSize& grid);
#else
    /// Read what the PTY holds, kMaxReadPerWake at most, and draw it;
    /// false once the shell's end of it is closed.
    bool takePtyOutput();
#endif
    /// Act on a CSI sequence: the moves and erases within a row
    /// everywhere, and on Windows the screen's too.
    void csi(QTextCursor& cur, ushort finalByte, QStringView params);

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
    int              screenRows_    = 25;
#else
    pid_t            childPid_     = -1;
#endif
    /// Where the shell's cursor is: the next read writes there. Null
    /// until a shell's first read, which starts below what is shown.
    QTextCursor      screenCursor_;
    /// The columns the shell was last told its terminal has.
    int              screenColumns_ = 80;
    QSocketNotifier* readNotifier_ = nullptr;
    QSocketNotifier* writeNotifier_ = nullptr;
    /// Active while a GIT_DIR, or a `cd`, waits for a command to end
    /// (switchWhenIdle), which it checks for every so often, or for a
    /// tick after the command's end, or for the Ctrl+C sent ahead of a
    /// GIT_DIR to take.
    QTimer*          switchTimer_  = nullptr;
    /// Bytes accepted by writeToPty but not yet written — the PTY
    /// master buffer is only ~1-4 KB, so a paste overflows it
    /// easily. Drained via writeNotifier_ as the fd becomes
    /// writable again (the old code silently dropped the tail).
    QByteArray writeQueue_;
    /// The shell's output is UTF-8, and a read() boundary can cut a
    /// character in two. The decoder keeps state across reads: it
    /// holds a character's first bytes back until the rest arrives,
    /// where decoding each read on its own turned both halves into
    /// U+FFFD.
    QStringDecoder   decoder_{QStringDecoder::Utf8};
    /// Tail of an escape sequence split across a read() boundary,
    /// re-prepended to the next chunk. Colored output splits like
    /// this constantly; without the carry the tail ("[0m", "[K", …)
    /// printed as literal text. Only a sequence's worth: one too long to
    /// be real is not kept (see appendOutput).
    QString          pendingOutput_;
    /// Partway through a CSI too long to be a real one: the rest of it,
    /// up to and with its final byte, is dropped as it arrives.
    bool             droppingCsi_  = false;

    // The directory the shell should cd into the next time it
    // starts. Cached so RepositoryView can call setInitialPath
    // before the user ever opens the Console tab.
    QString          pendingCwd_;
    QString          gitDir_;          // setGitDir
    QString          shellGitDir_;     // the running shell's GIT_DIR, as far as is known
    QString          shellName_;       // the file name of the shell started
    /// A Ctrl+C has dropped the shell's line for the GIT_DIR that waits,
    /// and nothing has gone to the shell since (noteTyped): the change
    /// can be typed (see switchWhenIdle). A command that runs after the
    /// Ctrl+C, a prompt hook's, leaves it set. Once per switch; cleared
    /// as the change is typed, and for a new shell (stopShell).
    bool             lineDropped_  = false;
    /// The switch has seen a command run, and a change of GIT_DIR waits
    /// a tick after its end before it goes on (see switchWhenIdle).
    bool             commandRan_   = false;
    /// The GIT_DIR waits for switchTimer_'s next tick: the Ctrl+C has
    /// only just gone, or the command only just ended.
    bool             waitForTick_  = false;
    /// Ticks the Ctrl+C has waited for the shell to read a line typed
    /// ahead (see switchWhenIdle), up to kMaxUnreadWaits.
    int              unreadWaits_  = 0;
    bool             autoStarted_  = false;
};

} // namespace gitbolt::widgets
