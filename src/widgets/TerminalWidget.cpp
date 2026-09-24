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

#ifndef Q_OS_WIN
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
    return childPid_ > 0 && masterFd_ >= 0;
}

void TerminalWidget::start(const QString& workingDirectory)
{
    if (isRunning())
        stopShell();

    if (!workingDirectory.isEmpty())
        pendingCwd_ = workingDirectory;

#ifdef Q_OS_WIN
    // No PTY backend on Windows yet (see the class doc) — say so
    // rather than leaving "Starting shell…" up forever.
    setPlaceholderText(tr("The built-in terminal isn't available on Windows yet."));
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
#ifndef Q_OS_WIN
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

    // Shell-quote the path with single quotes; escape any embedded
    // single quotes via the standard '\'' sequence.
    QString safe = path;
    safe.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    // Leading space lets the user keep HISTCONTROL=ignorespace if
    // they want to, but the cd will still take effect.
    const QString cmd = QStringLiteral(" cd '%1'\n").arg(safe);
    writeToPty(cmd.toLocal8Bit());
}

// ---------------------------------------------------------------------------
// PTY I/O
// ---------------------------------------------------------------------------

void TerminalWidget::writeToPty(const QByteArray& bytes)
{
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
}

int TerminalWidget::writeRaw(const QByteArray& bytes)
{
#ifdef Q_OS_WIN
    Q_UNUSED(bytes);
    return 0;  // no PTY on Windows
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
    QString text = QString::fromLocal8Bit(chunk);

    // Pre-strip OSC (terminal title / OSC-8 hyperlinks) and BEL —
    // neither affects visible layout and they have well-defined
    // terminators, so a single regex is fine. CSI is NOT
    // pre-stripped here: we walk it char-by-char below so we can
    // act on the few sequences that matter.
    static const QRegularExpression oscRe(
        QStringLiteral("\\x1b\\][^\\x07\\x1b]*(\\x07|\\x1b\\\\)"));
    text.remove(oscRe);
    text.remove(QChar(0x07));  // BEL
    // CRLF → LF so "\r\n" doesn't trigger a bogus "move to column 0
    // then newline" sequence on the walk below.
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    // Auto-scroll only if the user was already at the bottom —
    // if they scrolled up to read history we don't want to yank
    // them back down on every byte the shell prints.
    QScrollBar* sb = verticalScrollBar();
    const bool wasAtBottom = sb->value() == sb->maximum();

    // The write cursor always starts at end-of-document. \r then
    // walks it back to the start of the LAST line (where the
    // shell's current prompt lives), and subsequent printables
    // overwrite in place. This is why user clicks elsewhere in
    // the scrollback don't corrupt future output — we always
    // rebase at End at the top of each batch.
    QTextCursor cur = textCursor();
    cur.movePosition(QTextCursor::End);

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
            cur.movePosition(QTextCursor::EndOfBlock);
            cur.insertText(QStringLiteral("\n"));
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
        if (!cur.atBlockEnd())
            cur.deleteChar();
        cur.insertText(QString(text[i]));
        ++i;
    }

    // Sync the visible blinking caret with where we just finished
    // writing so the user can see where the shell's cursor is.
    setTextCursor(cur);

    if (wasAtBottom) {
        sb->setValue(sb->maximum());
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void TerminalWidget::keyPressEvent(QKeyEvent* event)
{
    // Cmd+C / Cmd+V handling first — these always mean copy/paste
    // in a GUI app. event->matches() handles the platform-correct
    // modifier (Cmd on macOS, Ctrl elsewhere).
    if (event->matches(QKeySequence::Copy)) {
        QPlainTextEdit::keyPressEvent(event);
        return;
    }
    if (event->matches(QKeySequence::Paste)) {
        const QString text = QApplication::clipboard()->text();
        if (!text.isEmpty())
            writeToPty(text.toLocal8Bit());
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
                bytes = event->text().toLocal8Bit();
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

void TerminalWidget::updatePtySize()
{
    if (masterFd_ < 0)
        return;
#ifndef Q_OS_WIN
    // Compute the character grid size from the current font and
    // viewport. Subtract a small fudge for the padding declared
    // in the stylesheet (6px each side).
    const QFontMetrics fm(font());
    const int charW = fm.horizontalAdvance(QLatin1Char('M'));
    const int charH = fm.lineSpacing();
    if (charW <= 0 || charH <= 0)
        return;

    const int cols = qMax(1, (viewport()->width()  - 12) / charW);
    const int rows = qMax(1, (viewport()->height() - 12) / charH);

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ::ioctl(masterFd_, TIOCSWINSZ, &ws);
#endif
}

} // namespace gitbolt::widgets
