#include "widgets/TerminalWidget.h"

#include "git/GitProcess.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSocketNotifier>
#include <QStringList>
#include <QTextBoundaryFinder>
#include <QTextBlock>
#include <QTextCursor>
#include <QTimer>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef Q_OS_WIN
#include "widgets/ConPtyProcess.h"

#include <QDir>
#include <QFileInfo>
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
#include <libproc.h>   // proc_name
#include <sys/param.h> // MAXCOMLEN
#include <util.h>      // forkpty
#elif defined(Q_OS_LINUX)
#include <pty.h>       // forkpty (glibc; linked from libutil before 2.34)
#else
#include <libutil.h>   // forkpty (FreeBSD)
#endif
#endif

#include <algorithm>
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

// The longest a CSI gets before its final byte, ESC and '[' included: a
// real one is a handful of characters, an SGR with every attribute and
// both colors a few dozen. Anything longer is dropped (see appendOutput).
constexpr int kMaxCsiLength = 256;

// An OSC a read cuts short waits for the next read only if it is
// shorter than this. An OSC 8 link carries a URL, which can run to a
// couple of KB.
constexpr int kMaxPendingOsc = 4096;

// The most digits a number in a CSI is read with, and what one with more
// is taken for: more than any screen has rows or columns (see
// TerminalWidget::csi).
constexpr qsizetype kMaxCountDigits = 5;
constexpr int kMaxCount = 99999;

// How often a switch that waits for a command to end looks again, and
// how long it waits after the command's end and after the Ctrl+C that
// drops the shell's line (see TerminalWidget::switchWhenIdle).
constexpr int kSwitchCheckMs = 250;
// How many of those ticks the Ctrl+C waits at most for the shell to read
// a line typed ahead (see TerminalWidget::switchWhenIdle): a slow prompt
// holds it up, but a line nothing reads mustn't hold the switch off.
constexpr int kMaxUnreadWaits = 8;

#ifndef Q_OS_WIN
// The most onPtyReadable() reads, and draws in one appendOutput(), before
// it goes back to the event loop. Drawing costs by the line, not the
// byte: tens of milliseconds for ordinary lines, a few hundred for a
// slice of `yes`'s two-byte ones.
constexpr qsizetype kMaxReadPerWake = 64 * 1024;
#endif

// Text -> the bytes the shell's terminal speaks. ConPTY speaks UTF-8
// whatever the ANSI code page, and the ANSI code page is what Qt's
// "local 8-bit" means on Windows; on Unix it means UTF-8 already. (The
// other way is decoder_, UTF-8 on both.)
QByteArray toTerminalBytes(const QString& text)
{
#ifdef Q_OS_WIN
    return text.toUtf8();
#else
    return text.toLocal8Bit();
#endif
}

#ifndef Q_OS_WIN
// Whether `text` can be typed into the shell as it is: a control
// character would act as a key there (Ctrl+C, Enter, Tab).
bool typeable(const QString& text)
{
    return std::none_of(text.begin(), text.end(), [](QChar c) {
        return c.unicode() < 0x20 || c.unicode() == 0x7f;
    });
}

// The shells that take sh's `export` and `unset`, and its quoting, by
// the name of their program.
bool isShFamily(const QString& name)
{
    static const QStringList names{
        QStringLiteral("sh"),  QStringLiteral("ash"),   QStringLiteral("dash"),
        QStringLiteral("bash"), QStringLiteral("ksh"),  QStringLiteral("ksh93"),
        QStringLiteral("mksh"), QStringLiteral("oksh"), QStringLiteral("yash"),
        QStringLiteral("zsh")};
    return names.contains(name);
}

// The shells that take csh's `setenv` and `unsetenv`.
bool isCshFamily(const QString& name)
{
    return name == QLatin1String("csh") || name == QLatin1String("tcsh");
}

// The shells a Ctrl+C at the prompt drops the line of. A program an rc
// file `exec`s in the shell's place (tmux, screen, ssh) holds the
// terminal without counting as a command (see commandRunning), and would
// pass the Ctrl+C on to whatever runs in it: a dev server, a build.
bool dropsLineOnCtrlC(const QString& name)
{
    return isShFamily(name) || isCshFamily(name) || name == QLatin1String("fish");
}

// `text` as one word of a line typed into the shell whose program is
// `shell`: in single quotes, with a quote in it as '\'' (in fish \', and
// a backslash \\). csh substitutes history inside them too, so a `!`
// goes as \!.
//
// In sh and csh a character outside ASCII goes as printf's octal escapes
// of its bytes, in a command substitution. Their line editors, bash's
// and tcsh's, take a byte with its high bit set for a Meta key where the
// locale has no such characters, as where none is set (GitBolt started
// from the Finder or the Dock has none to pass on): typed as it was,
// "café" came out "caf". There the characters that mean something to
// printf, or to the quotes around it, are escaped too.
QString shellWord(const QString& text, const QString& shell)
{
    if (shell == QLatin1String("fish")) {
        QString word = text;
        word.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        word.replace(QLatin1Char('\''), QStringLiteral("\\'"));
        return QLatin1Char('\'') + word + QLatin1Char('\'');
    }
    const bool csh = isCshFamily(shell);
    const bool ascii = std::all_of(text.begin(), text.end(),
                                   [](QChar c) { return c.unicode() < 0x80; });
    if (!ascii && (csh || isShFamily(shell))) {
        QString format;
        for (const char byte : toTerminalBytes(text)) {
            const auto code = static_cast<unsigned char>(byte);
            if (code >= 0x80 || QByteArrayLiteral("'\"\\%!$`").contains(byte))
                format += QStringLiteral("\\%1").arg(uint(code), 3, 8, QLatin1Char('0'));
            else
                format += QLatin1Char(byte);
        }
        return csh ? QStringLiteral("\"`printf '%1'`\"").arg(format)
                   : QStringLiteral("\"$(printf '%1')\"").arg(format);
    }
    QString word = text;
    word.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    if (csh)
        word.replace(QLatin1Char('!'), QStringLiteral("\\!"));
    return QLatin1Char('\'') + word + QLatin1Char('\'');
}
#endif

