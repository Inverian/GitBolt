//
// TestAboutDialog — Help › About is never too short for what it shows.
//
// A top-level window's minimum size ignores height-for-width, so the
// dialog's minimum height came out about 45 px short of what its
// word-wrapped labels need at its minimum width. Dragged that small,
// the build-info table was clipped; and on a short screen it opened
// clipped, because Qt caps a new window at 2/3 of the screen's height
// and only the minimum bounds that cap.
//
// Runs off screen on a 1093x614 screen (tests/fixtures/short-screen.json:
// 1366x768 at 125 %, a common laptop) with the app's .qrc linked, so the
// dialog shows its real 128 px icon.
//
// And it carries the notices the LGPL asks of an app that shows copyright
// notices (LGPL-3.0 4(c) for Qt, LGPL-2.1 section 6 for the LibXDiff in
// libgit2), with links to the licence texts GitBolt's package installs
// and to About Qt, and a link to the releases page. Its links work from
// the keyboard.
//

#include <QApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QLabel>
#include <QLayout>
#include <QScreen>
#include <QTest>
#include <QUrl>

#include "dialogs/AboutDialog.h"
#include "git/GitProcess.h"
#include "git/GitVersion.h"
#include "../TestProcessHelper.h"

using gitbolt::dialogs::AboutDialog;

namespace {

// The labels of `dlg` shorter than they need to be at their current
// width: wrapping labels need their height-for-width, the rest their
// size hint. Empty when nothing is clipped.
QStringList clippedLabels(const AboutDialog& dlg) {
    QStringList clipped;
    for (const QLabel* label : dlg.findChildren<QLabel*>()) {
        const int needs = label->hasHeightForWidth()
            ? label->heightForWidth(label->width())
            : label->sizeHint().height();
        if (label->height() < needs) {
            clipped << QStringLiteral("%1 px of %2 for \"%3\"")
                           .arg(label->height()).arg(needs)
                           .arg(label->text().left(60));
        }
    }
    return clipped;
}

// The label with the third-party notices.
QLabel* noticeLabel(const AboutDialog& dlg) {
    for (QLabel* label : dlg.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("LibXDiff")))
            return label;
    }
    return nullptr;
}

// The label with the links row (Website, Releases, ...).
QLabel* linksLabel(const AboutDialog& dlg) {
    for (QLabel* label : dlg.findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral(">Releases</a>")))
            return label;
    }
    return nullptr;
}

// Takes the URLs QDesktopServices would hand the desktop.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    UrlCatcher() {
        for (const QString& scheme : schemes())
            QDesktopServices::setUrlHandler(scheme, this, "open");
    }
    ~UrlCatcher() override {
        for (const QString& scheme : schemes())
            QDesktopServices::unsetUrlHandler(scheme);
    }
    QList<QUrl> urls;

public slots:
    void open(const QUrl& url) { urls << url; }

private:
    static QStringList schemes() {
        return {QStringLiteral("file"), QStringLiteral("http"), QStringLiteral("https")};
    }
};

} // namespace

class TestAboutDialog : public QObject {
    Q_OBJECT

private slots:
    void minimumHeightCoversTheWrappedText() {
        AboutDialog dlg;
        QVERIFY(dlg.layout());
        // What show() does first: the layout sets the minimum size it
        // computes wherever the dialog set none of its own.
        dlg.layout()->activate();
        const int needed = dlg.layout()->totalHeightForWidth(dlg.minimumWidth());
        QVERIFY2(dlg.minimumHeight() >= needed,
                 qPrintable(QStringLiteral("minimum height %1 < %2, what the "
                                           "labels need at the minimum width %3")
                                .arg(dlg.minimumHeight()).arg(needed)
                                .arg(dlg.minimumWidth())));
        // A minimum, not a fixed size: the dialog stays resizable.
        QVERIFY(dlg.maximumHeight() > dlg.minimumHeight());
        QVERIFY(dlg.maximumWidth() > dlg.minimumWidth());
    }

