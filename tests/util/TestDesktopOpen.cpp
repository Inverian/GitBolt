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
//     or follows (.fileloc, .url), whatever its case, may run when
//     opened; a text file, a plain folder (its executable bit is a
//     search bit) don't;
//   - but where the work tree keeps no modes (core.fileMode off, as git
//     sets it on an exFAT drive or WSL's /mnt/c), every file is marked
//     executable, and every one was asked about. There a marked file
//     may run if the index has it executable, or if the opener runs it
//     for the mark all the same: on macOS one with no extension, as
//     Launch Services finds it, or a .dylib (Terminal runs them), or a
//     shell script (iTerm2 does), elsewhere a program (an ELF binary, a
//     "#!" line, a shell script's name);
//   - the folder shown for a bundle is the nearest one above it that
//     isn't one (macOS); for a file, the folder it is in (Open in File
//     Explorer on a submodule whose path is an executable CHANGELOG
//     handed the desktop the file, which runs it); for a link, what it
//     leads to; any other folder is shown as it is;
//   - a path is in a folder by where it leads, not by how it is
//     spelled;
//   - but a submodule's path is judged as written first, before
//     anything looks on disk (on Windows //host/share is another
//     computer): one with a root, a drive, a UNC host or a ".." step
//     isn't in the work tree.
//

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

#include "util/DesktopOpen.h"

using namespace gitbolt;

namespace {

QString writeFile(const QTemporaryDir& dir, const QString& name, bool executable,
                  const QByteArray& content = "#!/bin/sh\necho marker\n") {
    const QString path = dir.filePath(name);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(content);
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

// The start of an ELF program (x86-64).
const QByteArray kElf("\x7f" "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0", 20);

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
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("README.md"), false))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("docs"))));
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(dir.filePath(QStringLiteral("docs")))));
    }

    // Types the platform's default app runs, or follows to what they
    // point to, though git checked them out at mode 644.
    void runnableTypes_data() {
        QTest::addColumn<QString>("name");
#if defined(Q_OS_MACOS)
        // Terminal: a session file runs the command it holds.
        for (const char* name : {"session.term", "SESSION.TERM", "run.tool", "dark.terminal",
                                 // JavaLauncher, the default app for them.
                                 "tool.jar", "Main.class", "app.jnlp",
                                 // Python Launcher, where it is theirs.
                                 "tool.py", "tool.pyw", "tool.pyc", "tool.pyo",
                                 // Finder, Safari, Screen Sharing, Mail...
                                 "a.fileloc", "a.inetloc", "a.webloc", "link.url",
                                 "share.afploc", "site.ftploc", "a.smbloc", "a.vncloc",
                                 "a.mailloc", "a.newsloc", "a.telnetloc"})
            QTest::newRow(name) << QString::fromLatin1(name);
#elif defined(Q_OS_WIN)
        for (const char* name : {"tool.exe", "run.cmd", "x.lnk", "x.url", "x.msi", "x.msp",
                                 "x.hta", "x.cpl", "x.chm", "x.jar", "x.reg", "x.appref-ms",
                                 "x.application", "x.settingcontent-ms", "x.library-ms",
                                 "x.search-ms", "x.scf", "x.theme", "x.themepack",
                                 "x.diagcab", "X.CPL", "x.appx", "x.appxbundle", "x.msix",
                                 "x.msixbundle", "x.appinstaller", "x.searchConnector-ms",
                                 // What an installed interpreter takes,
                                 // PATHEXT or not: Git Bash, Python's py
                                 // launcher, Ruby, Perl, Java Web Start.
                                 "install.sh", "build.bash", "tool.py", "tool.pyw",
                                 "tool.pyz", "tool.pyzw", "tool.pyc", "tool.pyo", "tool.rb",
                                 "tool.rbw", "tool.pl", "app.jnlp", "INSTALL.SH"})
            QTest::newRow(name) << QString::fromLatin1(name);
#else
        // Wine's desktop file takes Windows programs and installers.
        for (const char* name : {"run.desktop", "tool.jar", "app.jnlp", "setup.exe",
                                 "setup.msi", "run.bat", "SETUP.EXE"})
            QTest::newRow(name) << QString::fromLatin1(name);
#endif
    }

    void runnableTypes() {
        QFETCH(QString, name);
#if defined(Q_OS_WIN)
        // By the type alone: the runner's PATHEXT may list some of them.
        const QByteArray was = qgetenv("PATHEXT");
        const auto restore = qScopeGuard([&] { qputenv("PATHEXT", was); });
        qputenv("PATHEXT", ".EXE");
#endif
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(util::mayRunWhenOpened(QFileInfo(writeFile(dir, name, false))));
    }

