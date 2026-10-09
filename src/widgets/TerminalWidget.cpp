#include "widgets/TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSocketNotifier>
#include <QTextCursor>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef Q_OS_WIN
#include "widgets/ConPtyProcess.h"

#include <QDir>
#include <QFileInfo>
#include <QTextBlock>
#include <QTextDocument>

#include <string_view>
#else
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(Q_OS_MACOS)
#include <util.h>      // forkpty
#elif defined(Q_OS_LINUX)
#include <pty.h>       // forkpty (glibc; linked from libutil before 2.34)
#else
#include <libutil.h>   // forkpty (FreeBSD)
#endif
#endif

#include <vector>

#ifdef Q_OS_MACOS
#include <crt_externs.h>   // _NSGetEnviron
#endif

namespace gitbolt::widgets {

namespace {

// The host process environment, portably. On macOS `environ` is not
// directly visible to shared code — Apple provides _NSGetEnviron().
#if defined(Q_OS_MACOS)
char** hostEnviron() { return *_NSGetEnviron(); }
#elif !defined(Q_OS_WIN)
extern "C" char** environ;
char** hostEnviron() { return environ; }
#endif

constexpr int kMaxScrollback = 5000;

// Text <-> the bytes the shell's terminal speaks. ConPTY speaks UTF-8
// whatever the ANSI code page, and the ANSI code page is what Qt's
// "local 8-bit" means on Windows; on Unix it means UTF-8 already.
QByteArray toTerminalBytes(const QString& text)
{
#ifdef Q_OS_WIN
    return text.toUtf8();
#else
    return text.toLocal8Bit();
#endif
}

QString fromTerminalBytes(const QByteArray& bytes)
{
#ifdef Q_OS_WIN
    return QString::fromUtf8(bytes);
#else
    return QString::fromLocal8Bit(bytes);
#endif
}

#ifdef Q_OS_WIN
// Put `cur` at `column` (0-based) of its row. A row shorter than that
// is padded: the cells past the end of a line are blank.
void placeInRow(QTextCursor& cur, int column)
{
    const QTextBlock block = cur.block();
    const int length = block.length() - 1;
    if (column <= length) {
        cur.setPosition(block.position() + column);
    } else {
        cur.movePosition(QTextCursor::EndOfBlock);
        cur.insertText(QString(column - length, QLatin1Char(' ')));
    }
}

// Blank the cells [from, to) of the cursor's row without moving it.
void blankCells(QTextCursor& cur, int from, int to)
{
    if (to <= from)
        return;
    const int base = cur.block().position();
    const int at = cur.positionInBlock();
    QTextCursor blank = cur;
    blank.setPosition(base + from);
    blank.setPosition(base + to, QTextCursor::KeepAnchor);
    blank.insertText(QString(to - from, QLatin1Char(' ')));
    cur.setPosition(base + at);
}
#endif
} // namespace

TerminalWidget::TerminalWidget(QWidget* parent)
    : QPlainTextEdit(parent)
{
    // The widget is NOT read-only — we need QPlainTextEdit to draw
    // a blinking caret so users can see where the next character
    // will land. All keyboard input is still intercepted in
    // keyPressEvent and forwarded to the PTY, so the editable flag
    // is really just a switch to get caret rendering. Disabling
    // undo/redo keeps Ctrl+Z from fighting terminal output.
    setReadOnly(false);
    setUndoRedoEnabled(false);

    // Monospace font sized for comfortable terminal use.
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setPointSize(12);
    setFont(f);

    setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#1e1e1e; color:#d4d4d4; "
        "border:none; padding:6px; selection-background-color:#3a5f8f; }"));

    setPlaceholderText(tr("Starting shell…"));

    // Bound the scrollback so a noisy command doesn't pin the
    // process. 5000 lines is plenty for normal git workflows.
    setMaximumBlockCount(kMaxScrollback);

    // Don't wrap — terminal output relies on the PTY column count
    // we report via TIOCSWINSZ.
    setLineWrapMode(QPlainTextEdit::NoWrap);

    // A 2px bar caret is easy to spot against the dark background.
    setCursorWidth(2);
}

TerminalWidget::~TerminalWidget()
{
    stopShell();
}

bool TerminalWidget::isRunning() const
{
#ifdef Q_OS_WIN
    return conpty_ != nullptr;
#else
    return childPid_ > 0 && masterFd_ >= 0;
#endif
}

