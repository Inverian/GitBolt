//
// TestConPtyProcess — the built-in terminal's Windows backend, driven
// directly (no widget): a real cmd.exe under a real pseudo console.
//
//   - a command round trip: "echo" typed as terminal input comes back
//     as output, then stop() tears everything down in time, the shell
//     included, and no handler runs after it;
//   - a shell that exits by itself is noticed, with its exit code;
//   - lines written in one go each run, in turn: what a multi-line
//     paste becomes once TerminalWidget has made its line breaks CRs;
//   - flow control: output nobody takes is held back at
//     kMaxPendingOutput, a flood still arrives whole and in order, and
//     there is one output cue per batch taken, not one per read;
//   - stop() is prompt mid-flood, with the reader held at the cap;
//   - a long write gets through while the reader is held at the cap.
//     conhost can stop taking input until its output has gone out, so
//     unless write() lets the reader on, the two wait on each other.
//
// The handlers run on ConPtyProcess's threads, so they only count into
// a mutex-guarded Capture. The output is taken on the test's thread,
// as TerminalWidget takes it on its GUI thread, and the assertions
// poll with QTRY_*.
//
// ConPtyProcess only exists in Windows builds; everywhere else each
// test is a QSKIP, so the file still builds and shows up in ctest.
//

#include <QTest>

#ifdef Q_OS_WIN
#include "widgets/ConPtyProcess.h"

#include <QDir>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using gitbolt::widgets::ConPtyProcess;

namespace {

// What the handlers reported, and the output taken so far.
class Capture {
public:
    ConPtyProcess::OutputHandler outputHandler()
    {
        return [this] {
            const std::lock_guard lock(mutex_);
            ++cues_;
            ++calls_;
        };
    }

    ConPtyProcess::ExitHandler exitHandler()
    {
        return [this](unsigned long exitCode) {
            const std::lock_guard lock(mutex_);
            exitCode_ = exitCode;
            ++calls_;
        };
    }

    // Take what `pty` holds; returns how many bytes that was.
    qsizetype take(ConPtyProcess& pty)
    {
        const std::string output = pty.takeOutput();
        if (!output.empty())
            ++takes_;
        output_.append(output.data(), static_cast<qsizetype>(output.size()));
        return static_cast<qsizetype>(output.size());
    }

    // All the output, after taking what `pty` holds, without its VT
    // sequences: ConPTY wraps each frame in cursor hide/show and puts
    // the running command line in the window title (OSC), which must
    // not count as output, nor must a title still on its way.
    QString text(ConPtyProcess& pty)
    {
        take(pty);
        static const QRegularExpression osc(
            QStringLiteral("\\x1b\\][^\\x07\\x1b]*(\\x07|\\x1b\\\\|$)"));
        static const QRegularExpression csi(
            QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]"));
        QString text = QString::fromUtf8(output_);
        text.remove(osc);
        text.remove(csi);
        return text;
    }

    std::optional<unsigned long> exitCode() const
    {
        const std::lock_guard lock(mutex_);
        return exitCode_;
    }

    int calls() const
    {
        const std::lock_guard lock(mutex_);
        return calls_;
    }

    int cues() const
    {
        const std::lock_guard lock(mutex_);
        return cues_;
    }

    int takes() const { return takes_; }

private:
    mutable std::mutex mutex_;
    std::optional<unsigned long> exitCode_;  // mutex_
    int calls_ = 0;                          // mutex_
    int cues_  = 0;                          // mutex_
    QByteArray output_;                      // the test's thread only
    int takes_ = 0;                          // the test's thread only
};

ConPtyProcess::StartInfo cmdExe()
{
    const QString systemRoot =
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"));
    ConPtyProcess::StartInfo info;
    info.commandLine = QStringLiteral("\"%1\\System32\\cmd.exe\"")
                           .arg(QDir::toNativeSeparators(systemRoot))
                           .toStdWString();
    info.workingDirectory = QDir::toNativeSeparators(QDir::tempPath()).toStdWString();
    return info;
}

// cmd's prompt ("C:\...>") has arrived, so it is reading input.
bool promptShown(Capture& capture, ConPtyProcess& pty)
{
    return capture.text(pty).contains(QLatin1Char('>'));
}

// How far a flood of numbered lines ("gbflood1", "gbflood2", ...) has
// come in order: n once lines 1 to n have all arrived, or -n if line n
// went missing (a later one came first). A number seen again is ConPTY
// repainting a row, and doesn't count.
int floodProgress(const QString& text)
{
    static const QRegularExpression line(QStringLiteral("gbflood(\\d+)"));
    int next = 1;
    for (const QRegularExpressionMatch& match : line.globalMatch(text)) {
        const int number = match.capturedView(1).toInt();
        if (number == next)
            ++next;
        else if (number > next)
            return -next;
    }
    return next - 1;
}

// Starts a flood that would run for minutes.
constexpr std::string_view kEndlessFlood = "for /L %i in (1,1,1000000) do @echo gbflood%i\r";

constexpr int kTimeoutMs = 30000;
// A flood takes longer, but stays inside QtTest's per-function
// watchdog (QTEST_FUNCTION_TIMEOUT), so a slow one fails cleanly.
constexpr int kFloodTimeoutMs = 50000;

} // namespace
#endif // Q_OS_WIN

