//
// TestLogFile — the log file (util/LogFile.h):
//
//   - rotation: at most `files` files, none past the cap, the newest
//     lines in <base>.log, the oldest file gone; an oversized file
//     from an earlier session rotates at open
//   - lines from many threads at once: every one intact, none lost
//   - two logs on one file (two sessions): every line of both kept
//   - the message handler: lines with a time, the level and the
//     category, every message forwarded to the handler before it, and
//     that handler back once it is uninstalled
//   - a folder name with non-ASCII letters
//   - a rename that fails (Windows: another GitBolt has the file
//     open) keeps the session appending, with no retry per line, and
//     the older files as they were; the next open rotates
//   - logDirectory() on each platform
//

#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include "util/LogFile.h"

#include <memory>
#include <vector>

using gitbolt::util::RotatingLog;

namespace {

Q_LOGGING_CATEGORY(lcTest, "gitbolt.test")

QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QList<QByteArray> linesOf(const QString& path) {
    QList<QByteArray> lines = readAll(path).split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

QByteArray numbered(int n) {
    return QByteArray("line ") + QByteArray::number(n).rightJustified(4, '0');
}

// Changes an environment variable until the guard goes.
auto setEnv(const char* name, const QByteArray& value) {
    const bool had = qEnvironmentVariableIsSet(name);
    const QByteArray old = qgetenv(name);
    qputenv(name, value);
    return qScopeGuard([=] {
        if (had)
            qputenv(name, old);
        else
            qunsetenv(name);
    });
}

} // namespace

class TestLogFile : public QObject {
    Q_OBJECT

private slots:
    void rotationKeepsTheNewestFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RotatingLog log(dir.path(), QStringLiteral("GitBolt"), 100, 3);
        QCOMPARE(log.filePath(), dir.filePath(QStringLiteral("GitBolt.log")));
        for (int i = 0; i < 40; ++i)
            log.append(numbered(i));

        const QStringList names = QDir(dir.path()).entryList(QDir::Files, QDir::Name);
        QCOMPARE(names, (QStringList{QStringLiteral("GitBolt.1.log"), QStringLiteral("GitBolt.2.log"),
                                     QStringLiteral("GitBolt.log")}));
        // Oldest first: .2, .1, then the newest; consecutive lines, the
        // last one last, and line 0 long gone.
        QList<QByteArray> all;
        for (const char* name : {"GitBolt.2.log", "GitBolt.1.log", "GitBolt.log"}) {
            const QString path = dir.filePath(QString::fromLatin1(name));
            QVERIFY2(QFileInfo(path).size() <= 100, name);
            all += linesOf(path);
        }
        QCOMPARE(all.last(), numbered(39));
        for (qsizetype i = 1; i < all.size(); ++i)
            QCOMPARE(all.at(i), numbered(39 - int(all.size() - 1 - i)));
        QVERIFY(all.first() != numbered(0));
        QVERIFY(QFileInfo(log.filePath()).permissions().testFlag(QFile::ReadOwner));
#ifndef Q_OS_WIN
        QVERIFY(!QFileInfo(log.filePath()).permissions().testFlag(QFile::ReadOther));
#endif
    }