#if defined(Q_OS_WIN)
    // What PATHEXT adds runs by name, and Windows' own list runs when
    // opened whatever PATHEXT says. The type is one only PATHEXT makes
    // run (Tcl's installer adds .tcl to it): .py, which this was, is on
    // GitBolt's own list now, and passed with PATHEXT ignored.
    void pathExtAddsToWindowsOwn() {
        const QByteArray was = qgetenv("PATHEXT");
        const auto restore = qScopeGuard([&] { qputenv("PATHEXT", was); });
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QFileInfo tcl(writeFile(dir, QStringLiteral("x.tcl"), false));
        qputenv("PATHEXT", ".EXE");
        QVERIFY(!util::mayRunWhenOpened(tcl));
        qputenv("PATHEXT", ".EXE;.TCL");
        QVERIFY(util::mayRunWhenOpened(tcl));
        QVERIFY(util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("x.bat"), false))));
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("x.txt"), false))));
    }
#endif

    // Where the work tree keeps no modes every file is marked
    // executable, so the mark tells nothing: on Linux every file on an
    // exFAT drive or under WSL's /mnt/c was asked about. The index's
    // mode tells, or what the opener runs all the same: Launch Services
    // a file with no extension and a .dylib, a terminal app a shell
    // script, xdg-open's openers a program.
    void markedWhereModesAreNotKept_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("head");
        QTest::addColumn<uint>("indexMode");
        QTest::addColumn<bool>("onMacOS");
        QTest::addColumn<bool>("elsewhere");
        const QByteArray text = "notes\n";
        const QByteArray script = "#!/bin/sh\necho marker\n";
        QTest::newRow("text") << QStringLiteral("notes.txt") << text << 0u << false << false;
        QTest::newRow("tracked text") << QStringLiteral("README.md") << text << 0100644u << false
                                      << false;
        QTest::newRow("source") << QStringLiteral("main.cpp") << QByteArray("int main() {}\n")
                                << 0100644u << false << false;
        QTest::newRow("committed executable") << QStringLiteral("release.txt") << text
                                              << 0100755u << true << true;
        // Launch Services: a Unix executable, which Terminal runs.
        QTest::newRow("no extension") << QStringLiteral("CHANGELOG") << text << 0100644u << true
                                      << false;
        QTest::newRow("a dot file") << QStringLiteral(".notes") << text << 0u << true << false;
        QTest::newRow("a trailing dot") << QStringLiteral("notes.") << text << 0u << true << false;
        QTest::newRow("a number's dot") << QStringLiteral("v1.2") << text << 0u << false << false;
        // Text after the last dot with a space in it is no extension
        // either; a tab, or anything else, leaves it one.
        QTest::newRow("a space after the dot") << QStringLiteral("Version 1.2 notes") << text
                                               << 0100644u << true << false;
        QTest::newRow("a trailing space") << QStringLiteral("build.txt ") << text << 0u << true
                                          << false;
        QTest::newRow("a tab after the dot") << QStringLiteral("notes.a\tb") << text << 0u
                                             << false << false;
        // A Unix executable to Launch Services too.
        QTest::newRow("a library") << QStringLiteral("x.dylib") << text << 0100644u << true
                                   << false;
        // xdg-open's openers: a program by its content or its name. On
        // macOS a terminal app can take a shell script, and runs it.
        QTest::newRow("#! line") << QStringLiteral("run.md") << script << 0100644u << false
                                 << true;
        QTest::newRow("ELF") << QStringLiteral("data.bin") << kElf << 0u << false << true;
        QTest::newRow("ELF, no extension") << QStringLiteral("tool") << kElf << 0u << true << true;
        QTest::newRow("shell script") << QStringLiteral("build.sh") << text << 0100644u << true
                                      << true;
        QTest::newRow("shell script, upper case") << QStringLiteral("BUILD.SH") << text << 0u
                                                  << true << true;
        // Runs by its type, mark or not.
        QTest::newRow("runs by its type") << runnableName() << text << 0u << true << true;
    }

    void markedWhereModesAreNotKept() {
        QFETCH(QString, name);
        QFETCH(QByteArray, head);
        QFETCH(uint, indexMode);
        QFETCH(bool, onMacOS);
        QFETCH(bool, elsewhere);
        util::FileToOpen file{name, true, false, indexMode, head};
#if defined(Q_OS_MACOS)
        Q_UNUSED(elsewhere);
        QCOMPARE(util::mayRunWhenOpened(file), onMacOS);
#else
        Q_UNUSED(onMacOS);
        QCOMPARE(util::mayRunWhenOpened(file), elsewhere);
#endif
        // Where the work tree keeps modes, the mark tells.
        file.modesKept = true;
        QVERIFY(util::mayRunWhenOpened(file));
    }

    // What xdg-open's openers take for a program, on every platform.
    void programsAreTold_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("head");
        QTest::addColumn<bool>("program");
        QTest::newRow("ELF") << QStringLiteral("tool") << kElf << true;
        QTest::newRow("ELF, any name") << QStringLiteral("notes.txt") << kElf << true;
        QTest::newRow("#!") << QStringLiteral("notes") << QByteArray("#!/bin/sh\necho marker\n")
                            << true;
        QTest::newRow("#!, any name") << QStringLiteral("notes.md")
                                      << QByteArray("#!/usr/bin/env python3\n") << true;
        QTest::newRow(".sh") << QStringLiteral("build.sh") << QByteArray("echo marker\n") << true;
        QTest::newRow(".desktop") << QStringLiteral("run.desktop")
                                  << QByteArray("[Desktop Entry]\n") << true;
        QTest::newRow("text") << QStringLiteral("notes") << QByteArray("notes\n") << false;
        QTest::newRow("empty") << QStringLiteral("notes.txt") << QByteArray() << false;
        QTest::newRow("markdown") << QStringLiteral("README.md") << QByteArray("# Notes\n")
                                  << false;
        QTest::newRow("almost ELF") << QStringLiteral("notes") << QByteArray("\x7f" "ELX\n")
                                    << false;
    }

    void programsAreTold() {
        QFETCH(QString, name);
        QFETCH(QByteArray, head);
        QFETCH(bool, program);
        QCOMPARE(util::isProgram(name, head), program);
    }

