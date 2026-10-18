//
// TestTerminalWidget — the built-in terminal.
//
// Its output path on every platform, fed directly (no shell, no PTY):
//
//   - text overwrites what is under the cursor, as on a terminal;
//   - the control characters and escape sequences it knows keep their
//     meaning, and the rest are dropped;
//   - a read can end partway through an escape sequence or a UTF-8
//     character, and the rest is picked up from the next read. A
//     character cut in two used to come out as two U+FFFDs, and on
//     macOS and Linux an OSC cut in two as its text;
//   - the shell's cursor stays where a read leaves it, and the moves
//     and erases within its row are honored: git's progress meter and
//     the line editors' edits (recorded from zsh and bash) write over
//     what they drew. On macOS and Linux every read used to start at
//     the end of the text, and only EL was honored;
//   - overwriting replaces whole characters, surrogate pairs and
//     combining marks included;
//   - a read goes in as one edit, its plain text a run at a time: an
//     edit per character made a big output take seconds to draw;
//   - where Windows wraps a run at the last column, a character that
//     starts on the row ends there too: neither half of a surrogate
//     pair nor a combining mark goes on to the next row alone. (Only
//     the cut itself is tested off Windows.)
//
// End to end on macOS and Linux: the widget over a real PTY and
// /bin/sh, driven by key events.
//
//   - a flood of output (awk printing numbered lines) arrives whole
//     and in order while the event loop keeps turning. The read
//     handler used to read on until the PTY ran dry, which a shell
//     writing faster than the widget draws never lets happen; it now
//     reads a slice per call, and draws it in one edit;
//   - Ctrl+C typed during the flood gets through at once and stops it,
//     and the shell takes commands again. On Linux, as on Windows,
//     Ctrl+C is also the copy key, and with nothing selected it used
//     to copy nothing rather than interrupt;
//   - a progress meter's updates, reads of their own each ending in a
//     CR, redraw one row instead of piling up on it.
//
// And end to end on Windows: the widget over a real ConPTY and cmd.exe,
// driven by key events.
//
//   - start() on a hidden widget waits for the show;
//   - typed keystrokes run a command, and its output lands on a row of
//     its own. conhost puts the next prompt there with an absolute
//     cursor move, so without the screen model the prompt would be
//     glued onto the output row;
//   - `exit` ends the shell, and the widget says so;
//   - a two-line paste runs both lines: pasted line breaks go in as
//     CRs, since cmd under ConPTY keeps a LF as part of the line;
//   - a flood of plain text (`type` of a big file) arrives whole and in
//     order while the event loop keeps turning: ConPtyProcess holds
//     the shell back while a batch waits to be drawn, so no batch is
//     more than its cap;
//   - Ctrl+C stops a flood at once, rather than once a backlog has
//     been drawn, and the shell takes commands again;
//   - a row conhost paints with a surrogate pair, or a letter and its
//     combining mark, in its last column keeps the character whole,
//     as the fed wrap tests do with synthetic output.
//
// Each platform's end-to-end tests are a QSKIP on the others.
//

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStringList>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

#include "widgets/TerminalWidget.h"

#ifdef Q_OS_WIN
#include <QApplication>
#include <QClipboard>
#endif

using gitbolt::widgets::TerminalWidget;

namespace {

// The widget with its output path in reach: appendOutput() takes what
// a read of the PTY (or a batch from ConPTY) would hand it.
class FedTerminal : public TerminalWidget {
public:
    using TerminalWidget::appendOutput;
    using TerminalWidget::gridSize;
    using TerminalWidget::rowChunk;

    // Everything the shell's terminal sent, as it came.
    QByteArray read;

protected:
    void outputRead(const QByteArray& bytes) override { read += bytes; }
};

} // namespace

// The end-to-end tests: a real shell, its output read back off the
// widget.
namespace {

// A QTRY that runs out waits twice as long again before it fails (to
// report how long the condition took, if it ever came true), so it
// takes three times its timeout, and a little more: it counts 50 ms a
// step, and neither the step's wait running over (about 3 ms a step on
// macOS, more on Windows) nor the time the condition takes counts. A
// flood's 19 s wait took 60.5 s to fail. Each stays at a quarter of
// QtTest's per-function watchdog (QTEST_FUNCTION_TIMEOUT, 60 s), the
// floods' included (they pass in 2-3 s): a wait that never comes true
// then fails with the terminal's text, rather than the watchdog
// aborting the whole binary and the tests after it.
constexpr int kTimeoutMs = 15000;

QStringList rows(const TerminalWidget& terminal)
{
    QStringList result;
    for (const QString& row : terminal.toPlainText().split(QLatin1Char('\n')))
        result.append(row.trimmed());
    return result;
}

// The last row with anything on it: the screen's bottom rows are blank.
QString lastRow(const TerminalWidget& terminal)
{
    const QStringList all = rows(terminal);
    for (qsizetype i = all.size() - 1; i >= 0; --i) {
        if (!all.at(i).isEmpty())
            return all.at(i);
    }
    return {};
}

// The numbers of the rows that are exactly "<prefix><number>", top down.
QList<int> numberedRows(const TerminalWidget& terminal, const QString& prefix)
{
    const QRegularExpression numbered(
        QStringLiteral("^%1(\\d+)$").arg(QRegularExpression::escape(prefix)));
    QList<int> numbers;
    for (const QString& row : rows(terminal)) {
        const QRegularExpressionMatch match = numbered.match(row);
        if (match.hasMatch())
            numbers.append(match.capturedView(1).toInt());
    }
    return numbers;
}

// Each number one more than the one before: nothing lost or reordered.
bool consecutive(const QList<int>& numbers)
{
    for (qsizetype i = 1; i < numbers.size(); ++i) {
        if (numbers.at(i) != numbers.at(i - 1) + 1)
            return false;
    }
    return true;
}

} // namespace