class TestConPtyProcess : public QObject {
    Q_OBJECT

private slots:
    void echoRoundTrip()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        Capture capture;
        ConPtyProcess pty;  // declared second, so stopped before capture dies
        QVERIFY2(pty.start(cmdExe(), capture.outputHandler(), capture.exitHandler()),
                 qPrintable(QString::fromStdWString(pty.errorString())));
        QVERIFY(pty.isRunning());
        QVERIFY(pty.resize(100, 30));

        QTRY_VERIFY2_WITH_TIMEOUT(promptShown(capture, pty), qPrintable(capture.text(pty)),
                                  kTimeoutMs);
        QVERIFY(pty.write("echo gitbolt-conpty-ok\r"));
        // The text comes back twice: echoed as part of the command line
        // and printed by it. Only the printed one lacks the "echo ".
        static const QRegularExpression printed(
            QStringLiteral("(?<!echo )gitbolt-conpty-ok"));
        QTRY_VERIFY2_WITH_TIMEOUT(capture.text(pty).contains(printed),
                                  qPrintable(capture.text(pty)), kTimeoutMs);

        const HANDLE shell = OpenProcess(SYNCHRONIZE, FALSE, pty.processId());
        QVERIFY(shell);
        QElapsedTimer timer;
        timer.start();
        const bool stopped = pty.stop(10000);
        const qint64 elapsed = timer.elapsed();
        const DWORD shellState = WaitForSingleObject(shell, 0);
        CloseHandle(shell);
        QVERIFY2(stopped, qPrintable(QStringLiteral("not torn down after %1 ms").arg(elapsed)));
        QCOMPARE(shellState, WAIT_OBJECT_0);  // the hang-up ended cmd.exe
        QVERIFY(!pty.isRunning());
        QVERIFY(pty.takeOutput().empty());

        // stop() detached the handlers before the exit was reported,
        // and nothing trickles in afterwards.
        const int calls = capture.calls();
        QTest::qWait(300);
        QCOMPARE(capture.calls(), calls);
        QVERIFY(!capture.exitCode().has_value());
#endif
    }

    void reportsShellExit()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        Capture capture;
        ConPtyProcess pty;
        QVERIFY2(pty.start(cmdExe(), capture.outputHandler(), capture.exitHandler()),
                 qPrintable(QString::fromStdWString(pty.errorString())));
        QTRY_VERIFY2_WITH_TIMEOUT(promptShown(capture, pty), qPrintable(capture.text(pty)),
                                  kTimeoutMs);

        QVERIFY(pty.write("exit 3\r"));
        QTRY_VERIFY2_WITH_TIMEOUT(capture.exitCode().has_value(),
                                  qPrintable(capture.text(pty)), kTimeoutMs);
        QCOMPARE(*capture.exitCode(), 3ul);
        QVERIFY(!pty.isRunning());
        QVERIFY(pty.stop(0));  // everything was already released
#endif
    }

    void runsLinesWrittenTogether()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        Capture capture;
        ConPtyProcess pty;
        QVERIFY2(pty.start(cmdExe(), capture.outputHandler(), capture.exitHandler()),
                 qPrintable(QString::fromStdWString(pty.errorString())));
        QTRY_VERIFY2_WITH_TIMEOUT(promptShown(capture, pty), qPrintable(capture.text(pty)),
                                  kTimeoutMs);

        // cmd reads the second line once the first has run.
        QVERIFY(pty.write("echo gitbolt-line-one\recho gitbolt-line-two\r"));
        static const QRegularExpression first(QStringLiteral("(?<!echo )gitbolt-line-one"));
        static const QRegularExpression second(QStringLiteral("(?<!echo )gitbolt-line-two"));
        QTRY_VERIFY2_WITH_TIMEOUT(capture.text(pty).contains(second),
                                  qPrintable(capture.text(pty)), kTimeoutMs);
        const QString text = capture.text(pty);
        const qsizetype firstAt = text.indexOf(first);
        QVERIFY2(firstAt >= 0 && firstAt < text.indexOf(second), qPrintable(text));
