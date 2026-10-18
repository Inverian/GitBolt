//
// TestMacServices — Finder's "Open in GitBolt" service provider
// (src/app/platform/macos/MacServices.h), called directly with the
// pasteboards Finder sends, on pasteboards of the test's own: nothing
// is registered with macOS and no service is performed. macOS only.
//

#include "MacServicesTestSupport.h"
#include "app/platform/macos/MacServices.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using gitbolt::test::ServiceCall;

namespace {

QStringList canonical(const QStringList& paths)
{
    QStringList out;
    for (const QString& path : paths)
        out << QFileInfo(path).canonicalFilePath();
    return out;
}

} // namespace

class TestMacServices : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (!gitbolt::test::pasteboardAvailable())
            QSKIP("no pasteboard server in this session");
    }

    // Finder's selection, in order, a name with a space included.
    void foldersAsFinderSendsThem() {
        QTemporaryDir tmp;
        QVERIFY(QDir(tmp.path()).mkpath(QStringLiteral("repo a/.git")));
        QVERIFY(QDir(tmp.path()).mkpath(QStringLiteral("repo-b")));
        const QStringList folders{QDir(tmp.path()).filePath(QStringLiteral("repo a")),
                                  QDir(tmp.path()).filePath(QStringLiteral("repo-b"))};

        const ServiceCall result = gitbolt::test::callProvider(folders);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.handlerCalled);
        // /var/folders is /private/var/folders: compare where they lead.
        QCOMPARE(canonical(result.folders), canonical(folders));
    }

    void oldFilenamesListToo() {
        QTemporaryDir tmp;
        const ServiceCall result = gitbolt::test::callProviderWithFilenames({tmp.path()});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(canonical(result.folders), canonical({tmp.path()}));
    }

    // Folders only: a file in the selection is left out.
    void filesAreLeftOut() {
        QTemporaryDir tmp;
        const QString file = QDir(tmp.path()).filePath(QStringLiteral("f.txt"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        const ServiceCall result = gitbolt::test::callProvider({file, tmp.path()});
        QVERIFY(result.handlerCalled);
        QCOMPARE(canonical(result.folders), canonical({tmp.path()}));
    }

    // Nothing to open: Finder is told why, and the handler never runs.
    void noFolderIsRefused() {
        QTemporaryDir tmp;
        const QString file = QDir(tmp.path()).filePath(QStringLiteral("f.txt"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        const ServiceCall result =
            gitbolt::test::callProvider({file}, {QStringLiteral("/some/text")});
        QVERIFY(!result.handlerCalled);
        QVERIFY(!result.error.isEmpty());
    }

    // Info.plist.in's NSMessage, "openInGitBolt", names the method
    // macOS calls (tests/app/CheckMacInfoPlist.cmake checks the key).
    void providerAnswersTheServicesMessage() {
        QVERIFY(gitbolt::test::providerAnswers(QStringLiteral("openInGitBolt")));
        QVERIFY(!gitbolt::test::providerAnswers(QStringLiteral("openInGitBoltX")));
    }

    // With no cocoa NSApp (this test has no QGuiApplication at all),
    // nothing is installed, and there is nothing to send to.
    void nothingOffCocoa() {
        gitbolt::app::macos::installServicesProvider([](const QStringList&) {});
        gitbolt::app::macos::activateApp();
        QString error;
        QVERIFY(!gitbolt::app::macos::sendToServicesProvider({QDir::tempPath()}, error));
        QVERIFY(!error.isEmpty());
        gitbolt::app::macos::installServicesProvider({});
    }
};

QTEST_GUILESS_MAIN(TestMacServices)
#include "TestMacServices.moc"