void TerminalWidget::start(const QString& workingDirectory)
{
    if (isRunning())
        stopShell();

    if (!workingDirectory.isEmpty())
        pendingCwd_ = workingDirectory;

#ifdef Q_OS_WIN
    // A hidden widget has no real size yet, and ConPTY lays its screen
    // out at the size it is created with: started now, the banner
    // would wrap and scroll at a placeholder size before the first real
    // resize. showEvent starts it instead.
    if (!isVisible()) {
        autoStarted_ = false;
        return;
    }

    // %COMSPEC% is Windows' counterpart of $SHELL (the interpreter
    // system() and `start` run, cmd.exe in practice), and
    // %SystemRoot%\System32\cmd.exe stands in for /bin/sh. Not
    // PowerShell: it takes a second or more to start, and its line
    // editor redraws with more of VT than this widget renders.
    QString shell = qEnvironmentVariable("COMSPEC");
    if (shell.isEmpty()) {
        shell = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
                + QStringLiteral("\\System32\\cmd.exe");
    }
    shell = QDir::toNativeSeparators(shell);

    ConPtyProcess::StartInfo info;
    info.commandLine = QStringLiteral("\"%1\"").arg(shell).toStdWString();
    // As on Unix a bad cwd is not fatal, but CreateProcess would refuse
    // it outright instead of starting the shell elsewhere.
    if (!pendingCwd_.isEmpty() && QFileInfo(pendingCwd_).isDir())
        info.workingDirectory = QDir::toNativeSeparators(pendingCwd_).toStdWString();
    // The Unix child's TERM and GITBOLT_TERM. ConPTY renders the console
    // as xterm-256color, and TERM is what Git for Windows' less and vim
    // go by.
    info.environment = {{L"TERM", L"xterm-256color"}, {L"GITBOLT_TERM", L"1"}};
    if (const QSize grid = gridSize(); grid.isValid()) {
        info.columns = grid.width();
        info.rows    = grid.height();
    }

    // The handlers run on ConPtyProcess's threads: hop over to ours.
    // There is one output cue per batch, however much piles up before
    // we get to it, and all of it is drawn in one go; while a full batch
    // waits, ConPtyProcess stops reading and so holds the shell back,
    // as a full PTY does on Unix.
    const quint64 generation = ++conptyGeneration_;
    auto onOutput = [this, generation] {
        QMetaObject::invokeMethod(
            this,
            [this, generation] {
                if (generation == conptyGeneration_)
                    takeConPtyOutput();
            },
            Qt::QueuedConnection);
    };
    auto onExit = [this, generation](unsigned long) {
        QMetaObject::invokeMethod(
            this,
            [this, generation] {
                if (generation != conptyGeneration_)
                    return;
                takeConPtyOutput();  // normally none left: its cue came first
                stopShell();
                trimBlankRows();  // the screen's empty bottom rows
                appendPlainText(tr("\n[gitbolt] shell exited"));
            },
            Qt::QueuedConnection);
    };

    auto process = std::make_unique<ConPtyProcess>();
    if (!process->start(info, std::move(onOutput), std::move(onExit))) {
        appendPlainText(tr("[gitbolt] could not start %1: %2")
                            .arg(shell, QString::fromStdWString(process->errorString())));
        return;
    }
    conpty_ = std::move(process);
    // The new shell's screen starts below whatever is shown (its
    // output is queued, so none of it has been drawn yet).
    screenTop_     = QTextCursor();
    screenCursor_  = QTextCursor();
    screenColumns_ = info.columns;
    screenRows_    = info.rows;
    setPlaceholderText(QString{});
#else
    // -----------------------------------------------------------------
    // Snapshot EVERYTHING the child needs into plain byte buffers
    // BEFORE forking. GitBolt always has QtConcurrent git workers
    // alive, and a child of a multithreaded process may only call
    // async-signal-safe functions between fork and exec: Qt string
    // conversion, setenv and getpwuid all allocate or take locks, so
    // any of them can deadlock the child on a lock some other thread
    // held at fork time — the terminal then silently never starts.
    // chdir / execve / _exit are safe.
    // -----------------------------------------------------------------
    const QByteArray cwdBytes = pendingCwd_.toLocal8Bit();

    // Pick the user's login shell (parent side — getpwuid may lock).
    QByteArray shellBytes;
    if (const char* shellEnv = getenv("SHELL"); shellEnv && *shellEnv) {
        shellBytes = shellEnv;
    } else {
        struct passwd* pw = getpwuid(getuid());
        shellBytes = (pw && pw->pw_shell && *pw->pw_shell)
                         ? QByteArray(pw->pw_shell)
                         : QByteArrayLiteral("/bin/sh");
    }

    // Child environment = current env + TERM + GITBOLT_TERM.
    // TERM=xterm-256color so colorful programs (git, ls --color)
    // emit SGR sequences — we strip non-SGR CSI for now, so
    // fullscreen TUIs won't render properly (known follow-up).
    // GITBOLT_TERM lets dotfiles detect us.
    QList<QByteArray> envBytes;
    for (char** e = hostEnviron(); e && *e; ++e) {
        if (strncmp(*e, "TERM=", 5) == 0 ||
            strncmp(*e, "GITBOLT_TERM=", 13) == 0)
            continue;
        envBytes.append(QByteArray(*e));
    }
    envBytes.append(QByteArrayLiteral("TERM=xterm-256color"));
    envBytes.append(QByteArrayLiteral("GITBOLT_TERM=1"));

    std::vector<char*> envp;
    envp.reserve(static_cast<size_t>(envBytes.size()) + 1);
    for (auto& e : envBytes)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    QByteArray arg1 = QByteArrayLiteral("-i");
    std::vector<char*> argv{shellBytes.data(), arg1.data(), nullptr};

    // forkpty does the openpty + fork + dup2-of-slave-to-stdio in
    // one call. The child gets a fresh controlling terminal.
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, nullptr);
    if (pid < 0) {
        appendPlainText(tr("[gitbolt] forkpty failed: %1")
                            .arg(QString::fromLocal8Bit(strerror(errno))));
        return;
    }

    if (pid == 0) {
        // ----- child: async-signal-safe calls ONLY -----
        if (!cwdBytes.isEmpty() && chdir(cwdBytes.constData()) != 0) {
            // Non-fatal — fall through with whatever cwd we have.
            // (Tested rather than (void)-cast: glibc marks chdir
            // warn_unused_result, which a cast doesn't silence.)
        }
        execve(shellBytes.constData(), argv.data(), envp.data());
        // exec failed — terminate the child.
        _exit(127);
    }

    // ----- parent -----
    masterFd_ = fd;
    childPid_ = pid;

    // Non-blocking reads so we never deadlock the GUI thread when
    // we drain the master fd inside the read notifier.
    int flags = fcntl(masterFd_, F_GETFL, 0);
    fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK);

    readNotifier_ = new QSocketNotifier(masterFd_, QSocketNotifier::Read, this);
    connect(readNotifier_, &QSocketNotifier::activated,
            this, &TerminalWidget::onPtyReadable);

    setPlaceholderText(QString{});
    updatePtySize();
