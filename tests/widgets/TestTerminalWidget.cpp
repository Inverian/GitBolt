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
//     character cut in two used to come out as two U+FFFDs;
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
//     to copy nothing rather than interrupt.
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
//     been drawn, and the shell takes commands again.
//
// Each platform's end-to-end tests are a QSKIP on the others.
//

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStringList>
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
    using TerminalWidget::rowChunk;
};

} // namespace

// The end-to-end tests: a real shell, its output read back off the
// widget.
namespace {

constexpr int kTimeoutMs = 30000;
// A flood takes longer, but stays inside QtTest's per-function
// watchdog (QTEST_FUNCTION_TIMEOUT), so a slow one fails cleanly.
constexpr int kFloodTimeoutMs = 50000;

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
// the user's $SHELL, with a prompt the tests know, no rc file, and HOME
// a throwaway directory for any history it saves. start() hands the
// shell this process's environment, so that is where it goes; the
// destructor puts it back.
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
        QTRY_VERIFY2_WITH_TIMEOUT(done(), qPrintable(lastRow(terminal)), kFloodTimeoutMs);
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
        QTRY_VERIFY2_WITH_TIMEOUT(done(), qPrintable(lastRow(terminal)), kFloodTimeoutMs);
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