#ifndef Q_OS_WIN
    // The same from the files themselves, read from disk where their
    // content tells.
    void markedFilesWhereModesAreNotKept() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QFileInfo notes(writeFile(dir, QStringLiteral("notes.txt"), true, "notes\n"));
        const QFileInfo changelog(writeFile(dir, QStringLiteral("CHANGELOG"), true, "notes\n"));
        const QFileInfo script(writeFile(dir, QStringLiteral("run.md"), true));
        const QFileInfo elf(writeFile(dir, QStringLiteral("data.bin"), true, kElf));
        QVERIFY(!util::mayRunWhenOpened(notes, false, 0));
        QVERIFY(!util::mayRunWhenOpened(notes, false, 0100644));
        QVERIFY(util::mayRunWhenOpened(notes, false, 0100755));
        QVERIFY(util::mayRunWhenOpened(notes));
#if defined(Q_OS_MACOS)
        QVERIFY(util::mayRunWhenOpened(changelog, false, 0));
        QVERIFY(!util::mayRunWhenOpened(script, false, 0));
        QVERIFY(!util::mayRunWhenOpened(elf, false, 0));
#else
        QVERIFY(!util::mayRunWhenOpened(changelog, false, 0));
        QVERIFY(util::mayRunWhenOpened(script, false, 0));
        QVERIFY(util::mayRunWhenOpened(elf, false, 0));