#endif
}

void TerminalWidget::stopShell()
{
    if (readNotifier_) {
        readNotifier_->setEnabled(false);
        readNotifier_->deleteLater();
        readNotifier_ = nullptr;
    }
    if (writeNotifier_) {
        writeNotifier_->setEnabled(false);
        writeNotifier_->deleteLater();
        writeNotifier_ = nullptr;
    }
    writeQueue_.clear();
    pendingOutput_.clear();
#ifdef Q_OS_WIN
    if (conpty_) {
        // Returns at once: ConPtyProcess finishes the hang-up on its
        // own thread, and none of its handlers runs after this.
        conpty_->stop(0);
        conpty_.reset();
        ++conptyGeneration_;
    }
#else
    if (masterFd_ >= 0) {
        ::close(masterFd_);
        masterFd_ = -1;
    }
    if (childPid_ > 0) {
        ::kill(childPid_, SIGHUP);
        // Reap with a brief WNOHANG loop so we don't leak zombies
        // if the user repeatedly restarts the terminal.
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            const pid_t r = ::waitpid(childPid_, &status, WNOHANG);
            if (r == childPid_ || r < 0)
                break;
            usleep(5000);  // 5ms
        }
        childPid_ = -1;
    }
#endif
}

void TerminalWidget::changeDirectory(const QString& path)
{
    if (path.isEmpty())
        return;

    pendingCwd_ = path;

    if (!isRunning())
        return;  // start() will pick up pendingCwd_ when called

#ifdef Q_OS_WIN
    // cmd.exe; /d switches the drive too. Windows paths can't contain
    // '"', so the double quotes need no escaping, but %NAME% still
    // expands inside them: each '%' goes outside the quotes behind a
    // caret, where it can't start a variable name.
    QString safe = QDir::toNativeSeparators(path);
    safe.replace(QLatin1Char('%'), QStringLiteral("\"^%\""));
    const QString cmd = QStringLiteral("cd /d \"%1\"\r").arg(safe);
#else
    // Shell-quote the path with single quotes; escape any embedded
    // single quotes via the standard '\'' sequence.
    QString safe = path;
    safe.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    // Leading space lets the user keep HISTCONTROL=ignorespace if
    // they want to, but the cd will still take effect.
    const QString cmd = QStringLiteral(" cd '%1'\n").arg(safe);
#endif
    writeToPty(toTerminalBytes(cmd));
}