// Whether the shell can be sent `cd <path>` as typed text. A control
// character in the path would act as a key (Tab completes, Enter runs
// the line half typed), and git takes one in a submodule's path, which
// a cloned repository chooses; the shell is started there instead.
// Windows names can't hold one.
bool cdTypeable(const QString& path)
{
#ifdef Q_OS_WIN
    Q_UNUSED(path);
    return true;
#else
    return typeable(path);
#endif
}

// What appendOutput draws as text: everything but the C0 controls,
// though a tab is text too.
bool isText(QChar c)
{
    return c.unicode() >= 0x20 || c == QLatin1Char('\t');
}

// A CSI's final byte, which ends it (and says what it does).
bool isCsiFinal(QChar c)
{
    return c.unicode() >= 0x40 && c.unicode() <= 0x7e;
}

// Write `run` at the cursor the way a terminal does: it replaces a
// character under the cursor for each of its own, a whole one (a
// surrogate pair, a letter with its combining marks), and past the
// end of the line it extends it. One insertion however long the run,
// where a deleteChar() and an insertText() per character made big
// outputs crawl.
void overwrite(QTextCursor& cur, QStringView run)
{
    for (qsizetype k = 0; k < run.size() && !cur.atBlockEnd(); ++k)
        cur.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
    cur.insertText(run.toString());
}

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

    switchTimer_ = new QTimer(this);
    switchTimer_->setInterval(kSwitchCheckMs);
    connect(switchTimer_, &QTimer::timeout, this, [this] {
        waitForTick_ = false;  // the tick waited for (see switchWhenIdle)
        switchWhenIdle();
    });
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
    shellGitDir_ = gitDir_;
    shellCwd_ = pendingCwd_;

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
    // The repository is the one the shell starts in, as for every git
    // GitBolt runs (git::GitProcess::repositoryEnvVars).
    for (const auto& name : git::GitProcess::repositoryEnvVars())
        info.removed.push_back(QString::fromStdString(name).toStdWString());
    if (!gitDir_.isEmpty())
        info.environment.emplace_back(L"GIT_DIR", QDir::toNativeSeparators(gitDir_).toStdWString());
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
    shellName_ = QFileInfo(shell).fileName();
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
    // The repository is the one the shell starts in, as for every git
    // GitBolt runs: git's variables that say otherwise are left out
    // (git::GitProcess::repositoryEnvVars).
    QList<QByteArray> dropped{QByteArrayLiteral("TERM="), QByteArrayLiteral("GITBOLT_TERM=")};
    for (const auto& name : git::GitProcess::repositoryEnvVars())
        dropped.append(QByteArray::fromStdString(name) + '=');
    QList<QByteArray> envBytes;
    for (char** e = hostEnviron(); e && *e; ++e) {
        const QByteArray entry(*e);
        if (std::any_of(dropped.begin(), dropped.end(),
                        [&entry](const QByteArray& prefix) { return entry.startsWith(prefix); }))
            continue;
        envBytes.append(entry);
    }
    envBytes.append(QByteArrayLiteral("TERM=xterm-256color"));
    envBytes.append(QByteArrayLiteral("GITBOLT_TERM=1"));
    if (!gitDir_.isEmpty())
        envBytes.append("GIT_DIR=" + QFile::encodeName(gitDir_));

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
    shellName_ = QFile::decodeName(shellBytes.mid(shellBytes.lastIndexOf('/') + 1));
    screenCursor_ = QTextCursor();  // the new shell writes below

    // Non-blocking reads so we never deadlock the GUI thread when
    // we read the master fd a slice at a time inside the read notifier.
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
    droppingCsi_ = false;
    decoder_.resetState();
    switchTimer_->stop();
    lineEmpty_ = true;  // a new shell's line holds nothing
    commandRan_ = false;
    waitForTick_ = false;
    unreadWaits_ = 0;
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

void TerminalWidget::changeRepository(const QString& path, const QString& gitDir)
{
    if (path.isEmpty())
        return;
    // The repository the shell is in, or on its way to (a switch that
    // waits goes to the latest), opened again: Recent, the dashboard, a
    // second launch handing its path over. There is nothing to type, and
    // a Ctrl+C would only drop what the user has on the line.
    if (path == pendingCwd_ && gitDir == gitDir_)
        return;
    gitDir_ = gitDir;
    pendingCwd_ = path;
    // Back to where the shell is while a switch away from it waits: the
    // switch is off, or its Ctrl+C would drop the line for nothing.
    if (isRunning() && path == shellCwd_ && gitDir == shellGitDir_) {
        cancelSwitch();
        return;
    }
    // A shell not started yet takes both at its start (showEvent).
    if (isRunning())
        switchWhenIdle();
}

