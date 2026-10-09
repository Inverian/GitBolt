//
// TestTerminalWidget — the built-in terminal end to end on Windows:
// the widget over a real ConPTY and cmd.exe, driven by key events.
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
// ConPTY only exists on Windows; elsewhere the test is a QSKIP (the
// Unix PTY side has no automated test yet).
//

#include <QTest>

#ifdef Q_OS_WIN
#include "widgets/TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

using gitbolt::widgets::TerminalWidget;

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

// cmd's prompts ("C:\...>") shown so far.
qsizetype prompts(const TerminalWidget& terminal)
{
    qsizetype count = 0;
    for (const QString& row : rows(terminal))
        count += row.contains(QLatin1Char('>')) ? 1 : 0;
    return count;
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

// A fresh prompt is the last thing shown: the command before it is done.
bool atPrompt(const TerminalWidget& terminal)
{
    return lastRow(terminal).endsWith(QLatin1Char('>'));
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
#endif // Q_OS_WIN

class TestTerminalWidget : public QObject {
    Q_OBJECT

private slots:
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
};

QTEST_MAIN(TestTerminalWidget)
#include "TestTerminalWidget.moc"