    void anOversizedFileRotatesAtOpen() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray earlier(150, 'x');
        {
            QFile file(dir.filePath(QStringLiteral("GitBolt.log")));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(earlier), earlier.size());
        }
        RotatingLog log(dir.path(), QStringLiteral("GitBolt"), 100, 3);
        QCOMPARE(readAll(dir.filePath(QStringLiteral("GitBolt.1.log"))), earlier);
        log.append("fresh");
        QCOMPARE(readAll(log.filePath()), QByteArray("fresh\n"));

        // One under the cap is appended to.
        RotatingLog again(dir.path(), QStringLiteral("GitBolt"), 100, 3);
        again.append("more");
        QCOMPARE(readAll(log.filePath()), QByteArray("fresh\nmore\n"));
        QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("GitBolt.2.log"))));
    }

    void linesFromManyThreadsStayWhole() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RotatingLog log(dir.path(), QStringLiteral("GitBolt"), 64 * 1024 * 1024, 3);
        constexpr int kThreads = 8;
        constexpr int kLines = 500;
        std::vector<std::unique_ptr<QThread>> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back(QThread::create([&log, t] {
                for (int i = 0; i < kLines; ++i)
                    log.append(QByteArray("thread ") + QByteArray::number(t) + " line "
                               + QByteArray::number(i) + ' ' + QByteArray(40, char('a' + t)));
            }));
            threads.back()->start();
        }
        for (auto& thread : threads)
            QVERIFY(thread->wait(30000));

        static const QRegularExpression whole(
            QStringLiteral("^thread (\\d) line (\\d+) ([a-h])\\3{39}$"));
        QSet<QString> seen;
        for (const QByteArray& line : linesOf(log.filePath())) {
            const QRegularExpressionMatch m = whole.match(QString::fromLatin1(line));
            QVERIFY2(m.hasMatch(), line.constData());
            QCOMPARE(int(m.captured(3).at(0).unicode()), 'a' + m.captured(1).toInt());
            seen.insert(m.captured(1) + QLatin1Char('/') + m.captured(2));
        }
        QCOMPARE(seen.size(), kThreads * kLines);
    }

    // Two sessions on one file (Windows: every test session's
    // GitBolt-test.log). Both opened at the same size: each line must
    // still land at the end, not over the other's.
    void twoLogsOnOneFileKeepEveryLine() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RotatingLog first(dir.path(), QStringLiteral("GitBolt"));
        RotatingLog second(dir.path(), QStringLiteral("GitBolt"));
        QList<QByteArray> expected;
        for (int i = 0; i < 20; ++i) {
            expected << QByteArray("first ") + QByteArray(i % 7 + 1, 'a') << numbered(i);
            first.append(expected.at(expected.size() - 2));
            second.append(expected.last());
        }
        QCOMPARE(linesOf(first.filePath()), expected);
    }

    // Every message goes to the file and on to the handler installed
    // before (QtTest's here: ignoreMessage sees it only that way).
    void theHandlerWritesAndForwards() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RotatingLog log(dir.path(), QStringLiteral("GitBolt"));
        const QtMessageHandler before = qInstallMessageHandler(nullptr);
        qInstallMessageHandler(before);

        gitbolt::util::installMessageLog(&log);
        auto installed = qScopeGuard([] { gitbolt::util::uninstallMessageLog(); });
        QTest::ignoreMessage(QtWarningMsg, "a warning for the log");
        qWarning("a warning for the log");
        QTest::ignoreMessage(QtWarningMsg, "with a category");
        qCWarning(lcTest, "with a category");
        QTest::ignoreMessage(QtInfoMsg, "some news");
        qInfo("some news");
        installed.dismiss();
        gitbolt::util::uninstallMessageLog();

        // The handler before it is back, and the file hears no more.
        const QtMessageHandler after = qInstallMessageHandler(nullptr);
        qInstallMessageHandler(after);
        QVERIFY(after == before);
        QTest::ignoreMessage(QtWarningMsg, "not for the log");
        qWarning("not for the log");

        static const QRegularExpression stamp(QStringLiteral(
            "^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}(Z|[+-]\\d\\d:\\d\\d) "));
        const QList<QByteArray> lines = linesOf(log.filePath());
        QCOMPARE(lines.size(), 3);
        QStringList rest;
        for (const QByteArray& line : lines) {
            const QString text = QString::fromUtf8(line);
            const QRegularExpressionMatch m = stamp.match(text);
            QVERIFY2(m.hasMatch(), line.constData());
            rest << text.mid(m.capturedLength());
        }
        QCOMPARE(rest, (QStringList{QStringLiteral("W a warning for the log"),
                                    QStringLiteral("W gitbolt.test with a category"),
                                    QStringLiteral("I some news")}));
    }

    void aNonAsciiFolder() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.filePath(QStringLiteral("J\u00f6rg \u0416 \u738b/logs"));
        RotatingLog log(folder, QStringLiteral("GitBolt"));
        log.append(QStringLiteral("\u738b").toUtf8());
        QCOMPARE(readAll(folder + QStringLiteral("/GitBolt.log")),
                 QStringLiteral("\u738b\n").toUtf8());
    }

    // A rename that fails leaves the session appending to the file it
    // has, past the cap, with no retry per line, and the older files
    // as they were. The next open rotates. Windows: another process has
    // the file open (two test sessions share GitBolt-test.log).
    // Elsewhere: a folder it can't change.
    void aFailedRenameKeepsAppending() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString folder = dir.filePath(QStringLiteral("logs"));
        const auto linesIn = [&](const char* name) {
            return linesOf(folder + QLatin1Char('/') + QString::fromLatin1(name));
        };
        const auto range = [](int from, int to) {
            QList<QByteArray> lines;
            for (int i = from; i < to; ++i)
                lines << numbered(i);
            return lines;
        };
        // Ten 10-byte lines a file: .2 has 0-9, .1 10-19.
        auto log = std::make_unique<RotatingLog>(folder, QStringLiteral("GitBolt"), 100, 3);
        for (int i = 0; i < 25; ++i)
            log->append(numbered(i));
        QCOMPARE(linesIn("GitBolt.2.log"), range(0, 10));
        {
#ifdef Q_OS_WIN
            QFile holder(log->filePath());
            QVERIFY(holder.open(QIODevice::ReadOnly));
#else
            const QFile::Permissions writable = QFileInfo(folder).permissions();
            QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
            const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
            if (QFile(folder + QStringLiteral("/probe")).open(QIODevice::WriteOnly))
                QSKIP("the folder stays writable (root?)");
#endif
            for (int i = 25; i < 45; ++i)
                log->append(numbered(i));
        }
        // Released again: still no rotation in this session.
        for (int i = 45; i < 55; ++i)
            log->append(numbered(i));
        QCOMPARE(QDir(folder).entryList(QDir::Files, QDir::Name),
                 (QStringList{QStringLiteral("GitBolt.1.log"), QStringLiteral("GitBolt.2.log"),
                              QStringLiteral("GitBolt.log")}));
        QCOMPARE(linesIn("GitBolt.2.log"), range(0, 10));
        QCOMPARE(linesIn("GitBolt.1.log"), range(10, 20));
        QCOMPARE(linesIn("GitBolt.log"), range(20, 55));

        // The next session's log: this one has closed its file first.
        log.reset();
        log = std::make_unique<RotatingLog>(folder, QStringLiteral("GitBolt"), 100, 3);
        QCOMPARE(linesIn("GitBolt.2.log"), range(10, 20));
        QCOMPARE(linesIn("GitBolt.1.log"), range(20, 55));
        log->append("next session");
        QCOMPARE(linesOf(log->filePath()), QList<QByteArray>{QByteArray("next session")});
    }

    void logDirectoryPerPlatform() {
#if defined(Q_OS_MACOS)
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const auto restore = setEnv("HOME", home.path().toUtf8());
        QCOMPARE(gitbolt::util::logDirectory(), home.path() + QStringLiteral("/Library/Logs/GitBolt"));
#elif defined(Q_OS_WIN)
        QCOMPARE(gitbolt::util::logDirectory(),
                 QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                     + QStringLiteral("/GitBolt/logs"));
        QVERIFY(gitbolt::util::logDirectory().contains(QStringLiteral("AppData/Local")));
#else
        QTemporaryDir home;
        QVERIFY(home.isValid());
        const auto restoreHome = setEnv("HOME", home.path().toUtf8());
        {
            const auto state = setEnv("XDG_STATE_HOME", home.filePath(QStringLiteral("state")).toUtf8());
            QCOMPARE(gitbolt::util::logDirectory(), home.filePath(QStringLiteral("state/GitBolt")));
        }
        {
            const auto state = setEnv("XDG_STATE_HOME", "relative/state");
            QCOMPARE(gitbolt::util::logDirectory(), home.path() + QStringLiteral("/.local/state/GitBolt"));
        }
        {
            const auto state = setEnv("XDG_STATE_HOME", "");
            QCOMPARE(gitbolt::util::logDirectory(), home.path() + QStringLiteral("/.local/state/GitBolt"));
        }
#endif
    }
};

QTEST_GUILESS_MAIN(TestLogFile)
#include "TestLogFile.moc"