    void smallestSizeClipsNoLabel() {
        AboutDialog dlg;
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        // As small as it goes: resize() stops at the minimum.
        dlg.resize(dlg.minimumWidth(), 1);
        QTRY_COMPARE(dlg.height(), dlg.minimumHeight());
        const QStringList clipped = clippedLabels(dlg);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral("; "))));
    }

    void opensUnclippedOnAShortScreen() {
        // The fixture's screen, or this proves nothing: on a tall one
        // the 2/3 cap never bites.
        const QScreen* screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QCOMPARE(screen->geometry().size(), QSize(1093, 614));

        AboutDialog dlg;
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        QVERIFY2(dlg.height() <= screen->availableGeometry().height(),
                 "the dialog no longer fits the screen; the fixture is too short");
        const QStringList clipped = clippedLabels(dlg);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral("; "))));
    }

    // Help › About names the git GitBolt runs and where it is, from the
    // check at launch (git::probeGit()); as rich text, the path escaped.
    void showsTheGitItRuns() {
#ifdef Q_OS_WIN
        QSKIP("the stand-in git is a shell script, in a folder Windows can't name");
#else
        QTemporaryDir bin;
        QVERIFY(bin.isValid());
        const QString odd = bin.filePath(QStringLiteral("<i>odd</i>"));
        QVERIFY(QDir().mkpath(odd));
        const QString realGit = QString::fromStdString(gitbolt::git::GitProcess::findGitExecutable());
        QFile standIn(odd + QStringLiteral("/git"));
        QVERIFY(standIn.open(QIODevice::WriteOnly));
        standIn.write("#!/bin/sh\nexec '" + QFile::encodeName(realGit) + "' \"$@\"\n");
        standIn.close();
        QVERIFY(standIn.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const gitbolt::test::ScopedEnv path("PATH", QFile::encodeName(odd) + ':' + qgetenv("PATH"));
        const gitbolt::git::GitProbe probe = gitbolt::git::probeGit();
        QCOMPARE(probe.path, QDir::cleanPath(standIn.fileName()));
        QVERIFY(probe.version);

        AboutDialog dlg;
        const QLabel* info = nullptr;
        for (const QLabel* label : dlg.findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("<b>git:</b>")))
                info = label;
        }
        QVERIFY(info);
        QVERIFY2(info->text().contains(probe.version->text.toHtmlEscaped() + QStringLiteral(" at ")),
                 qPrintable(info->text()));
        QVERIFY2(info->text().contains(QStringLiteral("&lt;i&gt;odd&lt;/i&gt;/git")), qPrintable(info->text()));
        QVERIFY(!info->text().contains(QStringLiteral("<i>odd")));
        // A long path loses its middle rather than widen the dialog.
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        const QStringList clipped = clippedLabels(dlg);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral("; "))));
#endif
    }

    // Qt's copyright notice and LibXDiff's, each with its licence, and
    // libgit2's licence, beside GitBolt's own.
    void namesTheSoftwareItShips() {
        AboutDialog dlg;
        const QLabel* label = noticeLabel(dlg);
        QVERIFY(label);
        const QString text = label->text();
        const QStringList parts = {
            QStringLiteral("Qt %1 (").arg(QString::fromLatin1(qVersion())),
            QStringLiteral("© The Qt Company Ltd. and other contributors"),
            QStringLiteral("GNU LGPL v3"),
            QStringLiteral("libgit2 under the GNU GPL v2 with the linking exception"),
            QStringLiteral("LibXDiff (© 2003-2016 Davide Libenzi, Johannes E. Schindelin) under the GNU LGPL v2.1"),
            QStringLiteral(">their licences</a>"),
            QStringLiteral("<a href='#about-qt'>About Qt</a>"),
        };
        for (const QString& part : parts)
            QVERIFY2(text.contains(part), qPrintable(QStringLiteral("no \"%1\" in: %2").arg(part, text)));
        // Beside GitBolt's own notice, in the same label.
        QVERIFY(text.contains(QStringLiteral("Copyright © 2026 GitBolt.")));
    }

    // Where each package puts the licence texts, relative to the folder
    // of its binary; a build tree on Linux or Windows has none there.
    void findsTheNoticesOfEachPackage_data() {
        QTest::addColumn<QString>("appDir");
        QTest::addColumn<QString>("notices");
        QTest::addColumn<bool>("found");
        QTest::newRow("macOS bundle") << QStringLiteral("GitBolt.app/Contents/MacOS")
            << QStringLiteral("GitBolt.app/Contents/Resources/THIRD-PARTY-NOTICES.txt") << true;
        QTest::newRow("Windows install") << QStringLiteral("GitBolt/bin")
            << QStringLiteral("GitBolt/THIRD-PARTY-NOTICES.txt") << true;
        QTest::newRow(".deb") << QStringLiteral("usr/lib/gitbolt")
            << QStringLiteral("usr/share/doc/gitbolt/copyright") << true;
        QTest::newRow("AppImage") << QStringLiteral("squashfs-root/usr/bin")
            << QStringLiteral("squashfs-root/usr/share/doc/gitbolt/copyright") << true;
        QTest::newRow("build tree") << QStringLiteral("build/src/app")
            << QStringLiteral("build/THIRD-PARTY-NOTICES.txt") << false;
    }

    void findsTheNoticesOfEachPackage() {
        QFETCH(QString, appDir);
        QFETCH(QString, notices);
        QFETCH(bool, found);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString bin = root.filePath(appDir);
        const QString file = root.filePath(notices);
        QVERIFY(QDir().mkpath(bin));
        QCOMPARE(AboutDialog::thirdPartyNoticesPath(bin), QString());
        // A folder of that name is not the file.
        QVERIFY(QDir().mkpath(file));
        QCOMPARE(AboutDialog::thirdPartyNoticesPath(bin), QString());
        QVERIFY(QDir().rmdir(file));
        QFile text(file);
        QVERIFY(text.open(QIODevice::WriteOnly));
        text.close();
        QCOMPARE(AboutDialog::thirdPartyNoticesPath(bin),
                 found ? QDir::cleanPath(file) : QString());
    }

    // "their licences" opens the notices file installed with GitBolt in
    // the desktop's viewer, or their folder on GitHub when there is none,
    // and the web links still go to the browser. The label mustn't open
    // links itself (setOpenExternalLinks): then it hands none to the
    // dialog.
    void opensTheLicences() {
        UrlCatcher catcher;
        AboutDialog dlg;
        QLabel* label = noticeLabel(dlg);
        QVERIFY(label);
        QVERIFY(!label->openExternalLinks());
        // The test's binary is mostly in <build>/tests, so this is the
        // build's own THIRD-PARTY-NOTICES.txt, where a Windows install
        // has it; the label links the same either way.
        const QString notices =
            AboutDialog::thirdPartyNoticesPath(QCoreApplication::applicationDirPath());
        const QString href = notices.isEmpty()
            ? QStringLiteral("https://github.com/Inverian/GitBolt/tree/main/resources/licenses")
            : QStringLiteral("#third-party-notices");
        QVERIFY2(label->text().contains(QStringLiteral("<a href='%1'>their licences</a>").arg(href)),
                 qPrintable(label->text()));
        emit label->linkActivated(href);
        QCOMPARE(catcher.urls,
                 QList<QUrl>{notices.isEmpty() ? QUrl(href) : QUrl::fromLocalFile(notices)});

        catcher.urls.clear();
        const QString license = QStringLiteral("https://github.com/Inverian/GitBolt/blob/main/LICENSE");
        QVERIFY(label->text().contains(QStringLiteral("<a href='%1'>").arg(license)));
        emit label->linkActivated(license);
        QCOMPARE(catcher.urls, QList<QUrl>{QUrl(license)});
    }

    // The releases page is linked in every build, as the place to get
    // GitBolt where Check for Updates is greyed out.
    void linksTheReleasesPage() {
        AboutDialog dlg;
        const QString link = QStringLiteral("<a href='https://github.com/Inverian/GitBolt/releases'>Releases</a>");
        QStringList found;
        for (const QLabel* label : dlg.findChildren<QLabel*>()) {
            if (label->text().contains(link))
                found << label->text();
        }
        QCOMPARE(found.size(), 1);
    }

    // Tab reaches both link labels and Enter opens the link it is on, so
    // the releases page and the licences work without a mouse.
    void linksOpenFromTheKeyboard() {
        UrlCatcher catcher;
        AboutDialog dlg;
        dlg.show();
        dlg.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&dlg));
        QLabel* notices = noticeLabel(dlg);
        QLabel* links = linksLabel(dlg);
        QVERIFY(notices && links);

        // Tab through the dialog; on each link of the row, Enter opens it.
        const QUrl releases(QStringLiteral("https://github.com/Inverian/GitBolt/releases"));
        QList<QWidget*> reached;
        for (int i = 0; i < 60 && !catcher.urls.contains(releases); ++i) {
            QWidget* before = QApplication::focusWidget();
            QTest::keyClick(before ? before : static_cast<QWidget*>(&dlg), Qt::Key_Tab);
            reached << QApplication::focusWidget();
            if (before == links && reached.last() == links)   // on a link, not just arrived
                QTest::keyClick(links, Qt::Key_Return);
        }
        QVERIFY(reached.contains(notices));
        QCOMPARE(catcher.urls,
                 (QList<QUrl>{QUrl(QStringLiteral("https://www.gitbolt.com")), releases}));
        QVERIFY(dlg.isVisible());   // Enter opened the link, not Close
    }

    // About Qt shows Qt's own box, with its version and licences.
    void aboutQtLinkShowsQtsAbout() {
        UrlCatcher catcher;
        AboutDialog dlg;
        QLabel* label = noticeLabel(dlg);
        QVERIFY(label);
        QString shown;
        // On macOS the box is shown and left; elsewhere it runs its own
        // event loop, which this closes it from.
        QTimer closer;
        closer.setInterval(20);
        connect(&closer, &QTimer::timeout, &closer, [&shown]() {
            for (QWidget* window : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(window);
                if (box && box->isVisible()) {
                    shown = box->text() + box->informativeText();
                    box->done(QMessageBox::Ok);
                }
            }
        });
        closer.start();
        emit label->linkActivated(QStringLiteral("#about-qt"));
        QTRY_VERIFY(!shown.isEmpty());
        QVERIFY2(shown.contains(QStringLiteral("Qt version %1").arg(QString::fromLatin1(qVersion()))),
                 qPrintable(shown));
        QVERIFY(catcher.urls.isEmpty());
    }
};

QTEST_MAIN(TestAboutDialog)
#include "TestAboutDialog.moc"