// ---------------------------------------------------------------------------
// PTY I/O
// ---------------------------------------------------------------------------

void TerminalWidget::writeToPty(const QByteArray& bytes)
{
#ifdef Q_OS_WIN
    // A pipe, not a non-blocking fd: the write returns once the bytes
    // are in, and conhost drains its input continuously, so there is
    // nothing to queue.
    if (conpty_ && !bytes.isEmpty())
        conpty_->write(std::string_view(bytes.constData(),
                                        static_cast<size_t>(bytes.size())));
#else
    if (masterFd_ < 0 || bytes.isEmpty())
        return;

    // Preserve ordering: if a backlog exists, everything new goes
    // behind it.
    if (!writeQueue_.isEmpty()) {
        writeQueue_.append(bytes);
        return;
    }

    const int written = writeRaw(bytes);
    if (written < bytes.size() && masterFd_ >= 0) {
        // PTY master buffer is full (~1-4 KB) — typical for a paste,
        // which arrives as one large chunk. Queue the remainder and
        // drain as the fd signals writable; the old code dropped it
        // ("the user can't type fast enough" — true, but paste
        // isn't typing).
        writeQueue_ = bytes.mid(written);
        if (!writeNotifier_) {
            writeNotifier_ = new QSocketNotifier(
                masterFd_, QSocketNotifier::Write, this);
            connect(writeNotifier_, &QSocketNotifier::activated,
                    this, &TerminalWidget::onPtyWritable);
        }
        writeNotifier_->setEnabled(true);
    }
#endif
}

int TerminalWidget::writeRaw(const QByteArray& bytes)
{
#ifdef Q_OS_WIN
    Q_UNUSED(bytes);
    return 0;  // no PTY fd on Windows (writeToPty goes to ConPTY)
#else
    int written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(masterFd_,
                                  bytes.constData() + written,
                                  static_cast<size_t>(bytes.size() - written));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;  // EAGAIN/EWOULDBLOCK or real error — caller queues
        }
        written += static_cast<int>(n);
    }
    return written;
#endif
}

void TerminalWidget::onPtyWritable()
{
    if (masterFd_ < 0) {
        writeQueue_.clear();
        if (writeNotifier_)
            writeNotifier_->setEnabled(false);
        return;
    }
    const int written = writeRaw(writeQueue_);
    writeQueue_.remove(0, written);
    if (writeQueue_.isEmpty() && writeNotifier_)
        writeNotifier_->setEnabled(false);
}

#ifdef Q_OS_WIN
void TerminalWidget::takeConPtyOutput()
{
    if (!conpty_)
        return;
    const std::string output = conpty_->takeOutput();
    if (!output.empty())
        appendOutput(QByteArray(output.data(), static_cast<qsizetype>(output.size())));
}
#endif

void TerminalWidget::onPtyReadable()
{
    if (masterFd_ < 0)
        return;
#ifndef Q_OS_WIN
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(masterFd_, buf, sizeof(buf));
        if (n > 0) {
            appendOutput(QByteArray(buf, static_cast<int>(n)));
            continue;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            // Unrecoverable read error — child probably exited.
            stopShell();
            appendPlainText(tr("\n[gitbolt] shell exited"));
            break;
        }
        // n == 0 — EOF on the master means the slave was closed.
        stopShell();
        appendPlainText(tr("\n[gitbolt] shell exited"));
        break;
    }
#endif
}