// The shell follows the repository opened: a `cd` typed into it at its
// prompt, with a new GIT_DIR (a bare repository opened or left) ahead
// of it on its line (see gitDirCommand). The shell keeps its variables,
// its history and its jobs. A new GIT_DIR used to start it again, which
// hung up what it ran: its jobs, one stopped with Ctrl+Z (the editor of
// a `git commit`) or run in the background, which leave it at its
// prompt (see commandRunning), and on Windows the command running too.
// Typed while a command runs, the line would go to the command (an
// editor, a pager, a REPL, `cat`), so it waits for the prompt, which is
// looked for as the shell's output arrives (the prompt is output) and
// every so often in case none does; meanwhile a note says so, below the
// command's output. A `cd` alone, between two repositories with work
// trees, used to be typed at once, into whatever ran; it now waits as a
// GIT_DIR does. A switch meanwhile changes only where the shell goes. A
// shell gitDirCommand has no line for is still started again for a new
// GIT_DIR, once the command has ended, and its jobs are hung up with
// it; so is one whose folder a `cd` can't be typed for (cdTypeable).
//
// The prompt the line is typed at isn't always an empty one. A line the
// user had partly typed there took the switch's line on its end, and
// the Enter after it: the line ran, the `cd` among its arguments (a
// half-typed `rm -rf build` removed the build folder, and the
// repository being opened with it). Keys typed ahead while a command
// ran did the same once it ended. And at a prompt that continues a line
// (an open quote, a heredoc, a trailing backslash, tcsh's `foreach?`)
// the switch went into the string, the heredoc or the loop. A GIT_DIR
// then stayed as it was, yet shellGitDir_ took it for changed, so every
// switch after that typed only the `cd`, and git in the shell went on
// at the old bare repository (a `git branch -D` there deleted its
// branch). None of that shows in what the shell prints, so the line is
// dropped first, with a Ctrl+C, as a shell started again dropped it.
// Every shell drops its line on one, a continued one too, in whatever
// mode its line editor is (Ctrl+E Ctrl+U, say, left the line where it
// was in vi's command mode), and the terminal drops with it the keys
// typed ahead that the shell hasn't read. The one line known to be
// empty is one nothing has gone to since the shell started, since such
// a Ctrl+C, or since a switch typed its own whole line (lineEmpty_),
// and that gets no Ctrl+C: switching repositories without typing into
// the Console looks as it always did, a `cd` at the prompt. Otherwise
// the Ctrl+C shows, as a ^C (bash, sh) or the line left as it was
// (zsh), and a new prompt for the switch's line.
// The Ctrl+C goes only to a shell that drops its line on one: a program
// an rc file `exec`ed in the shell's place (tmux, screen, ssh), which
// commandRunning can't see into, would pass it on to what runs there,
// so with something on its line that one is started again instead (a
// tmux client detached, not what its session runs). A switch back to
// where the shell is, while one away from it waits, is called off.
//
// The line goes in at the timer's next tick rather than with the
// Ctrl+C, which is a signal: on Linux a shell that reads a whole line
// at a time (dash) can read it before the signal reaches it (a read of
// the terminal waits for input under way before it looks for one), and
// then drop it with the line before. Anything sent to the shell
// meanwhile can leave a line again (see noteTyped), and then another
// Ctrl+C goes first (lineEmpty_). A command that runs meanwhile can't:
// the Ctrl+C has emptied the line and the terminal's input, and only
// what is sent after it puts anything there. One taken for a line again
// used to send a Ctrl+C at every prompt for good where a prompt hook
// runs a program (bash's PROMPT_COMMAND, zsh's precmd): the Ctrl+C has
// the shell show its prompt again, which runs the hook, the shell's
// output is read while the hook holds the terminal, and at the tick the
// line counted as not empty. And the PTY is read before the Ctrl+C,
// since one makes it drop what it holds unread (on macOS, a background
// job's last lines).
//
// Once a command the switch waited for has ended, a switch that drops
// the line or starts the shell again waits a tick more (commandRan_)
// before it goes on. A command typed ahead, with its Enter, then gets
// to start, and the switch waits for it in turn: a Ctrl+C sent as soon
// as the shell took its terminal back reached zsh before it had read
// the line, which it dropped unread, though the terminal had shown it
// typed. A tick isn't always enough: a prompt that runs something in
// itself (`$(git …)` in PS1, precmd or PROMPT_COMMAND, as git prompts
// and starship do) keeps the shell from reading the line for as long as
// that takes, and it runs in the shell's own process group, so no
// command is seen running. So the Ctrl+C also waits while the terminal
// holds a whole line the shell hasn't read (lineUnread), a few ticks at
// most. And on Windows conhost has painted the program's last output by
// then (see below).
//
// On Windows there is no note: conhost paints its screen with absolute
// cursor moves, and would paint over it. And cmd is started again for a
// new GIT_DIR rather than typed it (see gitDirCommand), and for a `cd`
// too unless nothing can be on its line (lineEmpty_): a Ctrl+C through
// ConPTY ends cmd's read of its line, but conhost leaves the keys typed
// ahead in the console's input (only Ctrl+Break flushes it), and ends
// no read while one of its popups (F7's history) is up, so the `cd`
// would go on their end. Starting cmd again closes its pseudo console.
// What the console had not yet painted went with it, and what
// ConPtyProcess had read but not handed over (stopShell): cmd started
// again as soon as the program was seen to end lost its last lines, a
// `git push`'s error say. Hence the tick, and the output taken first.
void TerminalWidget::switchWhenIdle()
{
    if (!commandRunning()) {
        if (waitForTick_)
            return;
        const bool sameGitDir = gitDir_ == shellGitDir_;
        const QString gitDirLine = sameGitDir ? QString() : gitDirCommand();
        // The line typed into the shell, rather than the shell started
        // again; cmd only where nothing can be on its line (see above).
        bool typeIt = (sameGitDir || !gitDirLine.isNull()) && cdTypeable(pendingCwd_);
#ifdef Q_OS_WIN
        typeIt = typeIt && lineEmpty_;
#else
        // And a Ctrl+C only to a shell it drops the line of.
        typeIt = typeIt && (lineEmpty_ || dropsLineOnCtrlC(shellProgram()));
#endif
        if (commandRan_ && !(typeIt && lineEmpty_)) {
            commandRan_ = false;
            waitForTick_ = true;
            switchTimer_->start();  // a whole tick from now
            return;
        }
        if (typeIt) {
#ifndef Q_OS_WIN
            if (!lineEmpty_ && lineUnread() && unreadWaits_ < kMaxUnreadWaits) {
                ++unreadWaits_;
                waitForTick_ = true;
                switchTimer_->start();
                return;
            }
            if (!lineEmpty_) {
                unreadWaits_ = 0;
                takePtyOutput();
                writeToPty(QByteArrayLiteral("\x03"));
                lineEmpty_ = true;
                waitForTick_ = true;
                switchTimer_->start();
                return;
            }
#endif
            commandRan_ = false;
            if (switchTimer_->isActive()) {
                switchTimer_->stop();
                // The note goes too, or it stays below the prompt: the
                // shell's output went on above it.
                removeSwitchNote();
            }
            // A whole line, so the shell's is empty again once it has
            // read it: lineEmpty_ stays set.
            writeToPty(toTerminalBytes(gitDirLine + cdCommand(pendingCwd_)));
            shellGitDir_ = gitDir_;
            shellCwd_ = pendingCwd_;
            return;
        }
        // Closing the PTY loses what it holds unread, and a command that
        // has just ended, the one waited for above say, can have left
        // its last lines there (a summary, an error): it wrote them all
        // before the shell took the terminal back, which the check above
        // saw, but they are only drawn once read. So the PTY is read once
        // more first. A slice at most, as on any wake: much more than a
        // PTY holds unread (1 KiB on macOS), and a background job that
        // goes on writing doesn't keep it reading. On Windows what goes
        // is what ConPtyProcess has read and not yet handed over, so
        // that is drawn first.
#ifdef Q_OS_WIN
        takeConPtyOutput();
#else
        takePtyOutput();
#endif
        start(pendingCwd_);
        return;
    }
    commandRan_ = true;
    if (switchTimer_->isActive())
        return;
    switchTimer_->start();
#ifndef Q_OS_WIN
    // The note goes at the end, which can be just where the shell's
    // cursor is. The cursor stays put, so the command's output goes on
    // above the note, and a new shell starts on the row below it. Its
    // anchor goes along all the same: left there, it selected the note,
    // and the command's next output wrote over it.
    screenCursor_.setKeepPositionOnInsert(true);
    appendPlainText(switchNote() + QLatin1Char('\n'));
    screenCursor_.setKeepPositionOnInsert(false);
    screenCursor_.clearSelection();
#endif
}