#endif
    }

    void floodIsHeldBackAndArrivesInOrder()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        Capture capture;
        ConPtyProcess pty;
        QVERIFY2(pty.start(cmdExe(), capture.outputHandler(), capture.exitHandler()),
                 qPrintable(QString::fromStdWString(pty.errorString())));
        QTRY_VERIFY2_WITH_TIMEOUT(promptShown(capture, pty), qPrintable(capture.text(pty)),
                                  kTimeoutMs);

        constexpr int kLines = 5000;
        QVERIFY(pty.write("for /L %i in (1,1,5000) do @echo gbflood%i\r"));
        // Take nothing for a while: the reader stops at the cap rather
        // than buffer the flood.
        QTest::qWait(1500);
        const qsizetype held = capture.take(pty);
        QVERIFY2(held <= static_cast<qsizetype>(ConPtyProcess::kMaxPendingOutput),
                 qPrintable(QStringLiteral("%1 bytes held").arg(held)));

        // Then take it as it comes: every line arrives, in order.
        int lines = 0;
        const auto settled = [&] {
            lines = floodProgress(capture.text(pty));
            return lines < 0 || lines == kLines;
        };
        const auto report = [&] {
            return QStringLiteral("%1 of %2 lines in order (negative: that line went "
                                  "missing)").arg(lines).arg(kLines);
        };
        QTRY_VERIFY2_WITH_TIMEOUT(settled(), qPrintable(report()), kFloodTimeoutMs);
        QVERIFY2(lines == kLines, qPrintable(report()));
        // A cue only comes when output finds nothing waiting, and only
        // a take empties the wait.
        QVERIFY2(capture.cues() <= capture.takes() + 1,
                 qPrintable(QStringLiteral("%1 cues for %2 takes")
                                .arg(capture.cues()).arg(capture.takes())));
#endif
    }

    void stopIsPromptMidFlood()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        Capture capture;
        ConPtyProcess pty;
        QVERIFY2(pty.start(cmdExe(), capture.outputHandler(), capture.exitHandler()),
                 qPrintable(QString::fromStdWString(pty.errorString())));
        QTRY_VERIFY2_WITH_TIMEOUT(promptShown(capture, pty), qPrintable(capture.text(pty)),
                                  kTimeoutMs);

        QVERIFY(pty.write(kEndlessFlood));
        QTest::qWait(800);  // nothing taken: the reader stops at the cap

        const HANDLE shell = OpenProcess(SYNCHRONIZE, FALSE, pty.processId());
        QVERIFY(shell);
        QElapsedTimer timer;
        timer.start();
        const bool stopped = pty.stop(10000);
        const qint64 elapsed = timer.elapsed();
        const DWORD shellState = WaitForSingleObject(shell, 0);
        CloseHandle(shell);
        QVERIFY2(stopped, qPrintable(QStringLiteral("not torn down after %1 ms").arg(elapsed)));
        QCOMPARE(shellState, WAIT_OBJECT_0);
        QVERIFY(pty.takeOutput().empty());  // what was held went with it

        const int calls = capture.calls();
        QTest::qWait(300);
        QCOMPARE(capture.calls(), calls);
#endif
    }

    void writeGetsThroughWhileHeldBack()
    {
#ifndef Q_OS_WIN
        QSKIP("ConPTY is the Windows terminal backend");
#else
        // On the heap, to be left behind should the write hang.
        auto capture = std::make_unique<Capture>();
        auto pty = std::make_unique<ConPtyProcess>();
        QVERIFY2(pty->start(cmdExe(), capture->outputHandler(), capture->exitHandler()),
                 qPrintable(QString::fromStdWString(pty->errorString())));
        QTRY_VERIFY2_WITH_TIMEOUT(promptShown(*capture, *pty), qPrintable(capture->text(*pty)),
                                  kTimeoutMs);

        QVERIFY(pty->write(kEndlessFlood));
        QTest::qWait(800);  // nothing taken: the reader stops at the cap

        // Far more than the input pipe holds, as a long paste would be,
        // written while nothing takes output: as on a GUI thread that
        // is busy writing. On its own thread, so a hang shows as a
        // failure instead of a hung test.
        const auto written = std::make_shared<std::atomic<int>>(-1);  // -1: still writing
        std::thread writer([process = pty.get(), written] {
            *written = process->write(std::string(64 * 1024, 'x')) ? 1 : 0;
        });
        QElapsedTimer timer;
        timer.start();
        while (*written < 0 && timer.elapsed() < kTimeoutMs)
            QTest::qWait(20);
        if (*written < 0) {
            // Stuck for good, and the threads still use both.
            writer.detach();
            static_cast<void>(pty.release());
            static_cast<void>(capture.release());
            QFAIL("write() blocked for good while the reader was held at the cap");
        }
        writer.join();
        QCOMPARE(written->load(), 1);

        QVERIFY(pty->write("\x03"));  // Ctrl+C ends the flood
        QVERIFY(pty->stop(10000));
#endif
    }
};

QTEST_GUILESS_MAIN(TestConPtyProcess)
#include "TestConPtyProcess.moc"