// Stream PTY output into the scrollback with just-enough terminal
// emulation to make zsh's line editor (ZLE) display correctly:
//
//   - \r moves the insertion point to column 0 of the CURRENT
//     line (not a new line). ZLE uses \r + \x1b[K on every
//     keystroke to redraw the line it's editing in place.
//   - \x1b[K (EL — erase in line) deletes from the cursor to the
//     end of the current line.
//   - \b moves the cursor one column left without deleting.
//   - Printable characters OVERWRITE the character at the cursor,
//     matching real terminal semantics (a print at column N
//     replaces the char that was at column N; it does NOT push
//     it right). Without this, ZLE's in-place redraws would
//     continually grow the document instead of updating a single
//     line in place.
//
// Everything else we don't understand (SGR colors, cursor
// position queries, application-mode escapes, DEC private mode
// toggles, etc.) is silently dropped. That's enough to let ZLE,
// tab completion, up-arrow history, and in-line backspace edits
// all display correctly on a single-line prompt. Full SGR colors
// and fullscreen-TUI support (vim/htop) remain follow-up work.
void TerminalWidget::appendOutput(const QByteArray& bytes)
{
    // Re-attach the tail of any escape sequence the previous read
    // chopped mid-sequence (see pendingOutput_).
    QByteArray chunk;
    if (!pendingOutput_.isEmpty()) {
        chunk = pendingOutput_ + bytes;
        pendingOutput_.clear();
    } else {
        chunk = bytes;
    }
    QString text = fromTerminalBytes(chunk);

    // Pre-strip OSC (terminal title / OSC-8 hyperlinks) and BEL —
    // neither affects visible layout and they have well-defined
    // terminators, so a single regex is fine. CSI is NOT
    // pre-stripped here: we walk it char-by-char below so we can
    // act on the few sequences that matter.
    static const QRegularExpression oscRe(
        QStringLiteral("\\x1b\\][^\\x07\\x1b]*(\\x07|\\x1b\\\\)"));
    text.remove(oscRe);
    text.remove(QChar(0x07));  // BEL
#ifndef Q_OS_WIN
    // CRLF → LF so "\r\n" doesn't trigger a bogus "move to column 0
    // then newline" sequence on the walk below.
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
#endif

    // Auto-scroll only if the user was already at the bottom —
    // if they scrolled up to read history we don't want to yank
    // them back down on every byte the shell prints.
    QScrollBar* sb = verticalScrollBar();
    const bool wasAtBottom = sb->value() == sb->maximum();

#ifdef Q_OS_WIN
    // ConPTY doesn't stream lines, it paints a screen: it moves the
    // cursor about with absolute positions (a prompt after a blank
    // line arrives as "ESC[7;1H>", a backspace as "ESC[12;3H  ") and
    // expects it to stay put between writes. So the walk resumes at
    // ConPTY's cursor on a screen model (see screenMoveTo), \n is a
    // real line feed, and the cursor sequences below are honored.
    if (screenTop_.isNull())
        startScreen();
    QTextCursor cur = screenCursor_;
#else
    // The write cursor always starts at end-of-document. \r then
    // walks it back to the start of the LAST line (where the
    // shell's current prompt lives), and subsequent printables
    // overwrite in place. This is why user clicks elsewhere in
    // the scrollback don't corrupt future output — we always
    // rebase at End at the top of each batch.
    QTextCursor cur = textCursor();
    cur.movePosition(QTextCursor::End);
#endif

    const int n = static_cast<int>(text.length());
    int i = 0;
    while (i < n) {
        const ushort u = text[i].unicode();

        if (u == '\r') {
            // CR — move to column 0 of the current line (NOT a new line)
            cur.movePosition(QTextCursor::StartOfBlock);
            ++i;
            continue;
        }
        if (u == '\n') {
#ifdef Q_OS_WIN
            screenLineFeed(cur);
#else
            cur.movePosition(QTextCursor::EndOfBlock);
            cur.insertText(QStringLiteral("\n"));
#endif
            ++i;
            continue;
        }
        if (u == 0x08) {
            // Backspace — move left, do not delete
            if (cur.positionInBlock() > 0)
                cur.movePosition(QTextCursor::Left);
            ++i;
            continue;
        }
        if (u == 0x1b) {
            // Escape sequence
            if (i + 1 >= n) {
                // Chunk ended ON the ESC — hold it for the next
                // read instead of dropping it (the rest of the
                // sequence is in flight).
                pendingOutput_ = text.mid(i).toUtf8();
                break;
            }
            const ushort next = text[i + 1].unicode();
            if (next == '[') {
                // CSI: ESC [ <params> <intermediates> <final-byte>
                // where params are 0x30-0x3f, intermediates are
                // 0x20-0x2f, and the final byte is 0x40-0x7e.
                int j = i + 2;
                bool handled = false;
                while (j < n) {
                    const ushort fu = text[j].unicode();
                    if (fu >= 0x40 && fu <= 0x7e) {
#ifdef Q_OS_WIN
                        screenCsi(cur, fu, QStringView(text).mid(i + 2, j - i - 2));
#else
                        // Final byte — act on the few we care about
                        if (fu == 'K') {
                            // EL — erase to end of line (default param)
                            QTextCursor eraseCur = cur;
                            eraseCur.movePosition(QTextCursor::EndOfBlock,
                                                  QTextCursor::KeepAnchor);
                            eraseCur.removeSelectedText();
                        }
                        // SGR (`m`), CUP/HVP, CUU/CUD/CUF/CUB,
                        // DEC private mode set/reset, etc. — drop.
#endif
                        i = j + 1;
                        handled = true;
                        break;
                    }
                    ++j;
                }
                if (!handled) {
                    // Unterminated CSI — its final byte is in the
                    // next chunk. Park everything from the ESC and
                    // resume when it arrives. (Params/intermediates
                    // are pure ASCII, so the UTF-8 round-trip is
                    // lossless.)
                    pendingOutput_ = text.mid(i).toUtf8();
                    i = n;
                }
                continue;
            }
#ifdef Q_OS_WIN
            if (next == ']' && text.indexOf(QChar(0x1b), i + 2) < 0 && n - i < 4096) {
                // An OSC the regex above couldn't remove because its
                // terminator is in the next chunk: conhost retitles the
                // window on every command, so this does happen. Park
                // it like an unterminated CSI.
                pendingOutput_ = text.mid(i).toUtf8();
                break;
            }
#endif
            // ESC followed by a single byte we don't handle
            i += 2;
            continue;
        }
        if (u < 0x20 && u != '\t') {
            // Other C0 control chars — drop silently.
            ++i;
            continue;
        }

        // Printable char: OVERWRITE the character at the cursor
        // (terminal semantics), unless we're already past the end
        // of the current line, in which case a normal insert is
        // equivalent. deleteChar() removes one char to the right
        // of the cursor without moving it, and insertText then
        // inserts and advances — together that's an overwrite.
#ifdef Q_OS_WIN
        // Past the last column: wrap, as the terminal ConPTY paints for
        // would (it goes on at the next row without sending a newline).
        if (cur.positionInBlock() >= screenColumns_) {
            cur.movePosition(QTextCursor::StartOfBlock);
            screenLineFeed(cur);
        }
#endif
        if (!cur.atBlockEnd())
            cur.deleteChar();
        cur.insertText(QString(text[i]));
        ++i;
    }

    // Sync the visible blinking caret with where we just finished
    // writing so the user can see where the shell's cursor is.
    setTextCursor(cur);
#ifdef Q_OS_WIN
    screenCursor_ = cur;
#endif

    if (wasAtBottom) {
        sb->setValue(sb->maximum());
    }
}