// On Unix the terminal's foreground process group: an interactive shell
// runs each command in a group of its own, hands the terminal to it, and
// takes it back for the prompt. The shell's own group is its pid, as
// forkpty made it a session leader. What the shell runs in itself (a
// builtin such as `read`, a loop between its commands) is not told
// apart from the prompt, nor is anything a shell without job control
// runs. The shell's jobs, one stopped with Ctrl+Z (a pager, the editor
// of a `git commit`) or run in the background, leave it at its prompt,
// and that is right here: the line typed goes to the shell, and the
// jobs keep the GIT_DIR they started with. (Only a shell started again
// would end them, as closing a terminal window does: a stopped one is
// sent SIGHUP and SIGCONT once the shell is gone, and zsh hangs up its
// running ones itself.)
//
// Windows: ConPTY doesn't say. A console has no process groups, and its
// process list is only for a process attached to it (attaching GitBolt,
// as interruptTree in git/GitProcess.cpp does to send git a Ctrl-C,
// swaps out GitBolt's Ctrl-C handlers). What cmd waits for is a console
// program it runs, so a child of cmd's running one is taken for a
// command (ConPtyProcess::hasConsoleChild). That leaves out cmd's
// builtins (`type`, `for`), and a batch file between the programs it
// runs: a `cd` typed meanwhile waits in the console's input until cmd
// reads it at its prompt, unless something else reads the console
// first (`pause`, `set /p`, a program the batch file runs next), and cmd
// started again for a new GIT_DIR (see gitDirCommand) ends them. And it
// counts a console program cmd `start`ed in a window of its own, which
// the switch then waits for.
#ifndef Q_OS_WIN
// Whether the shell's terminal holds input it hasn't read: a whole line,
// with its Enter, while the terminal is in its line mode, as it is while
// the shell runs something and before its line editor takes the
// terminal for the prompt; a line partly typed doesn't count there. Read
// from the terminal's own side, which the master's FIONREAD isn't.
bool TerminalWidget::lineUnread() const
{
    if (masterFd_ < 0)
        return false;
    const char* name = ::ptsname(masterFd_);
    if (!name)
        return false;
    const int fd = ::open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    int unread = 0;
    const bool ok = ::ioctl(fd, FIONREAD, &unread) == 0;
    ::close(fd);
    return ok && unread > 0;
}
#endif

QString TerminalWidget::switchNote()
{
    return tr("[gitbolt] the shell will switch to this repository "
              "when the running command ends");
}

// The note of a switch that waited, taken away. It is the last row but
// one, below it the row a new shell would have started on: the
// command's output goes on above it (see switchWhenIdle).
void TerminalWidget::removeSwitchNote()
{
    QTextCursor last(document());
    last.movePosition(QTextCursor::End);
    last.movePosition(QTextCursor::PreviousBlock, QTextCursor::KeepAnchor);
    if (last.block().text() == switchNote()) {
        last.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
        last.removeSelectedText();
    }
}

void TerminalWidget::cancelSwitch()
{
    if (switchTimer_->isActive()) {
        switchTimer_->stop();
        removeSwitchNote();
    }
    commandRan_ = false;
    waitForTick_ = false;
    unreadWaits_ = 0;
}

bool TerminalWidget::commandRunning() const
{
#ifdef Q_OS_WIN
    return conpty_ && conpty_->hasConsoleChild();
#else
    if (!isRunning())
        return false;
    const pid_t group = ::tcgetpgrp(masterFd_);
    return group > 0 && group != childPid_;
#endif
}