#endif
        // A file that isn't marked isn't run for a mark: a drive mounted
        // so that no file is.
        QVERIFY(!util::mayRunWhenOpened(QFileInfo(writeFile(dir, QStringLiteral("tool"), false,
                                                            kElf)),
                                        false, 0100755));
    }
#endif

    void folderShownForABundleIsTheOneItIsIn() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // What is shown is a real path: the temporary folder may be
        // reached through a link (/var on macOS).
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
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

    // Only ever a folder: a file's is the folder it is in, an
    // executable one's too, and one that isn't there the nearest above.
    void folderShownForAFileIsTheOneItIsIn() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        const QString changelog = writeFile(dir, QStringLiteral("CHANGELOG"), true);
        const QString build = writeFile(dir, QStringLiteral("tools/build"), true);
        QVERIFY(!changelog.isEmpty() && !build.isEmpty());

        QCOMPARE(util::folderToShow(changelog), root);
        QCOMPARE(util::folderToShow(build), root + QStringLiteral("/tools"));
        QCOMPARE(util::folderToShow(root + QStringLiteral("/tools/gone/away")),
                 root + QStringLiteral("/tools"));
#ifdef Q_OS_MACOS
        const QString inApp = writeFile(dir, QStringLiteral("x/Tool.app/CHANGELOG"), true);
        QVERIFY(!inApp.isEmpty());
        QCOMPARE(util::folderToShow(inApp), root + QStringLiteral("/x"));
#endif
    }

#ifndef Q_OS_WIN
    // The desktop opens what a link leads to: a link to a script shows
    // the script's folder, and one to Tool.app the folder that is in,
    // not the link (which the desktop follows, and launches the app).
    void folderShownForALinkIsWhereItLeads() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        QVERIFY(!writeFile(dir, QStringLiteral("tools/CHANGELOG"), true).isEmpty());
        QVERIFY(QDir().mkpath(root + QStringLiteral("/x/Tool.app/Contents/MacOS")));
        QVERIFY(QDir().mkpath(root + QStringLiteral("/docs")));
        QVERIFY(QFile::link(root + QStringLiteral("/tools/CHANGELOG"),
                            root + QStringLiteral("/notes")));
        QVERIFY(QFile::link(root + QStringLiteral("/x/Tool.app"), root + QStringLiteral("/sub")));
        QVERIFY(QFile::link(root + QStringLiteral("/docs"), root + QStringLiteral("/linkdir")));

        QCOMPARE(util::folderToShow(root + QStringLiteral("/notes")),
                 root + QStringLiteral("/tools"));
        QCOMPARE(util::folderToShow(root + QStringLiteral("/linkdir")),
                 root + QStringLiteral("/docs"));
#ifdef Q_OS_MACOS
        QVERIFY(!QFileInfo(root + QStringLiteral("/sub")).isBundle());
        QCOMPARE(util::folderToShow(root + QStringLiteral("/sub")), root + QStringLiteral("/x"));
#else
        QCOMPARE(util::folderToShow(root + QStringLiteral("/sub")),
                 root + QStringLiteral("/x/Tool.app"));
#endif
    }