#ifdef Q_OS_WIN
// ---------------------------------------------------------------------------
// ConPTY screen model. ConPTY renders the console as a screen of
// screenColumns_ x screenRows_ cells: its rows are the blocks from
// screenTop_ down, everything above is scrollback, and rows below the
// last block are blank until something is painted there. Coordinates
// in escape sequences are 1-based.
// ---------------------------------------------------------------------------

// A fresh screen below the output so far. conhost clears the screen
// first thing, and a clear pushes the old screen into the scrollback,
// minus the blank rows at its bottom.
void TerminalWidget::startScreen()
{
    trimBlankRows();
    QTextCursor end(document());
    end.movePosition(QTextCursor::End);
    if (end.positionInBlock() > 0)
        end.insertText(QStringLiteral("\n"));
    screenTop_ = end;
    // Text written into the top-left cell must not push the marker
    // along with it.
    screenTop_.setKeepPositionOnInsert(true);
    screenCursor_ = end;
}

void TerminalWidget::trimBlankRows()
{
    const QTextBlock last = document()->lastBlock();
    QTextBlock block = last;
    while (block.previous().isValid() && block.text().trimmed().isEmpty())
        block = block.previous();
    if (block == last)
        return;
    QTextCursor cut(document());
    cut.setPosition(block.position() + block.length() - 1);
    cut.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    cut.removeSelectedText();
}

// CUP and friends. Out-of-range coordinates clamp, as on a terminal.
void TerminalWidget::screenMoveTo(QTextCursor& cur, int row, int column)
{
    row = qBound(1, row, screenRows_);
    column = qBound(1, column, screenColumns_);
    const int target = screenTop_.blockNumber() + row - 1;
    const QTextBlock block = document()->findBlockByNumber(target);
    if (block.isValid()) {
        cur.setPosition(block.position());
    } else {
        cur.movePosition(QTextCursor::End);
        cur.insertText(QString(target - cur.blockNumber(), QLatin1Char('\n')));
    }
    placeInRow(cur, column - 1);
}

// LF: down a row, same column (column 0 after the usual CR). Below the
// bottom row the screen scrolls, and its top row joins the scrollback.
void TerminalWidget::screenLineFeed(QTextCursor& cur)
{
    const int column = qMin(cur.positionInBlock(), screenColumns_ - 1);
    if (!cur.movePosition(QTextCursor::NextBlock)) {
        cur.movePosition(QTextCursor::EndOfBlock);
        cur.insertText(QStringLiteral("\n"));
    }
    if (cur.blockNumber() - screenTop_.blockNumber() >= screenRows_)
        screenTop_.movePosition(QTextCursor::NextBlock);
    placeInRow(cur, column);
}