// GIT_DIR goes ahead of the `cd` (cdCommand) on its line, so the
// shell takes both in one go, in the syntax of the shell that runs: sh's
// `export` and `unset` in the shells that have them, csh's `setenv` and
// `unsetenv`, and fish's `set`. Null for any other (nushell, PowerShell,
// or a program an rc file `exec`ed, such as tmux), and for a path with a
// control character, which would act as a key.
//
// And null on Windows, where cmd is started again instead, once the
// console program it runs has ended (commandRunning) and what it printed
// last is drawn (see switchWhenIdle): the new cmd has GIT_DIR in its
// environment, and can't be left with the old one. Typed, the change
// needs cmd's line dropped first, as in switchWhenIdle, and whether a
// Ctrl+C sent through ConPTY drops a line cmd continues (`More?`, after
// a `(` or a trailing caret) and the keys typed ahead in the console's
// input isn't something to count on untried: a change typed over either
// was lost, and taken for made. cmd has no jobs to lose, only its own
// variables and its history.
QString TerminalWidget::gitDirCommand() const
{
#ifdef Q_OS_WIN
    return {};
#else
    if (!typeable(gitDir_) || !typeable(pendingCwd_))
        return {};
    // Quoted as cdCommand quotes, and with its leading space.
    const QString shell = shellProgram();
    const QString value = shellWord(gitDir_, shell);
    if (shell == QLatin1String("fish")) {
        return gitDir_.isEmpty() ? QStringLiteral(" set -e GIT_DIR;")
                                 : QStringLiteral(" set -gx GIT_DIR %1;").arg(value);
    }
    if (isCshFamily(shell)) {
        return gitDir_.isEmpty() ? QStringLiteral(" unsetenv GIT_DIR;")
                                 : QStringLiteral(" setenv GIT_DIR %1;").arg(value);
    }
    if (isShFamily(shell)) {
        return gitDir_.isEmpty() ? QStringLiteral(" unset GIT_DIR;")
                                 : QStringLiteral(" export GIT_DIR=%1;").arg(value);
    }
    return {};
#endif
}

QString TerminalWidget::cdCommand(const QString& path) const
{
#ifdef Q_OS_WIN
    // cmd.exe; /d switches the drive too. Windows paths can't contain
    // '"', so the double quotes need no escaping, but %NAME% still
    // expands inside them: each '%' goes outside the quotes behind a
    // caret, where it can't start a variable name.
    QString safe = QDir::toNativeSeparators(path);
    safe.replace(QLatin1Char('%'), QStringLiteral("\"^%\""));
    return QStringLiteral("cd /d \"%1\"\r").arg(safe);
#else
    // Shell-quoted, for the shell that runs (see shellWord). Leading
    // space lets the user keep HISTCONTROL=ignorespace if they want
    // to, but the cd will still take effect.
    return QStringLiteral(" cd %1\n").arg(shellWord(path, shellProgram()));
#endif
}

#ifndef Q_OS_WIN
// The process's name, where the system tells it: an rc file can `exec`
// another shell (fish from a .bashrc) or something else (tmux), which
// $SHELL doesn't say. Elsewhere the name of the shell started.
QString TerminalWidget::shellProgram() const
{
#if defined(Q_OS_MACOS)
    char name[2 * MAXCOMLEN + 1] = {};
    if (proc_name(childPid_, name, sizeof(name)) > 0)
        return QFile::decodeName(name);
#elif defined(Q_OS_LINUX)
    QFile comm(QStringLiteral("/proc/%1/comm").arg(childPid_));
    if (comm.open(QIODevice::ReadOnly))
        return QFile::decodeName(comm.readAll().trimmed());
#endif
    return shellName_;
}
#endif

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
    if (!output.empty()) {
        const QByteArray bytes(output.data(), static_cast<qsizetype>(output.size()));
        outputRead(bytes);
        appendOutput(bytes);
    }
}
#endif

void TerminalWidget::onPtyReadable()
{
    if (masterFd_ < 0)
        return;
#ifndef Q_OS_WIN
    // Read up to kMaxReadPerWake, draw it in one go, then go back to the
    // event loop: the notifier fires again at once while there is more.
    // A shell that writes faster than the widget draws (`yes`, a `cat`
    // of a big file) keeps the PTY full, and reading it until it ran dry
    // kept the GUI thread here for as long as the flood lasted, with
    // nothing repainted and Ctrl+C stuck behind it. In between, the full
    // PTY holds the shell back, as ConPtyProcess's cap does on Windows.
    if (!takePtyOutput()) {
        stopShell();
        appendPlainText(tr("\n[gitbolt] shell exited"));
    } else if (switchTimer_->isActive()) {
        switchWhenIdle();  // the command's last output, or the prompt after it
    }
#endif
}

