#pragma once
//
// Stand-ins for installed GitBolts, for the updater's installer tests:
// a "GitBolt" that is a shell script appending a line to the file
// $GITBOLT_TEST_MARKER names (which copy it is, its pid and process
// group, the instance name it was given), a bundle laid out like the
// release's around it, and (macOS) its ad-hoc signature and a disk
// image made from it. Everything lives in the caller's temporary
// folders: nothing here touches /Applications, the real Trash or an
// installed GitBolt.
//

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTest>

namespace gitbolt::test {

inline bool writeFile(const QString& path, const QByteArray& content, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(content) != content.size())
        return false;
    file.close();
    QFile::Permissions mode = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
    if (executable)
        mode |= QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
    return file.setPermissions(mode);
}

inline QString readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

/// A GitBolt that only says it ran: one line per start, appended to
/// $GITBOLT_TEST_MARKER, "<label> pid=<pid> pgid=<group> instance=<name>".
inline QByteArray fakeAppScript(const QString& label) {
    return QByteArrayLiteral("#!/bin/sh\n"
                             "printf '%s pid=%s pgid=%s instance=%s\\n' '")
           + label.toUtf8()
           + QByteArrayLiteral("' \"$$\" \"$(ps -o pgid= -p $$ | tr -d ' ')\" \"${GITBOLT_INSTANCE_NAME:-}\" "
                               ">>\"$GITBOLT_TEST_MARKER\"\n");
}

inline QByteArray infoPlist(const QString& id, const QString& version) {
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                          "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                          "<plist version=\"1.0\">\n<dict>\n"
                          "  <key>CFBundleExecutable</key><string>GitBolt</string>\n"
                          "  <key>CFBundleIdentifier</key><string>%1</string>\n"
                          "  <key>CFBundleName</key><string>GitBolt</string>\n"
                          "  <key>CFBundlePackageType</key><string>APPL</string>\n"
                          "  <key>CFBundleShortVersionString</key><string>%2</string>\n"
                          "  <key>CFBundleVersion</key><string>%2</string>\n"
                          "</dict>\n</plist>\n")
        .arg(id, version)
        .toUtf8();
}

/// …/GitBolt.app laid out like the release's: Info.plist, the program
/// in Contents/MacOS, Contents/Frameworks (which macdeployqt makes and
/// detectPackage looks for), and Contents/Resources/copy.txt saying
/// which copy it is. `label` defaults to the version.
inline bool makeBundle(const QString& bundle, const QString& version,
                       const QString& id = QStringLiteral("com.gitbolt.app"), QString label = {}) {
    if (label.isEmpty())
        label = version;
    const QDir contents(bundle + QStringLiteral("/Contents"));
    return QDir().mkpath(contents.filePath(QStringLiteral("Frameworks")))
           && writeFile(contents.filePath(QStringLiteral("Info.plist")), infoPlist(id, version))
           && writeFile(contents.filePath(QStringLiteral("MacOS/GitBolt")), fakeAppScript(label), true)
           && writeFile(contents.filePath(QStringLiteral("Resources/copy.txt")), label.toUtf8() + '\n');
}

/// Runs a tool to the end; its exit code, or -1. Output to `output`.
inline int runTool(const QString& program, const QStringList& arguments, QString* output = nullptr,
                   int timeoutMs = 60000) {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(program, arguments);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        return -1;
    }
    if (output)
        *output = QString::fromLocal8Bit(process.readAll());
    return process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
}

/// macOS: an ad-hoc signature, as the release's bundle has.
inline bool adHocSign(const QString& bundle) {
    return runTool(QStringLiteral("/usr/bin/codesign"),
                   {QStringLiteral("--force"), QStringLiteral("--sign"), QStringLiteral("-"), bundle})
           == 0;
}

/// macOS: a disk image of `folder`, compressed like the release's
/// (UDBZ). hdiutil's intermittent "Resource busy" is retried.
inline bool makeDiskImage(const QString& folder, const QString& dmg) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        QFile::remove(dmg);
        QString output;
        if (runTool(QStringLiteral("/usr/bin/hdiutil"),
                    {QStringLiteral("create"), QStringLiteral("-quiet"), QStringLiteral("-fs"), QStringLiteral("HFS+"),
                     QStringLiteral("-format"), QStringLiteral("UDBZ"), QStringLiteral("-volname"),
                     QStringLiteral("GitBoltTest"), QStringLiteral("-srcfolder"), folder, dmg},
                    &output, 120000)
            == 0)
            return true;
        qWarning("hdiutil create failed (attempt %d): %s", attempt, qPrintable(output));
        QTest::qWait(attempt * 1000);
    }
    return false;
}

/// Waits, turning the event loop, until `path` has `lines` lines.
inline bool waitForLines(const QString& path, int lines, int timeoutMs) {
    const QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        if (readFile(path).count(u'\n') >= lines)
            return true;
        QTest::qWait(50);
    }
    return readFile(path).count(u'\n') >= lines;
}

} // namespace gitbolt::test