// The CSI sequences ConPTY paints with. SGR (colors), mode switches
// (ESC[?25l hides the cursor), window reports and the rest change
// nothing drawn here.
void TerminalWidget::screenCsi(QTextCursor& cur, ushort finalByte, QStringView params)
{
    for (const QChar c : params) {
        if (c.unicode() < '0' || c.unicode() > ';')
            return;  // private (ESC[?...) or with intermediates: not ours
    }
    const QList<QStringView> args = params.split(u';');
    const auto arg = [&args](qsizetype index) {
        return index < args.size() ? args.at(index).toInt() : 0;
    };
    const int count  = qMax(1, arg(0));  // moves treat 0 as 1
    const int row    = cur.blockNumber() - screenTop_.blockNumber() + 1;
    const int column = cur.positionInBlock() + 1;
    const int length = cur.block().length() - 1;

    switch (finalByte) {
    case 'H':  // CUP
    case 'f':  // HVP
        screenMoveTo(cur, qMax(1, arg(0)), qMax(1, arg(1)));
        break;
    case 'A': screenMoveTo(cur, row - count, column); break;  // CUU
    case 'B': screenMoveTo(cur, row + count, column); break;  // CUD
    case 'C': screenMoveTo(cur, row, column + count); break;  // CUF
    case 'D': screenMoveTo(cur, row, column - count); break;  // CUB
    case 'G': screenMoveTo(cur, row, count); break;           // CHA
    case 'd': screenMoveTo(cur, count, column); break;        // VPA
    case 'K': {  // EL: 0 cursor to end, 1 start to cursor, 2 whole row
        const int mode = arg(0);
        if (mode == 0 || mode == 2) {
            QTextCursor erase = cur;
            erase.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            erase.removeSelectedText();
        }
        if (mode == 1 || mode == 2)
            blankCells(cur, 0, qMin(column, cur.block().length() - 1));
        break;
    }
    case 'X':  // ECH: blank cells from the cursor on, without moving
        blankCells(cur, column - 1, qMin(column - 1 + count, length));
        break;
    case 'J':  // ED
        if (arg(0) == 0) {  // cursor to the end of the screen
            QTextCursor erase = cur;
            erase.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            erase.removeSelectedText();
        } else if (arg(0) == 2) {  // the whole screen; the cursor stays put
            startScreen();
            cur = screenCursor_;
            screenMoveTo(cur, row, column);
        } else if (arg(0) == 3) {  // the scrollback (cmd's `cls` sends it)
            QTextCursor erase(document());
            erase.setPosition(screenTop_.position(), QTextCursor::KeepAnchor);
            erase.removeSelectedText();
        }
        break;
    case 'S':  // SU: the screen scrolls up; the cursor keeps its cell
        for (int k = 0; k < count; ++k) {
            if (!screenTop_.block().next().isValid()) {
                QTextCursor end(document());
                end.movePosition(QTextCursor::End);
                end.insertText(QStringLiteral("\n"));
            }
            screenTop_.movePosition(QTextCursor::NextBlock);
        }
        screenMoveTo(cur, row, column);
        break;
    default:
        break;
    }
}