#ifndef Q_OS_WIN
bool TerminalWidget::takePtyOutput()
{
    QByteArray slice(kMaxReadPerWake, Qt::Uninitialized);
    qsizetype got = 0;
    bool exited = false;
    while (got < slice.size()) {
        const ssize_t n = ::read(masterFd_, slice.data() + got,
                                 static_cast<size_t>(slice.size() - got));
        if (n > 0) {
            got += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        // EOF on the master (the slave was closed) or a read error:
        // the child has probably exited. What came before it is
        // drawn first.
        exited = true;
        break;
    }
    slice.truncate(got);
    if (!slice.isEmpty()) {
        outputRead(slice);
        appendOutput(slice);
    }
    return !exited;
}
#endif

// Stream PTY output into the scrollback with just-enough terminal
// emulation for the shells' line editors (zsh's ZLE, bash's readline)
// and progress meters to display correctly:
//
//   - The shell's cursor stays where a read leaves it
//     (screenCursor_), and the next read goes on from there.
//   - \r moves the insertion point to column 0 of the CURRENT
//     line (not a new line). ZLE uses \r + \x1b[K on every
//     keystroke to redraw the line it's editing in place.
//   - \b moves the cursor one column left without deleting.
//   - The CSI sequences that move or erase within the row: CUF, CUB,
//     CHA, HPA, EL, ECH, ICH and DCH (see csi()). zsh moves along
//     the line it edits with CUF and CUB, bash inserts and deletes
//     with ICH and DCH.
//   - Printable characters OVERWRITE the character at the cursor,
//     matching real terminal semantics (a print at column N
//     replaces the char that was at column N; it does NOT push
//     it right). Without this, ZLE's in-place redraws would
//     continually grow the document instead of updating a single
//     line in place. A run of them, up to the next control
//     character, is written in one go (see overwrite()).
//
// Everything else we don't understand (SGR colors, cursor
// position queries, application-mode escapes, DEC private mode
// toggles, etc.) is silently dropped. That's enough to let ZLE,
// tab completion, up-arrow history, and in-line edits all display
// correctly on a single-line prompt. Full SGR colors and
// fullscreen-TUI support (vim/htop) remain follow-up work.
//
// What remains approximate on macOS and Linux: this is a model of the
// cursor's row, not of a screen. A row doesn't wrap at the PTY's width
// (a longer one stays one row, scrolled sideways), and the moves
// between rows (CUP, CUU, CUD) and ED are dropped. A line editor
// assumes the terminal wraps: zsh redraws a command line longer than
// the terminal is wide with those moves, so such a line can show wrong
// while it is edited. Windows runs the screen model below instead.
void TerminalWidget::appendOutput(const QByteArray& bytes)
{
    // Decode (the decoder keeps a character the read cut in two for
    // the next call, see decoder_), then re-attach the tail of any
    // escape sequence the previous read chopped mid-sequence (see
    // pendingOutput_).
    QString text = decoder_.decode(bytes);
    if (!pendingOutput_.isEmpty()) {
        text.prepend(pendingOutput_);
        pendingOutput_.clear();
    }

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
    // The walk resumes where the last read left the shell's cursor:
    // git's progress meter ends each update with a CR for the next one
    // to write over, and a line editor sends each arrow key as a read
    // of its own. Starting every read at the end of the text piled the
    // meter's updates up on one row. screenCursor_ is the widget's own
    // cursor, not the caret, so a click in the scrollback doesn't move
    // it. A new shell's first read starts at the end of the text.
    if (screenCursor_.isNull()) {
        screenCursor_ = QTextCursor(document());
        screenCursor_.movePosition(QTextCursor::End);
    }
    QTextCursor cur = screenCursor_;
#endif

    // The whole read is one edit: the document lays out the lines it
    // changed and trims the scrollback once, at the end, rather than
    // after every run of text and every line feed.
    QTextCursor edit(document());
    edit.beginEditBlock();

    const int n = static_cast<int>(text.length());
    int i = 0;
    if (droppingCsi_) {
        // The rest of a CSI too long to be one (see below), as far as
        // its final byte, which goes with it.
        while (i < n && !isCsiFinal(text[i]))
            ++i;
        if (i < n) {
            ++i;
            droppingCsi_ = false;
        }
    }
    while (i < n) {
        const ushort u = text[i].unicode();

        if (u == '\r') {
            // CR — move to column 0 of the current line (NOT a new line)
#ifdef Q_OS_WIN
            cur.movePosition(QTextCursor::StartOfBlock);
#else
            // Of the current screen row, where the line is longer than
            // the terminal: its rows don't wrap here, but the shell's
            // do, so they are one block, a row every screenColumns_
            // cells. readline, at the right margin, sends "<key> \r" to
            // get onto the next row; to the block's start, the keys
            // after it overwrote the prompt and the command. A cursor
            // just past the last column hasn't wrapped yet (terminals
            // put that off until the next character), so its row is
            // still the one before.
            const int column = cur.positionInBlock();
            const int columns = qMax(1, screenColumns_);
            placeInRow(cur, column > 0 ? (column - 1) / columns * columns : 0);
#endif
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
                // sequence is in flight). Just the one character.
                pendingOutput_ = text.mid(i);
                break;
            }
            const ushort next = text[i + 1].unicode();
            if (next == '[') {
                // CSI: ESC [ <params> <intermediates> <final-byte>
                // where params are 0x30-0x3f, intermediates are
                // 0x20-0x2f, and the final byte is 0x40-0x7e.
                int j = i + 2;
                while (j < n && !isCsiFinal(text[j]))
                    ++j;
                if (j - i > kMaxCsiLength) {
                    // Too long to be a CSI: an ESC [ and then nothing
                    // but parameter bytes, megabytes of them, from a
                    // file `cat`ed here or a server's message during a
                    // fetch. Parked, it grew with every read until a
                    // final byte came, and was walked again from its
                    // ESC each time, so every read took longer than the
                    // one before. Dropped instead, as far as its final
                    // byte, in this read or a later one (droppingCsi_):
                    // what follows it shows as usual.
                    droppingCsi_ = j == n;
                    i = droppingCsi_ ? n : j + 1;
                    continue;
                }
                if (j < n) {
                    csi(cur, text[j].unicode(), QStringView(text).mid(i + 2, j - i - 2));
                    i = j + 1;
                } else {
                    // Unterminated CSI — its final byte is in the
                    // next chunk. Park everything from the ESC and
                    // resume when it arrives.
                    pendingOutput_ = text.mid(i);
                    i = n;
                }
                continue;
            }
            if (next == ']' && n - i < kMaxPendingOsc) {
                // An OSC the regex above couldn't remove because its
                // terminator is in the next chunk, or its last byte is
                // (the read ends on the ESC of an ESC \): conhost
                // retitles the window on every command, and shells'
                // prompts often do, so this does happen. Park it like
                // an unterminated CSI. n - i is all of the text from
                // the ESC on, what would be parked; a longer one isn't,
                // and its text shows.
                const qsizetype esc = text.indexOf(QChar(0x1b), i + 2);
                if (esc < 0 || esc == n - 1) {
                    pendingOutput_ = text.mid(i);
                    break;
                }
            }
            // ESC followed by a single byte we don't handle
            i += 2;
            continue;
        }
        if (!isText(text[i])) {
            // Other C0 control chars — drop silently.
            ++i;
            continue;
        }

        // Printable text, as far as the next control character:
        // OVERWRITE what is under the cursor (terminal semantics),
        // and extend the line past its end.
        int end = i + 1;
        while (end < n && isText(text[end]))
            ++end;
#ifdef Q_OS_WIN
        // Past the last column: wrap, as the terminal ConPTY paints for
        // would (it goes on at the next row without sending a newline).
        // So the run goes in a row's worth at a time (see rowChunk).
        while (i < end) {
            if (cur.positionInBlock() >= screenColumns_) {
                cur.movePosition(QTextCursor::StartOfBlock);
                screenLineFeed(cur);
            }
            const auto take = static_cast<int>(
                rowChunk(QStringView(text).mid(i, end - i),
                         screenColumns_ - cur.positionInBlock()));
            overwrite(cur, QStringView(text).mid(i, take));
            i += take;
        }
#else
        overwrite(cur, QStringView(text).mid(i, end - i));
        i = end;
#endif
    }
    edit.endEditBlock();

    // Sync the visible blinking caret with where we just finished
    // writing so the user can see where the shell's cursor is.
    setTextCursor(cur);
    screenCursor_ = cur;

    if (wasAtBottom) {
        sb->setValue(sb->maximum());
    }
}