#endif

    // A submodule's path is the repository's to write, and on Windows
    // //host/share/x is another computer's folder: whatever looks at
    // it on disk connects to that host. So it is judged as written
    // first, with the same rule on every platform.
    void relativeInsideAsWritten_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("inside");
        QTest::newRow("a folder") << QStringLiteral("sub") << true;
        QTest::newRow("a folder in one") << QStringLiteral("libs/sub/") << true;
        QTest::newRow("backslashes") << QStringLiteral("libs\\sub") << true;
        QTest::newRow("dot steps") << QStringLiteral("./libs/./sub") << true;
        QTest::newRow("dots in names") << QStringLiteral("a..b/...") << true;
        QTest::newRow("empty") << QString() << false;
        QTest::newRow("absolute") << QStringLiteral("/etc") << false;
        QTest::newRow("from the drive's root") << QStringLiteral("\\Windows") << false;
        QTest::newRow("UNC") << QStringLiteral("//host/share/x") << false;
        QTest::newRow("UNC, backslashes") << QStringLiteral("\\\\host\\share\\x") << false;
        QTest::newRow("UNC, both slashes") << QStringLiteral("\\/host/share") << false;
        QTest::newRow("a device") << QStringLiteral("\\\\?\\C:\\x") << false;
        QTest::newRow("a drive") << QStringLiteral("C:/Windows") << false;
        QTest::newRow("a drive, backslash") << QStringLiteral("c:\\Windows") << false;
        QTest::newRow("a drive's folder") << QStringLiteral("C:Windows") << false;
        QTest::newRow("a drive alone") << QStringLiteral("C:") << false;
        QTest::newRow("a Qt resource") << QStringLiteral(":/x") << false;
        QTest::newRow("up") << QStringLiteral("../x") << false;
        QTest::newRow("up alone") << QStringLiteral("..") << false;
        QTest::newRow("up and back in") << QStringLiteral("sub/../sub") << false;
        QTest::newRow("up, backslashes") << QStringLiteral("a\\..\\..\\x") << false;
        QTest::newRow("up at the end") << QStringLiteral("a/..") << false;
    }

    void relativeInsideAsWritten() {
        QFETCH(QString, path);
        QFETCH(bool, inside);
        QCOMPARE(util::isRelativeInside(path), inside);
    }

    void withinAFolderIsWhereItLeads() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = QFileInfo(dir.path()).canonicalFilePath();
        const QString repo = root + QStringLiteral("/repo");
        QVERIFY(!writeFile(dir, QStringLiteral("repo/a/notes.txt"), false).isEmpty());
        QVERIFY(!writeFile(dir, QStringLiteral("repo-other/notes.txt"), false).isEmpty());

        QVERIFY(util::isWithinFolder(repo, repo));
        QVERIFY(util::isWithinFolder(repo + QStringLiteral("/a/notes.txt"), repo));
        QVERIFY(util::isWithinFolder(repo + QStringLiteral("/a/../a/notes.txt"),
                                     repo + QLatin1Char('/')));
        // Not by its spelling: a sibling whose name starts the same,
        // a path up and out, one that isn't there.
        QVERIFY(!util::isWithinFolder(root + QStringLiteral("/repo-other/notes.txt"), repo));
        QVERIFY(!util::isWithinFolder(repo + QStringLiteral("/../repo-other/notes.txt"), repo));
        QVERIFY(!util::isWithinFolder(root, repo));
        QVERIFY(!util::isWithinFolder(repo + QStringLiteral("/gone"), repo));
        QVERIFY(!util::isWithinFolder(repo, root + QStringLiteral("/gone")));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(root + QStringLiteral("/repo-other"), repo + QStringLiteral("/out")));
        QVERIFY(!util::isWithinFolder(repo + QStringLiteral("/out/notes.txt"), repo));
        QVERIFY(QFile::link(repo, root + QStringLiteral("/repo-link")));
        QVERIFY(util::isWithinFolder(repo + QStringLiteral("/a/notes.txt"),
                                     root + QStringLiteral("/repo-link")));
#endif
    }
};

QTEST_MAIN(TestDesktopOpen)
#include "TestDesktopOpen.moc"
