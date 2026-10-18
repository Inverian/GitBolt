//
// TestDesktopOpen — what GitBolt checks before it hands a path in a
// work tree to the desktop (util/DesktopOpen.h).
//
// Open Externally handed the desktop any file in the work tree, and
// on macOS an executable CHANGELOG opens in Terminal, which runs it;
// Open in File Explorer handed it any folder, and a folder named
// Tool.app is launched as an app. Now:
//
//   - a file marked executable, or of a type the platform's opener
//     runs (.command on macOS, .bat on Windows, .desktop elsewhere),
//     whatever its case, may run when opened; a text file, a plain
//     folder (its executable bit is a search bit) don't;
//   - the folder shown for a bundle is the nearest one above it that
//     isn't one (macOS); any other folder is shown as it is.
//

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include "util/DesktopOpen.h"

using namespace gitbolt;

namespace {

QString writeFile(const QTemporaryDir& dir, const QString& name, bool executable) {
    const QString path = dir.filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write("#!/bin/sh\necho marker\n");
    file.close();
    QFile::Permissions mode = QFile::ReadOwner | QFile::WriteOwner;
    if (executable)
        mode |= QFile::ExeOwner;
    file.setPermissions(mode);
    return path;
}

// A name the platform's opener runs, or follows, by its type alone.
QString runnableName() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("RUN.Command");
#elif defined(Q_OS_WIN)
    return QStringLiteral("Run.BAT");
#else
    return QStringLiteral("run.Desktop");
#endif
}

} // namespace

class TestDesktopOpen : public QObject {
    Q_OBJECT

private slots:
    void runnableFilesAreTold() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
#ifndef Q_OS_WIN
        // Windows has no executable bit: there the type says it.
        QVERIFY(util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("CHANGELOG"), true))));
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("TODO"), false))));
#endif
        QVERIFY(util::mayRunWhenOpened(QFileInfo(writeFile(dir, runnableName(), false))));
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("notes.txt"), false))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("docs"))));
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(dir.filePath(QStringLiteral("docs")))));
    }

    void folderShownForABundleIsTheOneItIsIn() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = QDir::cleanPath(dir.path());
        const QString docs = root + QStringLiteral("/docs");
        const QString app = root + QStringLiteral("/Tool.app");
        const QString nested = app + QStringLiteral("/Nested.app");
        const QString contents = app + QStringLiteral("/Contents");
        const QString helper = contents + QStringLiteral("/Helper.app");
        const QString macos = contents + QStringLiteral("/MacOS");
        QVERIFY(QDir().mkpath(docs) && QDir().mkpath(nested) && QDir().mkpath(helper)
                && QDir().mkpath(macos));

        QCOMPARE(util::folderToShow(docs), docs);
        QCOMPARE(util::folderToShow(docs + QLatin1Char('/')), docs);
        // A folder in a bundle is an ordinary one: Finder shows it.
        QCOMPARE(util::folderToShow(macos), macos);
#ifdef Q_OS_MACOS
        QVERIFY(util::mayRunWhenOpened(QFileInfo(app)));
        QCOMPARE(util::folderToShow(app), root);
        QCOMPARE(util::folderToShow(nested), root);
        QCOMPARE(util::folderToShow(helper), contents);
#else
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(app)));
        QCOMPARE(util::folderToShow(app), app);
        QCOMPARE(util::folderToShow(nested), nested);
        QCOMPARE(util::folderToShow(helper), helper);
#endif
    }
};

QTEST_MAIN(TestDesktopOpen)
#include "TestDesktopOpen.moc"