// A character that starts in the room goes in whole, the row a unit or
// more over. In the terminal conhost paints for, that character is in
// the row's last cell: conhost doesn't start a wide character where it
// won't fit, so only a narrow one can be there (an astral one such as
// U+1D400, or a letter with combining marks), and the extra units are
// only the model's. Cutting it put half a surrogate pair, or a mark
// without its letter, at the start of the next row. (Only within a
// read: see the screen model's approximations below.)
qsizetype TerminalWidget::rowChunk(QStringView run, qsizetype room)
{
    // At least one: a row with no room left is wrapped before this.
    const qsizetype take = qMax(room, qsizetype(1));
    if (take >= run.size())
        return run.size();
    // Only the character at the cut is in question, and it is a few
    // units long (a Zalgo pile of marks is cut after these): no need
    // to look at the rest of a long run for each row.
    constexpr qsizetype kLookahead = 64;
    QTextBoundaryFinder characters(QTextBoundaryFinder::Grapheme,
                                   run.left(qMin(run.size(), take + kLookahead)));
    characters.setPosition(take);
    return characters.isAtBoundary() ? take : characters.toNextBoundary();
}

#ifdef Q_OS_WIN
// ---------------------------------------------------------------------------
// ConPTY screen model. ConPTY renders the console as a screen of
// screenColumns_ x screenRows_ cells: its rows are the blocks from
// screenTop_ down, everything above is scrollback, and rows below the
// last block are blank until something is painted there. Coordinates
// in escape sequences are 1-based.
//
// Approximations:
//   - A column is a UTF-16 unit of the row's text, not a cell. A wide
//     character in the BMP (CJK) is one unit over two cells and a
//     narrow astral one (U+1D400) two units in one, so a row with them
//     wraps late or early, and a cursor move into it lands off by as
//     much.
//   - rowChunk keeps a character whole only within one run of text in
//     one read. One that a read boundary, or an escape sequence, cuts
//     between its parts (a letter and the combining
//     mark after it, an emoji ZWJ sequence, a flag's two regional
//     indicators) can still wrap apart: if the first part ends a row,
//     the next read's rest starts the row below. (A surrogate pair is
//     never cut: it is one UTF-8 character, which the decoder holds
//     back until it is whole.) Rare, and only drawn wrong, so left as
//     it is.
//   - What conhost itself breaks stays broken. Windows' conhost (seen
//     on build 26100) gives each half of a surrogate pair a cell of its
//     own, so an astral character printed in the last column comes out
//     of ConPTY split over the wrap: something else in that cell, and
//     the low half alone, as invalid UTF-8 (three U+FFFD here), at the
//     start of the next row. The bytes are wrong before they get here;
//     TestTerminalWidget::conhostWrapKeepsCharactersWhole tells the two
//     apart by what ConPTY sent (outputRead).
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

#endif // Q_OS_WIN