#ifdef Q_OS_WIN
namespace {

// cmd's prompts ("C:\...>") shown so far.
qsizetype prompts(const TerminalWidget& terminal)
{
    qsizetype count = 0;
    for (const QString& row : rows(terminal))
        count += row.contains(QLatin1Char('>')) ? 1 : 0;
    return count;
}

// A fresh prompt is the last thing shown: the command before it is done.
bool atPrompt(const TerminalWidget& terminal)
{
    return lastRow(terminal).endsWith(QLatin1Char('>'));
}

// A shown terminal with cmd's first prompt up. The vertical scroll bar
// is there from the start: one appearing mid-flood would resize the
// pseudo console, and conhost's repaint at the new size is not what
// the flood tests are about.
void showAtPrompt(TerminalWidget& terminal)
{
    terminal.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    terminal.resize(800, 400);
    terminal.show();
    QVERIFY(QTest::qWaitForWindowExposed(&terminal));
    QTRY_VERIFY2_WITH_TIMEOUT(prompts(terminal) >= 1,
                              qPrintable(terminal.toPlainText()), kTimeoutMs);
}

} // namespace
#else
namespace {

const QString kShPrompt = QStringLiteral("gbprompt>");

// The physical Control key, which Qt calls Meta on macOS (see
// TerminalWidget::keyPressEvent).
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier kTerminalCtrl = Qt::MetaModifier;
#else
constexpr Qt::KeyboardModifier kTerminalCtrl = Qt::ControlModifier;
#endif

// A shell the tests can rely on, whoever runs them: /bin/sh rather than
// the user's $SHELL, with a prompt the tests know, no rc file of the
// user's, and HOME a throwaway directory for any history it saves.
// start() hands the shell this process's environment, so that is where
// it goes; the destructor puts it back.
//
// A test that runs bash instead gets the prompt from HOME's .bashrc as
// well: Debian's and Ubuntu's bash read /etc/bash.bashrc first, which
// sets a PS1 of its own over the one in the environment, and ~/.bashrc
// after it. dash, and bash run as sh, don't read it.
class PlainShell {
public:
    PlainShell()
    {
        set("SHELL", "/bin/sh");
        set("PS1", kShPrompt.toLatin1() + ' ');
        set("HOME", QFile::encodeName(home_.path()));
        set("ENV", {});
        set("BASH_ENV", {});
        set("HISTFILE", {});
        QFile bashrc(home_.path() + QStringLiteral("/.bashrc"));
        if (home_.isValid() && bashrc.open(QIODevice::WriteOnly))
            bashrc.write("PS1='" + kShPrompt.toLatin1() + " '\n");
    }

    ~PlainShell()
    {
        for (const Saved& saved : std::as_const(saved_)) {
            if (saved.wasSet)
                qputenv(saved.name.constData(), saved.value);
            else
                qunsetenv(saved.name.constData());
        }
    }

    bool isValid() const { return home_.isValid(); }
    QString home() const { return home_.path(); }

private:
    struct Saved {
        QByteArray name;
        bool       wasSet;
        QByteArray value;
    };

    // An empty value unsets it.
    void set(const char* name, const QByteArray& value)
    {
        saved_.append({name, qEnvironmentVariableIsSet(name), qgetenv(name)});
        if (value.isEmpty())
            qunsetenv(name);
        else
            qputenv(name, value);
    }

    QTemporaryDir home_;
    QList<Saved>  saved_;
};

// The shell has printed a fresh prompt and is waiting for a command.
bool atShPrompt(const TerminalWidget& terminal)
{
    return lastRow(terminal) == kShPrompt;
}

// A running terminal with the first prompt up. Not shown: nothing here
// needs a window, and none pops up.
void startAtPrompt(TerminalWidget& terminal)
{
    terminal.resize(800, 400);
    terminal.start(QDir::tempPath());
    QVERIFY(terminal.isRunning());
    QTRY_VERIFY2_WITH_TIMEOUT(atShPrompt(terminal),
                              qPrintable(terminal.toPlainText()), kTimeoutMs);
}

// Prints "gbflood1" to "gbflood<lines>", one per line, as fast as the
// PTY takes them.
QString floodCommand(int lines)
{
    return QStringLiteral("awk 'BEGIN { for (i = 1; i <= %1; i++) print \"gbflood\" i }'")
        .arg(lines);
}

// About 2 MB: seconds of drawing, where the read handler takes a few
// tens of milliseconds a call.
constexpr int kPtyFloodLines = 150000;

// The times the PTY wakes the terminal (its read notifier going off),
// and the most edits its document took in any one of them.
class WakeCounter : public QObject {
public:
    explicit WakeCounter(const TerminalWidget& terminal)
    {
        for (QSocketNotifier* notifier : terminal.findChildren<QSocketNotifier*>()) {
            if (notifier->type() == QSocketNotifier::Read)
                notifier->installEventFilter(this);
        }
        connect(terminal.document(), &QTextDocument::contentsChange, this,
                [this](int from, int /*removed*/, int added) {
                    // Not the trim that keeps the scrollback to its cap:
                    // the document makes that a change of its own after
                    // the edit, rows off the top and nothing added.
                    if (from == 0 && added == 0)
                        return;
                    mostEdits_ = qMax(mostEdits_, ++edits_);
                });
    }