// A resize makes conhost repaint the screen from its top-left cell. If
// the screen got shorter than the cursor's row, conhost first pushed
// the rows above into the scrollback to keep the cursor on screen, and
// rows below the new bottom are off the screen altogether.
void TerminalWidget::resizeScreen(const QSize& grid)
{
    screenColumns_ = grid.width();
    screenRows_    = grid.height();
    if (screenTop_.isNull())
        return;
    const int cursorRow = screenCursor_.blockNumber() - screenTop_.blockNumber() + 1;
    for (int k = cursorRow - screenRows_; k > 0; --k)
        screenTop_.movePosition(QTextCursor::NextBlock);
    const QTextBlock bottom =
        document()->findBlockByNumber(screenTop_.blockNumber() + screenRows_ - 1);
    if (bottom.isValid() && bottom.next().isValid()) {
        QTextCursor cut(document());
        cut.setPosition(bottom.position() + bottom.length() - 1);
        cut.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        cut.removeSelectedText();
    }
}
#endif // Q_OS_WIN

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void TerminalWidget::keyPressEvent(QKeyEvent* event)
{
    // Cmd+C / Cmd+V handling first — these always mean copy/paste
    // in a GUI app. event->matches() handles the platform-correct
    // modifier (Cmd on macOS, Ctrl elsewhere).
    if (event->matches(QKeySequence::Copy)) {
#ifdef Q_OS_WIN
        // Copy is Ctrl+C here, which is also the only interrupt cmd.exe
        // knows: copy a selection, otherwise send ^C below (Windows
        // Terminal's rule).
        if (textCursor().hasSelection()) {
            QPlainTextEdit::keyPressEvent(event);
            return;
        }
#else
        QPlainTextEdit::keyPressEvent(event);
        return;
#endif
    }
    if (event->matches(QKeySequence::Paste)) {
        QString text = QApplication::clipboard()->text();
#ifdef Q_OS_WIN
        // Under ConPTY only CR is Enter. A LF reaches cmd as Ctrl+J,
        // which its line editor keeps as an ordinary character, so
        // pasted lines would never run. Windows Terminal pastes every
        // line break as a CR, and so does this.
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\r"));
        text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
#endif
        if (!text.isEmpty())
            writeToPty(toTerminalBytes(text));
        return;
    }

    if (!isRunning()) {
        // The shell hasn't been started yet — kick it off so the
        // user can press a key on a fresh tab and start typing.
        start(pendingCwd_);
        if (!isRunning())
            return;
    }

    // On macOS, Qt swaps the physical Ctrl key into Qt::MetaModifier
    // and Cmd into Qt::ControlModifier. The "terminal Ctrl chord"
    // (e.g. Ctrl+C → SIGINT) needs the physical Control key.
#ifdef Q_OS_MAC
    const auto terminalCtrl = Qt::MetaModifier;
#else
    const auto terminalCtrl = Qt::ControlModifier;
#endif

    QByteArray bytes;
    const int key = event->key();

    switch (key) {
        case Qt::Key_Return:
        case Qt::Key_Enter:    bytes = "\r"; break;
        case Qt::Key_Backspace: bytes = "\x7f"; break;  // DEL
        case Qt::Key_Tab:       bytes = "\t"; break;
        case Qt::Key_Escape:    bytes = "\x1b"; break;
        case Qt::Key_Up:        bytes = "\x1b[A"; break;
        case Qt::Key_Down:      bytes = "\x1b[B"; break;
        case Qt::Key_Right:     bytes = "\x1b[C"; break;
        case Qt::Key_Left:      bytes = "\x1b[D"; break;
        case Qt::Key_Home:      bytes = "\x1b[H"; break;
        case Qt::Key_End:       bytes = "\x1b[F"; break;
        case Qt::Key_Delete:    bytes = "\x1b[3~"; break;
        case Qt::Key_PageUp:    bytes = "\x1b[5~"; break;
        case Qt::Key_PageDown:  bytes = "\x1b[6~"; break;
        default:
            if ((event->modifiers() & terminalCtrl) &&
                key >= Qt::Key_A && key <= Qt::Key_Z)
            {
                // Ctrl+A → 0x01 ... Ctrl+Z → 0x1A
                const char c = static_cast<char>(key - Qt::Key_A + 1);
                bytes.append(c);
            } else if ((event->modifiers() & terminalCtrl) &&
                       key == Qt::Key_BracketLeft)
            {
                bytes.append('\x1b');
            } else {
                bytes = toTerminalBytes(event->text());
            }
            break;
    }

    if (!bytes.isEmpty())
        writeToPty(bytes);
}

void TerminalWidget::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    updatePtySize();
}

void TerminalWidget::showEvent(QShowEvent* event)
{
    QPlainTextEdit::showEvent(event);
    if (!autoStarted_) {
        autoStarted_ = true;
        // Start the shell the first time we're shown so the user
        // doesn't have to click into the widget before they can
        // interact with it.
        start(pendingCwd_);
    }
}

QSize TerminalWidget::gridSize() const
{
    // Compute the character grid size from the current font and
    // viewport. Subtract a small fudge for the padding declared
    // in the stylesheet (6px each side).
    const QFontMetrics fm(font());
    const int charW = fm.horizontalAdvance(QLatin1Char('M'));
    const int charH = fm.lineSpacing();
    if (charW <= 0 || charH <= 0)
        return {};

    const int cols = qMax(1, (viewport()->width()  - 12) / charW);
    const int rows = qMax(1, (viewport()->height() - 12) / charH);
    return {cols, rows};
}

void TerminalWidget::updatePtySize()
{
#ifdef Q_OS_WIN
    if (!conpty_)
        return;
    const QSize grid = gridSize();
    if (grid.isValid() && grid != QSize(screenColumns_, screenRows_)
        && conpty_->resize(grid.width(), grid.height()))
        resizeScreen(grid);
#else
    if (masterFd_ < 0)
        return;
    const QSize grid = gridSize();
    if (!grid.isValid())
        return;

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(grid.width());
    ws.ws_row = static_cast<unsigned short>(grid.height());
    ::ioctl(masterFd_, TIOCSWINSZ, &ws);
#endif
}

} // namespace gitbolt::widgets