// The CSI sequences that move or erase within the cursor's row, and on
// Windows the screen ones ConPTY paints with. SGR (colors), mode
// switches (ESC[?25l hides the cursor), window reports and the rest
// change nothing drawn here. The line editors use the row's: zsh moves
// with CUF and CUB, bash inserts and deletes with ICH and DCH.
void TerminalWidget::csi(QTextCursor& cur, ushort finalByte, QStringView params)
{
    for (const QChar c : params) {
        if (c.unicode() < '0' || c.unicode() > ';')
            return;  // private (ESC[?...) or with intermediates: not ours
    }
    const QList<QStringView> args = params.split(u';');
    const auto arg = [&args](qsizetype index) {
        if (index >= args.size())
            return 0;
        // A number of more than kMaxCountDigits digits is past every
        // edge of the screen, which is as far as anything goes (see
        // below), so it isn't read: toInt made one too big for an int
        // 0, the default, and one just under INT_MAX overflowed the
        // sums below. Kept to that, a count can go in them as it is.
        const QStringView digits = args.at(index);
        return digits.size() > kMaxCountDigits ? kMaxCount : digits.toInt();
    };
    const int count  = qMax(1, arg(0));  // moves treat 0 as 1
    const int column = cur.positionInBlock() + 1;
    const int length = cur.block().length() - 1;
#ifdef Q_OS_WIN
    // SU scrolls a screen's worth at most, as a terminal does: it adds a
    // row at the bottom for each row, and past that it would only push
    // blank rows into the scrollback, as many as it asked for (see ICH).
    const int lines  = qMin(count, qMax(1, screenRows_));
    const int row    = cur.blockNumber() - screenTop_.blockNumber() + 1;
    const auto toColumn = [&](int to) { screenMoveTo(cur, row, to); };
#else
    // No wrap here, so a row can run past the last column; a move right
    // stops there, as on a terminal, but never takes the cursor left.
    const auto toColumn = [&](int to) {
        if (to > column)
            to = qMax(column, qMin(to, screenColumns_));
        placeInRow(cur, qMax(1, to) - 1);
    };
#endif

    switch (finalByte) {
    case 'C': toColumn(column + count); break;  // CUF
    case 'D': toColumn(column - count); break;  // CUB
    case 'G':                                   // CHA
    case '`': toColumn(count); break;           // HPA
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
    case '@': {  // ICH: blanks in at the cursor, the rest of the row right
        // A screen's width at most: on a terminal, blanks past that
        // only push the rest of the row off its edge, which fewer do
        // too. ESC[50000000@ put fifty million in. The other counts
        // need no such cap, a move stopping at the row's start or the
        // screen's edge and an erase or delete at the row's end, and
        // must not have one: here a line longer than the terminal is
        // one row (see appendOutput), and with a screen's width as the
        // most, ESC[1000D took the cursor only partway back to its start.
        //
        // And the rest of the cursor's screen row goes no further than
        // that takes it: to the end of the screen row after it. What
        // another ICH pushes past there is lost, as off a terminal's
        // edge, and the rows beyond stay put. Pushed on without end,
        // each ESC[99999@ at the same place added a screen's width of
        // blanks to the row: a file of them `cat`ed in the Console made
        // one row of ten or more characters for each of its bytes, which
        // took longer to lay out with every read.
        if (column <= length) {
            const int columns = qMax(1, screenColumns_);
            const int blanks = qMin(count, columns);
            const int edge = (column - 1) / columns * columns + 2 * columns;
            const int base = cur.block().position();
            const int at = cur.position();
            cur.insertText(QString(blanks, QLatin1Char(' ')));
            if (const int over = qMin(blanks, length + blanks - edge); over > 0) {
                cur.setPosition(base + edge);
                cur.setPosition(base + edge + over, QTextCursor::KeepAnchor);
                cur.removeSelectedText();
            }
            cur.setPosition(at);
        }
        break;
    }
    case 'P': {  // DCH: the cursor's characters out, the rest of the row left
        QTextCursor erase = cur;
        for (int k = 0; k < count && !erase.atBlockEnd(); ++k)
            erase.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        erase.removeSelectedText();
        break;
    }
#ifdef Q_OS_WIN
    case 'H':  // CUP
    case 'f':  // HVP
        screenMoveTo(cur, qMax(1, arg(0)), qMax(1, arg(1)));
        break;
    case 'A': screenMoveTo(cur, row - count, column); break;  // CUU
    case 'B': screenMoveTo(cur, row + count, column); break;  // CUD
    case 'd': screenMoveTo(cur, count, column); break;        // VPA
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
        for (int k = 0; k < lines; ++k) {
            if (!screenTop_.block().next().isValid()) {
                QTextCursor end(document());
                end.movePosition(QTextCursor::End);
                end.insertText(QStringLiteral("\n"));
            }
            screenTop_.movePosition(QTextCursor::NextBlock);
        }
        screenMoveTo(cur, row, column);
        break;
#endif
    default:
        break;
    }
}

#ifdef Q_OS_WIN
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
    // Copy and paste first. event->matches() handles the platform-
    // correct modifier (Cmd on macOS, Ctrl elsewhere).
    if (event->matches(QKeySequence::Copy)) {
#ifndef Q_OS_MACOS
        // Copy is Ctrl+C here, which is also the interrupt (and the
        // only one cmd.exe knows): copy a selection, otherwise send ^C
        // below (Windows Terminal's rule). On macOS copy is Cmd+C, and
        // the Control key's Ctrl+C never matches it.
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
        if (!text.isEmpty()) {
            noteTyped();
            writeToPty(toTerminalBytes(text));
        }
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

    if (!bytes.isEmpty()) {
        noteTyped();
        writeToPty(bytes);
    }
}

// Whatever the user sends the shell can leave something on its line: a
// key at its prompt (an arrow or a Tab as much as a letter), an Enter
// that continues the line, keys typed ahead while a command runs, which
// the shell reads once it ends. So the next switch drops the line before
// it types its own (see switchWhenIdle), and one that has dropped it
// already and still waits drops it again. That costs a Ctrl+C at an
// empty prompt where the keys left none, after a command run and done
// say: nothing the shell prints tells an empty line from a continued one
// (the prompts are the user's to set). Only keys at the prompt used to
// count, after its last Enter, so the change went into a continued line
// or keys typed ahead; and a `cd` alone took no Ctrl+C at all. And only
// what is sent can: a command that runs after the Ctrl+C, a prompt
// hook's, puts nothing on the line (see switchWhenIdle).
void TerminalWidget::noteTyped()
{
    lineEmpty_ = false;
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
    if (::ioctl(masterFd_, TIOCSWINSZ, &ws) == 0)
        screenColumns_ = grid.width();
#endif
}

} // namespace gitbolt::widgets