    int wakes() const { return wakes_; }
    int mostEditsInAWake() const { return mostEdits_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::SockAct) {
            ++wakes_;
            edits_ = 0;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    int wakes_     = 0;
    int edits_     = 0;
    int mostEdits_ = 0;
};

} // namespace
#endif // Q_OS_WIN

class TestTerminalWidget : public QObject {
    Q_OBJECT

private slots:
    // zsh's line editor redraws the line it edits in place: a CR, then
    // the new text over the old.
    void textOverwritesInPlace()
    {
        FedTerminal terminal;
        terminal.appendOutput("hello world");
        terminal.appendOutput("\rJ");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("Jello world"));
        terminal.appendOutput("\rgoodbye, cruel world");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("goodbye, cruel world"));
        terminal.appendOutput("\r\nsecond\rS");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("goodbye, cruel world\nSecond"));
    }

    void controlsAndEscapes_data()
    {
        QTest::addColumn<QByteArray>("output");
        QTest::addColumn<QString>("shown");

        // (A hex escape takes every hex digit after it, hence the
        // literals split after some of them.)
        QTest::newRow("CR returns to column 0")
            << QByteArray("hello\rJ") << QStringLiteral("Jello");
        QTest::newRow("CRLF starts a line")
            << QByteArray("one\r\ntwo") << QStringLiteral("one\ntwo");
        QTest::newRow("BS moves left without erasing")
            << QByteArray("abc\b\bX") << QStringLiteral("aXc");
        QTest::newRow("BS stops at column 0")
            << QByteArray("a\b\b\bX") << QStringLiteral("X");
        QTest::newRow("EL erases to the end of the line")
            << QByteArray("abcdef\b\b\b\x1b[Kxy") << QStringLiteral("abcxy");
        QTest::newRow("SGR is dropped")
            << QByteArray("\x1b[1;31mred\x1b[0m!") << QStringLiteral("red!");
        QTest::newRow("DEC private modes are dropped")
            << QByteArray("\x1b[?2004hon\x1b[?2004l") << QStringLiteral("on");
        QTest::newRow("OSC and BEL are dropped")
            << QByteArray("\x1b]0;title\x07" "ab\x07" "c") << QStringLiteral("abc");
        QTest::newRow("other C0 controls are dropped")
            << QByteArray("a\x01" "b\x0e" "c") << QStringLiteral("abc");
        QTest::newRow("an unhandled ESC pair is dropped")
            << QByteArray("a\x1b=b") << QStringLiteral("ab");
        QTest::newRow("CUB moves left")
            << QByteArray("abcdef\x1b[2DX") << QStringLiteral("abcdXf");
        QTest::newRow("CUF moves right, padding past the end")
            << QByteArray("ab\x1b[3CX") << QStringLiteral("ab   X");
        QTest::newRow("CHA moves to a column")
            << QByteArray("abcdef\x1b[3GX") << QStringLiteral("abXdef");
        QTest::newRow("ECH blanks without moving")
            << QByteArray("abcdef\x1b[4G\x1b[2XZ") << QStringLiteral("abcZ f");
        QTest::newRow("ICH inserts blanks")
            << QByteArray("abcd\x1b[3G\x1b[2@X") << QStringLiteral("abX cd");
        QTest::newRow("DCH deletes characters")
            << QByteArray("abcdef\x1b[2G\x1b[2PX") << QStringLiteral("aXef");
        QTest::newRow("EL 1 blanks to the cursor")
            << QByteArray("abcdef\x1b[3G\x1b[1K") << QStringLiteral("   def");
        QTest::newRow("EL 2 blanks the row")
            << QByteArray("abcdef\x1b[3G\x1b[2KX") << QStringLiteral("  X");
        QTest::newRow("tab is kept")
            << QByteArray("a\tb") << QStringLiteral("a\tb");
        // PROMPT_SP: a reverse-video '%' and a row of spaces, which a
        // CR and the prompt then write over.
        QTest::newRow("zsh prompt")
            << QByteArray("\x1b[1m\x1b[7m%\x1b[27m\x1b[1m\x1b[0m          \r \r"
                          "\ruser@host repo % \x1b[K\x1b[?2004h")
            << QStringLiteral("user@host repo % ");
    }

    void controlsAndEscapes()
    {
        QFETCH(QByteArray, output);
        QFETCH(QString, shown);
        FedTerminal terminal;
        terminal.appendOutput(output);
        QCOMPARE(terminal.toPlainText(), shown);
    }

    // A read can end anywhere in an escape sequence, ESC included; the
    // rest arrives with the next one.
    void escapeSplitAcrossReads()
    {
        FedTerminal terminal;
        terminal.appendOutput("ab\x1b[3");
        terminal.appendOutput("1mc\x1b");
        terminal.appendOutput("[0md\x1b");
        terminal.appendOutput("[K\rX");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("Xbcd"));
    }

    // Or partway through a UTF-8 character, inside an escape sequence's
    // tail or not.
    void utf8SplitAcrossReads()
    {
        FedTerminal terminal;
        terminal.appendOutput("caf\xc3");
        terminal.appendOutput("\xa9 \xe2");
        terminal.appendOutput("\x82");
        terminal.appendOutput("\xac \xf0\x9f");
        terminal.appendOutput("\x98\x80 \x1b[1");
        terminal.appendOutput("m\xc3");
        terminal.appendOutput("\xb1!");
        QCOMPARE(terminal.toPlainText(),
                 QString::fromUtf16(u"caf\u00e9 \u20ac \U0001F600 \u00f1!"));
    }

    // A character written over another replaces all of it: both halves
    // of a surrogate pair, or a letter with its combining accent.
    void overwriteReplacesWholeCharacters()
    {
        FedTerminal terminal;
        terminal.appendOutput("\xf0\x9f\x98\x80" "b\rX");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("Xb"));
        terminal.appendOutput("\r\ne\xcc\x81" "x\rY");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("Xb\nYx"));
    }

    // A read goes in as one edit, its plain text a run at a time. An
    // edit per character (two to write over one) made a big output, a
    // long `git log` or a `cat`, take seconds to draw.
    void aReadIsDrawnInOneEdit()
    {
        // Colored rows, shorter than the 80 columns Windows wraps at.
        constexpr int kRows = 40;
        const QByteArray row(60, 'x');
        QByteArray output;
        for (int k = 0; k < kRows; ++k)
            output += "\x1b[33m" + QByteArray::number(k) + "\x1b[m " + row + "\r\n";

        FedTerminal terminal;
        int edits = 0;
        connect(terminal.document(), &QTextDocument::contentsChange,
                this, [&edits] { ++edits; });
        terminal.appendOutput(output);
        QCOMPARE(terminal.document()->blockCount(), kRows + 1);
        QCOMPARE(edits, 1);

        // Written over, too: a CR, then a shorter row over the last one.
        terminal.appendOutput(row + "\r" + QByteArray(30, 'y'));
        edits = 0;
        terminal.appendOutput("\r" + QByteArray(40, 'z'));
        QCOMPARE(terminal.document()->lastBlock().text(),
                 QString(40, QLatin1Char('z')) + QString(20, QLatin1Char('x')));
        QCOMPARE(edits, 1);
    }

    // The cursor stays where a read leaves it: git's progress meter ends
    // each update with a CR for the next one to write over. On macOS and
    // Linux every read used to start at the end of the text, so the
    // updates piled up on one row.
    void cursorStaysPutBetweenReads()
    {
        FedTerminal terminal;
        terminal.appendOutput("Receiving objects:  10% (1/10)\r");
        terminal.appendOutput("Receiving objects:  20% (2/10)\r");
        terminal.appendOutput("Receiving objects: 100% (10/10), done.\r\n");
        terminal.appendOutput("remote: Counting objects:  50% (1/2)\x1b[K\r");
        terminal.appendOutput("remote: Counting objects: 100% (2/2), done.\x1b[K\r\n");
        QCOMPARE(terminal.toPlainText(),
                 QStringLiteral("Receiving objects: 100% (10/10), done.\n"
                                "remote: Counting objects: 100% (2/2), done.\n"));
    }

    // Editing mid-line, as the shells' line editors draw it (recorded
    // from zsh 5.9 and bash 3.2 under a PTY): each arrow key is a read
    // of its own.
    void lineEditorsEditMidLine_data()
    {
        QTest::addColumn<QList<QByteArray>>("reads");
        QTest::addColumn<QString>("shown");

        // abcd, Left, Left, X, Ctrl+A, Ctrl+E, Left x3, Y
        QTest::newRow("zsh: backspaces and CUF")
            << QList<QByteArray>{"P> ", "a\x08" "abcd", "\x08", "\x08", "Xcd\x08\x08",
                                 "\x08\x08\x08", "\x1b[5C", "\x08\x08\x08", "YXcd\x08\x08\x08"}
            << QStringLiteral("P> abYXcd");
        // abcdefgh, Left x3, Backspace, Delete, X
        QTest::newRow("bash: DCH and ICH")
            << QList<QByteArray>{"P> ", "abcdefgh", "\x08\x08\x08", "\x08\x1b[1P",
                                 "\x1b[1P", "\x1b[1@X"}
            << QStringLiteral("P> abcdXgh");
        // zsh's Ctrl+U: back to the start, blank the line, CUB back
        QTest::newRow("zsh: kill line")
            << QList<QByteArray>{"P> abcdefgh", "\x08\x08\x08\x08\x08\x08\x08\x08        \x1b[8D", "x"}
            << QStringLiteral("P> x       ");
    }

    void lineEditorsEditMidLine()
    {
        QFETCH(QList<QByteArray>, reads);
        QFETCH(QString, shown);
        FedTerminal terminal;
        for (const QByteArray& read : reads)
            terminal.appendOutput(read);
        QCOMPARE(terminal.toPlainText(), shown);
    }

    // A command line longer than the terminal, as bash's readline types
    // it: at the right margin it sends the key, a space and a CR, for a
    // terminal that wraps to put the cursor at the start of the next
    // row. Rows don't wrap here (the line is one row), and the CR went
    // to its start: the keys after it overwrote the prompt. The widget
    // was never shown, so it has the default 80 columns.
    void longCommandLineStaysWhole()
    {
#ifdef Q_OS_WIN
        QSKIP("ConPTY's screen wraps its rows itself");
#else
        FedTerminal terminal;
        const QByteArray prompt = "P> ";
        const QByteArray first(80 - prompt.size() - 1, 'x');   // to the last column but one
        terminal.appendOutput(prompt);
        for (const char c : first)
            terminal.appendOutput(QByteArray(1, c));
        terminal.appendOutput("y \r");                        // the last column, and the wrap
        for (const char c : QByteArrayLiteral("echo"))
            terminal.appendOutput(QByteArray(1, c));
        // A second wrap.
        const QByteArray second(80 - 4 - 1, 'z');
        terminal.appendOutput(second + "w \r" + "tail");
        QCOMPARE(terminal.toPlainText(),
                 QString::fromLatin1(prompt + first + "y" + "echo" + second + "w" + "tail"));

        // A CR right after the last column: the cursor hasn't wrapped
        // yet, so it goes to the start of that row, not of the next. One
        // character on, it's the next row's.
        FedTerminal margin;
        margin.appendOutput(QByteArray(80, 'a') + "\rB");
        QCOMPARE(margin.toPlainText(), QStringLiteral("B") + QString(79, QLatin1Char('a')));
        margin.appendOutput("\r\n" + QByteArray(80, 'a') + "b\rC");
        QCOMPARE(margin.toPlainText().section(QLatin1Char('\n'), 1),
                 QString(80, QLatin1Char('a')) + QStringLiteral("C"));
#endif
    }

    // An OSC (a title, an OSC 8 link) cut by a read, anywhere up to its
    // terminator's last byte, used to print its text on macOS and Linux.
    void oscSplitAcrossReads()
    {
        FedTerminal terminal;
        terminal.appendOutput("a\x1b]0;ti");
        terminal.appendOutput("tle\x07" "b");
        terminal.appendOutput("\x1b]8;;https://example.com\x1b");
        terminal.appendOutput("\\c\x1b]8;;\x1b\\d");
        QCOMPARE(terminal.toPlainText(), QStringLiteral("abcd"));
    }

    // Windows wraps a long run a row at a time, a UTF-16 unit to a
    // column. The cut used to fall wherever the units ran out, so a
    // character at the edge could lose half its surrogate pair, or its
    // combining marks, to the next row.
    void rowChunkKeepsCharactersWhole_data()
    {
        QTest::addColumn<QString>("run");
        QTest::addColumn<int>("room");
        QTest::addColumn<int>("chunk");

        QTest::newRow("all of it fits") << QStringLiteral("abc") << 5 << 3;
        QTest::newRow("as much as fits") << QStringLiteral("abcdef") << 4 << 4;
        QTest::newRow("at least one") << QStringLiteral("abc") << 0 << 1;
        QTest::newRow("a surrogate pair in the last column")
            << QString::fromUtf16(u"\U0001D400y") << 1 << 2;
        QTest::newRow("a surrogate pair across the edge")
            << QString::fromUtf16(u"abc\U0001D400y") << 4 << 5;
        QTest::newRow("a surrogate pair past the edge")
            << QString::fromUtf16(u"abcd\U0001D400") << 4 << 4;
        QTest::newRow("a letter and its combining marks")
            << QString::fromUtf16(u"abe\u0301\u0302x") << 3 << 5;
        QTest::newRow("a flag, two regional indicators")
            << QString::fromUtf16(u"a\U0001F1FA\U0001F1F8\U0001F1EC\U0001F1E7") << 3 << 5;
        QTest::newRow("an emoji ZWJ sequence")
            << QString::fromUtf16(u"a\U0001F469\u200D\U0001F4BBb") << 2 << 6;
    }

    void rowChunkKeepsCharactersWhole()
    {
        QFETCH(QString, run);
        QFETCH(int, room);
        QFETCH(int, chunk);
        QCOMPARE(FedTerminal::rowChunk(run, room), qsizetype(chunk));
    }

    // The same through the screen ConPTY paints (80 columns until it is
    // resized), with the character at the edge in the row's read or in
    // the next one.
    void wrapKeepsCharactersWhole()
    {
#ifndef Q_OS_WIN
        QSKIP("Only ConPTY's screen wraps rows");
#else
        const QString row(79, QLatin1Char('x'));
        for (const QString& character : {QString::fromUtf16(u"\U0001D400"),
                                         QString::fromUtf16(u"e\u0301")}) {
            const QString shown = row + character + QStringLiteral("\ny");

            FedTerminal oneRead;
            oneRead.appendOutput((row + character + QStringLiteral("y")).toUtf8());
            QCOMPARE(oneRead.toPlainText(), shown);

            FedTerminal twoReads;
            twoReads.appendOutput(row.toUtf8());
            twoReads.appendOutput((character + QStringLiteral("y")).toUtf8());
            QCOMPARE(twoReads.toPlainText(), shown);
        }
#endif
    }

    // And with what real conhost paints: cmd types out a row of x's that
    // ends in the character at the last column, then a y. No row may
    // start with half a surrogate pair (UTF-8 can't carry one, so only
    // the widget's cut could put it there), and nothing may be lost or
    // reordered. conhost itself may give a combining mark a cell of its
    // own, and so paint it at the start of the next row, which the
    // widget can only show as it comes: that is reported, not failed.
    // So is an astral character conhost doesn't send whole: build 26100
    // gives each half of the surrogate pair a cell, and ConPTY sends
    // the halves on either side of the wrap, as bytes that aren't
    // UTF-8 (the widget shows U+FFFD for them). Whether the character
    // came whole is read off what ConPTY sent (outputRead); one that
    // did must show whole.
    void conhostWrapKeepsCharactersWhole()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        FedTerminal terminal;
        terminal.start(QDir::tempPath());
        showAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;
        const int columns = terminal.gridSize().width();
        QVERIFY(columns > 1);

        int round = 0;
        for (const QString& character : {QString::fromUtf16(u"\U0001D400"),
                                         QString::fromUtf16(u"é")}) {
            const QString line =
                QString(columns - 1, QLatin1Char('x')) + character + QStringLiteral("y");
            QFile file(dir.filePath(QStringLiteral("wrap%1.txt").arg(++round)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(line.toUtf8() + "\r\n");
            file.close();

            // `type` copies the file's bytes; code page 65001 makes
            // conhost read them as the UTF-8 they are.
            const qsizetype before = prompts(terminal);
            terminal.read.clear();
            QTest::keyClicks(&terminal, QStringLiteral("chcp 65001>nul & type \"%1\"")
                                            .arg(QDir::toNativeSeparators(file.fileName())));
            QTest::keyClick(&terminal, Qt::Key_Return);
            QTRY_VERIFY2_WITH_TIMEOUT(prompts(terminal) > before && atPrompt(terminal),
                                      qPrintable(terminal.toPlainText()), kTimeoutMs);

            for (const QString& row : terminal.toPlainText().split(QLatin1Char('\n'))) {
                QVERIFY2(row.isEmpty() || !row.front().isLowSurrogate(),
                         qPrintable(terminal.toPlainText()));
                if (!row.isEmpty() && row.front() == QChar(u'́'))
                    qInfo("conhost put the combining mark at the start of a row");
            }
            const QString shown = rows(terminal).join(QString());
            if (!terminal.read.contains(character.toUtf8())) {
                // Broken before the widget got it: what came after the
                // x's, in hex, for the record.
                const QByteArray xs(columns - 1, 'x');
                const qsizetype at = terminal.read.lastIndexOf(xs);
                qInfo("conhost didn't send U+%04X whole; after the x's it sent: %s",
                      character.toUcs4().value(0),
                      at < 0 ? "(no row of x's)"
                             : terminal.read.mid(at + xs.size(), 24).toHex(' ').constData());
                // The rest came, in order.
                QVERIFY2(shown.contains(QString(columns - 1, QLatin1Char('x')))
                             && shown.indexOf(QLatin1Char('y'),
                                              shown.indexOf(QString(columns - 1, QLatin1Char('x'))))
                                    > 0,
                         qPrintable(terminal.toPlainText()));
                continue;
            }
            QVERIFY2(shown.contains(line), qPrintable(terminal.toPlainText()));
        }
#endif
    }

    void runsCmdUnderConPty()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        TerminalWidget terminal;
        terminal.resize(800, 400);
        terminal.start(QDir::tempPath());
        QVERIFY(!terminal.isRunning());  // hidden: waits for the show

        terminal.show();
        QVERIFY(QTest::qWaitForWindowExposed(&terminal));
        QVERIFY(terminal.isRunning());
        QTRY_VERIFY2_WITH_TIMEOUT(prompts(terminal) >= 1,
                                  qPrintable(terminal.toPlainText()), kTimeoutMs);

        QTest::keyClicks(&terminal, QStringLiteral("echo gitbolt-widget-ok"));
        QTest::keyClick(&terminal, Qt::Key_Return);
        // Wait for the prompt after the output, then check the output
        // has its row to itself.
        QTRY_VERIFY2_WITH_TIMEOUT(prompts(terminal) >= 2,
                                  qPrintable(terminal.toPlainText()), kTimeoutMs);
        QVERIFY2(rows(terminal).contains(QStringLiteral("gitbolt-widget-ok")),
                 qPrintable(terminal.toPlainText()));

        QTest::keyClicks(&terminal, QStringLiteral("exit"));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(
            terminal.toPlainText().contains(QStringLiteral("[gitbolt] shell exited")),
            qPrintable(terminal.toPlainText()), kTimeoutMs);
        QVERIFY(!terminal.isRunning());
#endif
    }

    void pasteRunsEachLine()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        TerminalWidget terminal;
        terminal.start(QDir::tempPath());
        showAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;

        // A CRLF and a LF: clipboard text comes with either.
        QApplication::clipboard()->setText(
            QStringLiteral("echo gitbolt-paste-one\r\necho gitbolt-paste-two\n"));
        QTest::keyClick(&terminal, Qt::Key_V, Qt::ControlModifier);
        QTRY_VERIFY2_WITH_TIMEOUT(rows(terminal).contains(QStringLiteral("gitbolt-paste-two")),
                                  qPrintable(terminal.toPlainText()), kTimeoutMs);
        const QStringList shown = rows(terminal);
        const qsizetype one = shown.indexOf(QStringLiteral("gitbolt-paste-one"));
        QVERIFY2(one >= 0 && one < shown.indexOf(QStringLiteral("gitbolt-paste-two")),
                 qPrintable(terminal.toPlainText()));
#endif
    }

    void floodArrivesInOrderAndStaysResponsive()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        // Plain text is what costs the widget most to draw.
        constexpr int kLines = 40000;
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile flood(dir.filePath(QStringLiteral("flood.txt")));
        QVERIFY(flood.open(QIODevice::WriteOnly));
        for (int line = 1; line <= kLines; ++line)
            flood.write(QStringLiteral("gbtype%1\r\n").arg(line).toLatin1());
        flood.close();

        TerminalWidget terminal;
        terminal.start(QDir::tempPath());
        showAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;

        // The longest the event loop goes without getting round to a
        // 10 ms timer while the flood is drawn.
        QElapsedTimer clock;
        clock.start();
        qint64 lastTick = 0;
        qint64 longestGap = 0;
        QTimer ticker;
        ticker.setInterval(10);
        connect(&ticker, &QTimer::timeout, this, [&] {
            const qint64 now = clock.elapsed();
            longestGap = qMax(longestGap, now - lastTick);
            lastTick = now;
        });
        ticker.start();

        QTest::keyClicks(&terminal, QStringLiteral("type \"%1\"")
                                        .arg(QDir::toNativeSeparators(flood.fileName())));
        QTest::keyClick(&terminal, Qt::Key_Return);
        const auto done = [&] {
            const QList<int> shown = numberedRows(terminal, QStringLiteral("gbtype"));
            return atPrompt(terminal) && !shown.isEmpty() && shown.last() == kLines;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(done(), qPrintable(lastRow(terminal)), kTimeoutMs);
        ticker.stop();
        longestGap = qMax(longestGap, clock.elapsed() - lastTick);  // and since the last turn

        // The scrollback keeps the last 5000 rows: those must be the
        // flood's last lines, every one, in order.
        const QList<int> shown = numberedRows(terminal, QStringLiteral("gbtype"));
        QVERIFY2(shown.size() > 1000 && consecutive(shown),
                 qPrintable(QStringLiteral("%1 rows, from %2 to %3").arg(shown.size())
                                .arg(shown.value(0)).arg(shown.value(shown.size() - 1))));
        qInfo("%d lines in %lld ms; the event loop went %lld ms at most between turns",
              kLines, clock.elapsed(), longestGap);
        QVERIFY2(longestGap < 3000, qPrintable(QStringLiteral("%1 ms").arg(longestGap)));
#endif
    }

    void ctrlCStopsAFlood()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        TerminalWidget terminal;
        terminal.start(QDir::tempPath());
        showAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;

        QTest::keyClicks(&terminal,
                         QStringLiteral("for /L %i in (1,1,1000000) do @echo gbflood%i"));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(numberedRows(terminal, QStringLiteral("gbflood")).size() >= 100,
                                  qPrintable(lastRow(terminal)), kTimeoutMs);

        // No selection, so Ctrl+C interrupts. Only what was held back
        // (a batch, and whatever conhost had in hand) is left to draw.
        QVERIFY(!terminal.textCursor().hasSelection());
        QTest::keyClick(&terminal, Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY2_WITH_TIMEOUT(atPrompt(terminal), qPrintable(lastRow(terminal)), 10000);
        const QList<int> shown = numberedRows(terminal, QStringLiteral("gbflood"));
        QVERIFY2(!shown.isEmpty() && consecutive(shown) && shown.last() < 1000000,
                 qPrintable(QStringLiteral("%1 rows, last %2").arg(shown.size())
                                .arg(shown.value(shown.size() - 1))));

        QTest::keyClicks(&terminal, QStringLiteral("echo gitbolt-after-flood"));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(rows(terminal).contains(QStringLiteral("gitbolt-after-flood")),
                                  qPrintable(lastRow(terminal)), kTimeoutMs);
#endif
    }

    // The same two on Unix, over a real PTY and /bin/sh. A shell that
    // writes faster than the widget draws keeps the PTY full, and the
    // read handler used to read on until it ran dry: the GUI thread
    // stayed in it for as long as the flood lasted.
    void ptyFloodArrivesInOrderAndStaysResponsive()
    {
#ifdef Q_OS_WIN
        QSKIP("The PTY is the Unix terminal backend");
#else
        PlainShell shell;
        QVERIFY(shell.isValid());
        TerminalWidget terminal;
        startAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;

        // The longest the event loop goes without getting round to a
        // 10 ms timer while the flood is drawn.
        QElapsedTimer clock;
        clock.start();
        qint64 lastTick = 0;
        qint64 longestGap = 0;
        QTimer ticker;
        ticker.setInterval(10);
        connect(&ticker, &QTimer::timeout, this, [&] {
            const qint64 now = clock.elapsed();
            longestGap = qMax(longestGap, now - lastTick);
            lastTick = now;
        });
        ticker.start();
        WakeCounter wakes(terminal);

        QTest::keyClicks(&terminal, floodCommand(kPtyFloodLines));
        QTest::keyClick(&terminal, Qt::Key_Return);
        // The flood's last line with the prompt after it. Just the last
        // two rows: reading back all 5000 each time this is asked would
        // add to the gaps measured.
        const auto done = [&] {
            const QTextBlock prompt = terminal.document()->lastBlock();
            return prompt.text().trimmed() == kShPrompt
                   && prompt.previous().text()
                          == QStringLiteral("gbflood%1").arg(kPtyFloodLines);
        };
        QTRY_VERIFY2_WITH_TIMEOUT(done(), qPrintable(lastRow(terminal)), kTimeoutMs);
        ticker.stop();
        longestGap = qMax(longestGap, clock.elapsed() - lastTick);  // and since the last turn

        // The scrollback keeps the last 5000 rows: those must be the
        // flood's last lines, every one, in order.
        const QList<int> shown = numberedRows(terminal, QStringLiteral("gbflood"));
        QVERIFY2(shown.size() > 1000 && consecutive(shown),
                 qPrintable(QStringLiteral("%1 rows, from %2 to %3").arg(shown.size())
                                .arg(shown.value(0)).arg(shown.value(shown.size() - 1))));
        qInfo("%d lines in %lld ms, %d wakes; the event loop went %lld ms at most "
              "between turns", kPtyFloodLines, clock.elapsed(), wakes.wakes(), longestGap);
        QVERIFY2(longestGap < 1000, qPrintable(QStringLiteral("%1 ms").arg(longestGap)));

        // And each wake's slice went in as one edit (see
        // aReadIsDrawnInOneEdit), where an edit per read() laid out and
        // trimmed the document dozens of times a wake: a PTY hands over
        // as little as a kilobyte a read.
        QCOMPARE(wakes.mostEditsInAWake(), 1);
#endif
    }

    // A progress meter over a real PTY: its updates come in reads of
    // their own, each ending in a CR for the next to write over.
    void ptyProgressMeterRedrawsInPlace()
    {
#ifdef Q_OS_WIN
        QSKIP("The PTY is the Unix terminal backend");
#else
        PlainShell shell;
        QVERIFY(shell.isValid());
        // In a script, so the line typed is short: a terminal that was
        // never shown tells the shell its default size, about 44
        // columns, whatever resize() asked for, and the shell would
        // wrap a longer one, which this widget's rows don't.
        QFile script(QDir(shell.home()).filePath(QStringLiteral("meter.sh")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("printf 'gbmeter 1\\r'; sleep 1; printf 'gbmeter 2\\n'\n");
        script.close();
        TerminalWidget terminal;
        startAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;
        QTest::keyClicks(&terminal, QStringLiteral(". \"$HOME/meter.sh\""));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(rows(terminal).contains(QStringLiteral("gbmeter 2")),
                                  qPrintable(terminal.toPlainText()), kTimeoutMs);
        QVERIFY2(!terminal.toPlainText().contains(QStringLiteral("gbmeter 1")),
                 qPrintable(terminal.toPlainText()));
#endif
    }

    // A command line longer than the terminal, typed into bash over a
    // real PTY: readline's wrap at the right margin (the key, a space, a
    // CR) took the line back to the prompt, which the rest of the
    // command overwrote. 0.9.5 showed it whole.
    void ptyLongCommandLineStaysWhole()
    {
#ifdef Q_OS_WIN
        QSKIP("The PTY is the Unix terminal backend");
#else
        if (!QFileInfo::exists(QStringLiteral("/bin/bash")))
            QSKIP("no /bin/bash");
        PlainShell shell;
        QVERIFY(shell.isValid());
        // bash's readline, whose wrap this is (PlainShell puts SHELL
        // back on the way out).
        qputenv("SHELL", "/bin/bash");
        TerminalWidget terminal;
        startAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;
        // Past the margin (the terminal tells the shell its size, about
        // 44 columns never shown) twice.
        const QString command = QStringLiteral("echo ") + QString(90, QLatin1Char('x'))
                              + QStringLiteral("END");
        QTest::keyClicks(&terminal, command);
        QTRY_VERIFY2_WITH_TIMEOUT(terminal.toPlainText().contains(QStringLiteral("END")),
                                  qPrintable(terminal.toPlainText()), kTimeoutMs);
        QTest::qWait(200);
        QVERIFY2(rows(terminal).contains(kShPrompt + QLatin1Char(' ') + command),
                 qPrintable(terminal.toPlainText()));
#endif
    }

    // The shell's repository is the one it starts in: GIT_DIR and
    // GIT_WORK_TREE in GitBolt's own environment (a shell exporting them
    // for a bare dotfiles repository) are left out, and a bare
    // repository's own GIT_DIR (setGitDir) goes in.
    void ptyShellGetsNoInheritedGitVariables_data()
    {
        QTest::addColumn<QString>("gitDir");
        QTest::addColumn<QString>("expected");
        QTest::newRow("a repository with a work tree") << QString() << QStringLiteral("gbenv[][]");
        QTest::newRow("a bare repository")
            << QStringLiteral("/tmp/bare.git") << QStringLiteral("gbenv[/tmp/bare.git][]");
    }

    void ptyShellGetsNoInheritedGitVariables()
    {
#ifdef Q_OS_WIN
        QSKIP("The PTY is the Unix terminal backend");
#else
        QFETCH(QString, gitDir);
        QFETCH(QString, expected);
        PlainShell shell;
        QVERIFY(shell.isValid());
        const QByteArray savedDir = qgetenv("GIT_DIR");
        const QByteArray savedTree = qgetenv("GIT_WORK_TREE");
        const bool hadDir = qEnvironmentVariableIsSet("GIT_DIR");
        const bool hadTree = qEnvironmentVariableIsSet("GIT_WORK_TREE");
        const auto restore = qScopeGuard([&] {
            if (hadDir) qputenv("GIT_DIR", savedDir); else qunsetenv("GIT_DIR");
            if (hadTree) qputenv("GIT_WORK_TREE", savedTree); else qunsetenv("GIT_WORK_TREE");
        });
        qputenv("GIT_DIR", "/elsewhere/.git");
        qputenv("GIT_WORK_TREE", "/elsewhere");

        TerminalWidget terminal;
        terminal.setGitDir(gitDir);
        startAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;
        QTest::keyClicks(&terminal, QStringLiteral("echo gbenv[$GIT_DIR][$GIT_WORK_TREE]"));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(rows(terminal).contains(expected),
                                  qPrintable(terminal.toPlainText()), kTimeoutMs);
#endif
    }

    void ctrlCStopsAPtyFlood()
    {
#ifdef Q_OS_WIN
        QSKIP("The PTY is the Unix terminal backend");
#else
        PlainShell shell;
        QVERIFY(shell.isValid());
        TerminalWidget terminal;
        startAtPrompt(terminal);
        if (QTest::currentTestFailed())
            return;

        // A flood with an end, so a widget that keeps the event loop
        // waiting fails the test rather than hanging it: the Ctrl+C
        // then gets through only once the flood is over.
        QTest::keyClicks(&terminal, floodCommand(kPtyFloodLines));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(numberedRows(terminal, QStringLiteral("gbflood")).size() >= 100,
                                  qPrintable(lastRow(terminal)), kTimeoutMs);
        // Everything drawn so far, in order, but for the last row: a read
        // can end partway through a line. (Not checked after the
        // interrupt: the line discipline throws away the output it
        // holds, so a line can be cut short there.)
        QList<int> before = numberedRows(terminal, QStringLiteral("gbflood"));
        before.removeLast();
        QVERIFY2(consecutive(before),
                 qPrintable(QStringLiteral("%1 rows, from %2 to %3").arg(before.size())
                                .arg(before.value(0)).arg(before.value(before.size() - 1))));

        // No selection, so Ctrl+C interrupts (outside macOS it is the
        // copy key too, and would copy one).
        QVERIFY(!terminal.textCursor().hasSelection());
        QElapsedTimer clock;
        clock.start();
        QTest::keyClick(&terminal, Qt::Key_C, kTerminalCtrl);
        QTRY_VERIFY2_WITH_TIMEOUT(atShPrompt(terminal), qPrintable(lastRow(terminal)), 10000);
        const qint64 stoppedIn = clock.elapsed();
        const QList<int> shown = numberedRows(terminal, QStringLiteral("gbflood"));
        QVERIFY2(!shown.isEmpty() && shown.last() < kPtyFloodLines,
                 qPrintable(QStringLiteral("%1 rows, last %2").arg(shown.size())
                                .arg(shown.value(shown.size() - 1))));
        qInfo("Ctrl+C stopped the flood after %d lines, in %lld ms",
              shown.last(), stoppedIn);
        QVERIFY2(stoppedIn < 3000, qPrintable(QStringLiteral("%1 ms").arg(stoppedIn)));

        QTest::keyClicks(&terminal, QStringLiteral("echo gitbolt-after-flood"));
        QTest::keyClick(&terminal, Qt::Key_Return);
        QTRY_VERIFY2_WITH_TIMEOUT(rows(terminal).contains(QStringLiteral("gitbolt-after-flood")),
                                  qPrintable(lastRow(terminal)), kTimeoutMs);
#endif
    }
};

QTEST_MAIN(TestTerminalWidget)
#include "TestTerminalWidget.moc"
