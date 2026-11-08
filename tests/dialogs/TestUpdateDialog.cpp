//
// TestUpdateDialog — Help › Check for Updates' dialog in each of its
// states, off screen (dialogs/UpdateDialog.h):
//
//   - each state shows its own text and buttons, and Enter does its
//     main action (Install and Restart, Open Release Page, Try Again,
//     Close, Cancel);
//   - GitHub's text is shown as written: the notes in a plain text
//     edit, markup and all; the reason and every label as plain text;
//     the headline names the version, never the release's title;
//   - the only links are the release's page and the releases page,
//     and they go to the desktop (caught here, never opened);
//   - the buttons say what was clicked; Skip and Later close it, and it
//     is non-modal;
//   - sizes read as the platforms count them, and the bar can't
//     overflow;
//   - it fits what it says: without the notes it is shorter, and a
//     long message leaves no gap under it;
//   - Available warns of the system's prompt where one comes (Windows'
//     UAC, a deb's administrator password), and only there;
//   - once the helper runs nothing can be cancelled: Restarting has no
//     button, and QuitNeeded offers Quit GitBolt and Close;
//   - an earlier GitBolt's failed install shows its log where the notes
//     go, as written, and links that version's release.
//

#include <QApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QLayout>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include "dialogs/UpdateDialog.h"
#include "update/Package.h"
#include "update/Release.h"
#include "update/Version.h"

#include <algorithm>

using gitbolt::dialogs::UpdateDialog;
using gitbolt::update::PackageKind;
using gitbolt::update::Release;
using gitbolt::update::Version;
using State = UpdateDialog::State;

namespace {

// Catches the URLs the dialog hands the desktop: nothing opens a browser.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    UrlCatcher() {
        QDesktopServices::setUrlHandler(QStringLiteral("http"), this, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "open");
    }
    ~UrlCatcher() override {
        QDesktopServices::unsetUrlHandler(QStringLiteral("http"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    }
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls << url; }
};

Version v(int major, int minor, int patch) {
    return Version{major, minor, patch};
}

Release release(const QString& notes = QStringLiteral("## [0.1.1]\n\n- **Fixed**: things.")) {
    Release r;
    r.version = v(0, 1, 1);
    r.tag = QStringLiteral("v0.1.1");
    r.pageUrl = QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.1"));
    // GitHub's title: never shown.
    r.name = QStringLiteral("<b>TITLE</b> 9.9.9");
    r.notes = notes;
    return r;
}

template <typename T>
T* child(const QWidget& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}

// The buttons a state shows, by objectName.
QStringList shownButtons(const UpdateDialog& dialog) {
    QStringList names;
    for (const QPushButton* b : dialog.findChildren<QPushButton*>()) {
        if (!b->isHidden())
            names << b->objectName();
    }
    names.sort();
    return names;
}

QPushButton* defaultButton(const UpdateDialog& dialog) {
    for (QPushButton* b : dialog.findChildren<QPushButton*>()) {
        if (!b->isHidden() && b->isDefault())
            return b;
    }
    return nullptr;
}

QString headline(const UpdateDialog& dialog) {
    return child<QLabel>(dialog, "updateDialog.headline")->text();
}

} // namespace

class TestUpdateDialog : public QObject {
    Q_OBJECT

private slots:
    void startsCheckingAndIsNonModal() {
        UpdateDialog dialog;
        QCOMPARE(dialog.objectName(), QStringLiteral("updateDialog"));
        QVERIFY(!dialog.isModal());
        QCOMPARE(dialog.state(), State::Checking);
        QCOMPARE(dialog.property("updateState").toString(), QStringLiteral("checking"));
        QCOMPARE(headline(dialog), QStringLiteral("Checking for updates…"));
        QCOMPARE(shownButtons(dialog), QStringList{QStringLiteral("updateDialog.close")});
        auto* progress = child<QProgressBar>(dialog, "updateDialog.progress");
        QVERIFY(!progress->isHidden());
        QCOMPARE(progress->maximum(), 0);   // busy: no end known
        QVERIFY(child<QPlainTextEdit>(dialog, "updateDialog.notes")->isHidden());
    }

    void stateNames() {
        QCOMPARE(UpdateDialog::stateName(State::Checking), QStringLiteral("checking"));
        QCOMPARE(UpdateDialog::stateName(State::UpToDate), QStringLiteral("up-to-date"));
        QCOMPARE(UpdateDialog::stateName(State::Available), QStringLiteral("available"));
        QCOMPARE(UpdateDialog::stateName(State::NotInstallable), QStringLiteral("not-installable"));
        QCOMPARE(UpdateDialog::stateName(State::Downloading), QStringLiteral("downloading"));
        QCOMPARE(UpdateDialog::stateName(State::Preparing), QStringLiteral("preparing"));
        QCOMPARE(UpdateDialog::stateName(State::Failed), QStringLiteral("failed"));
        QCOMPARE(UpdateDialog::stateName(State::Restarting), QStringLiteral("restarting"));
        QCOMPARE(UpdateDialog::stateName(State::QuitNeeded), QStringLiteral("quit-needed"));
    }

    // The prompt comes from the system once GitBolt has quit: Available
    // says so beforehand, for the kinds that bring one up.
    void availableWarnsOfTheSystemsPrompt_data() {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("warning");
        QTest::newRow("Windows") << int(PackageKind::WindowsInstall)
                                 << "Windows will ask for permission to install it.";
        QTest::newRow("deb") << int(PackageKind::Deb) << "You'll be asked for an administrator's password.";
        QTest::newRow("macOS") << int(PackageKind::MacBundle) << "";
        QTest::newRow("AppImage") << int(PackageKind::AppImage) << "";
        QTest::newRow("unknown") << int(PackageKind::Unknown) << "";
    }
    void availableWarnsOfTheSystemsPrompt() {
        QFETCH(int, kind);
        QFETCH(QString, warning);
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0), {}, PackageKind(kind));
        const QString message = child<QLabel>(dialog, "updateDialog.message")->text();
        const QString base = QStringLiteral("You have 0.1.0. Install and Restart downloads the new version, "
                                            "checks it, and restarts GitBolt with it.");
        QCOMPARE(message, warning.isEmpty() ? base : base + QLatin1Char(' ') + warning);
        // Nothing to warn of when it can't be installed here.
        dialog.showAvailable(release(), v(0, 1, 0), QStringLiteral("Not here."), PackageKind(kind));
        QCOMPARE(child<QLabel>(dialog, "updateDialog.message")->text(), QStringLiteral("You have 0.1.0."));
    }

    // The helper runs, GitBolt quits: no button, nothing to cancel.
    void restartingOffersNothingToCancel() {
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        dialog.showPreparing(QStringLiteral("Copying GitBolt…"));
        dialog.showRestarting();
        QCOMPARE(dialog.state(), State::Restarting);
        QCOMPARE(dialog.property("updateState").toString(), QStringLiteral("restarting"));
        QCOMPARE(headline(dialog), QStringLiteral("Restarting GitBolt to finish the update…"));
        QVERIFY(dialog.shownText().contains(QStringLiteral("GitBolt 0.1.1 is put in place")));
        QCOMPARE(shownButtons(dialog), QStringList{});
        QVERIFY(!defaultButton(dialog));
        auto* progress = child<QProgressBar>(dialog, "updateDialog.progress");
        QVERIFY(!progress->isHidden());
        QCOMPARE(progress->maximum(), 0);
        QVERIFY(child<QPlainTextEdit>(dialog, "updateDialog.notes")->isHidden());
    }

    // GitBolt didn't quit: quit it, or close this and quit later.
    void quitNeededOffersQuit() {
        UpdateDialog dialog;
        QSignalSpy quit(&dialog, &UpdateDialog::quitClicked);
        dialog.showAvailable(release(), v(0, 1, 0));
        dialog.showRestarting();
        dialog.showQuitNeeded();
        QCOMPARE(dialog.state(), State::QuitNeeded);
        QCOMPARE(headline(dialog), QStringLiteral("Quit GitBolt to finish installing 0.1.1."));
        QVERIFY(dialog.shownText().contains(QStringLiteral("within 10 minutes")));
        QCOMPARE(shownButtons(dialog),
                 (QStringList{QStringLiteral("updateDialog.close"), QStringLiteral("updateDialog.quit")}));
        QCOMPARE(defaultButton(dialog)->objectName(), QStringLiteral("updateDialog.quit"));
        QCOMPARE(child<QPushButton>(dialog, "updateDialog.quit")->text(), QStringLiteral("Quit GitBolt"));
        QVERIFY(child<QProgressBar>(dialog, "updateDialog.progress")->isHidden());
        child<QPushButton>(dialog, "updateDialog.quit")->click();
        QCOMPARE(quit.count(), 1);
    }

    // A state without the notes is as tall as its text at the width it
    // has: a long message (QuitNeeded's wraps over lines) leaves no gap
    // under it. adjustSize alone sized it for its labels' narrower hint.
    void aLongMessageLeavesNoGap() {
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.showRestarting();
        dialog.showQuitNeeded();
        const int fits = std::max(dialog.minimumSizeHint().height(),
                                  dialog.layout()->totalHeightForWidth(dialog.width()));
        QVERIFY2(dialog.height() <= fits, qPrintable(QStringLiteral("%1 > %2").arg(dialog.height()).arg(fits)));
        dialog.showCheckFailed(QStringLiteral("GitHub is limiting requests from your network; try again later. "
                                              "Again and again, at length, so that this wraps over lines."));
        QVERIFY(dialog.height() <= std::max(dialog.minimumSizeHint().height(),
                                            dialog.layout()->totalHeightForWidth(dialog.width())));
    }

    // An earlier GitBolt's install that didn't go in: its log where the
    // notes go, as written; that version's release page.
    void earlierInstallFailedShowsTheLog() {
        UrlCatcher catcher;
        UpdateDialog dialog;
        const QString log = QStringLiteral("10:00:01 <b>waiting</b>\n10:00:02 apt-get failed with status 100");
        dialog.showEarlierInstallFailed(QStringLiteral("0.1.2"), QStringLiteral("It failed.\n\nDo this instead."), log);
        QCOMPARE(dialog.state(), State::Failed);
        QCOMPARE(headline(dialog), QStringLiteral("Couldn't install GitBolt 0.1.2"));
        QCOMPARE(child<QLabel>(dialog, "updateDialog.message")->text(), QStringLiteral("It failed.\n\nDo this instead."));
        auto* notes = child<QPlainTextEdit>(dialog, "updateDialog.notes");
        auto* notesLabel = child<QLabel>(dialog, "updateDialog.notesLabel");
        QVERIFY(!notes->isHidden());
        QCOMPARE(notes->toPlainText(), log);
        QCOMPARE(notesLabel->text(), QStringLiteral("The end of the installer's log:"));
        QCOMPARE(shownButtons(dialog),
                 (QStringList{QStringLiteral("updateDialog.close"), QStringLiteral("updateDialog.openReleasePage"),
                              QStringLiteral("updateDialog.retry")}));
        QCOMPARE(defaultButton(dialog)->objectName(), QStringLiteral("updateDialog.retry"));
        child<QPushButton>(dialog, "updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls,
                 QList<QUrl>{QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases/tag/v0.1.2"))});

        // Without a log, no box for it.
        dialog.showEarlierInstallFailed(QStringLiteral("0.1.2"), QStringLiteral("It failed."), {});
        QVERIFY(notes->isHidden());
        // The next states are the usual ones: no log, then the notes.
        dialog.showEarlierInstallFailed(QStringLiteral("0.1.2"), QStringLiteral("It failed."), log);
        dialog.showChecking();
        QVERIFY(notes->isHidden());
        dialog.showCheckFailed(QStringLiteral("Couldn't reach GitHub."));
        QVERIFY(notes->isHidden());
        dialog.showAvailable(release(), v(0, 1, 0));
        QCOMPARE(notesLabel->text(), QStringLiteral("What's new:"));
        QCOMPARE(notes->toPlainText(), release().notes);
        dialog.showInstallFailed(QStringLiteral("The download didn't match its checksum."));
        QVERIFY(notes->isHidden());
    }

    void upToDate() {
        UpdateDialog dialog;
        dialog.showUpToDate(v(0, 1, 1), v(0, 1, 1));
        QCOMPARE(dialog.state(), State::UpToDate);
        QCOMPARE(child<QLabel>(dialog, "updateDialog.message")->text(),
                 QStringLiteral("GitBolt 0.1.1 is the latest version."));
        QCOMPARE(shownButtons(dialog), QStringList{QStringLiteral("updateDialog.close")});
        QCOMPARE(defaultButton(dialog)->objectName(), QStringLiteral("updateDialog.close"));
        QVERIFY(child<QProgressBar>(dialog, "updateDialog.progress")->isHidden());

        // A development build newer than the release says so; 0.1.10 is
        // newer than 0.1.9 by number.
        dialog.showUpToDate(v(0, 1, 9), v(0, 1, 10));
        QCOMPARE(child<QLabel>(dialog, "updateDialog.message")->text(),
                 QStringLiteral("GitBolt 0.1.9 is the latest version. This copy, 0.1.10, is newer."));
    }

    void availableOffersInstall() {
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        QCOMPARE(dialog.state(), State::Available);
        QCOMPARE(dialog.property("updateState").toString(), QStringLiteral("available"));
        QCOMPARE(headline(dialog), QStringLiteral("GitBolt 0.1.1 is available"));
        QVERIFY(child<QLabel>(dialog, "updateDialog.message")->text().startsWith(QStringLiteral("You have 0.1.0.")));
        QCOMPARE(shownButtons(dialog),
                 (QStringList{QStringLiteral("updateDialog.install"), QStringLiteral("updateDialog.later"),
                              QStringLiteral("updateDialog.skip"), QStringLiteral("updateDialog.viewOnGitHub")}));
        QCOMPARE(defaultButton(dialog)->objectName(), QStringLiteral("updateDialog.install"));
        QCOMPARE(child<QPushButton>(dialog, "updateDialog.install")->text(), QStringLiteral("Install and Restart"));
        QCOMPARE(child<QPushButton>(dialog, "updateDialog.skip")->text(), QStringLiteral("Skip This Version"));
        QVERIFY(child<QLabel>(dialog, "updateDialog.reason")->isHidden());
        QVERIFY(!child<QPlainTextEdit>(dialog, "updateDialog.notes")->isHidden());
    }

    // Markdown and HTML from GitHub: shown as written, never rendered,
    // and never a link. The title GitHub gave isn't shown at all.
    void githubTextIsShownAsWritten() {
        const QString notes = QStringLiteral("<b>bold?</b> <a href=\"https://evil.invalid/\">link</a>\n"
                                             "**markdown** [x](https://evil.invalid/)");
        UpdateDialog dialog;
        dialog.showAvailable(release(notes), v(0, 1, 0));
        auto* edit = child<QPlainTextEdit>(dialog, "updateDialog.notes");
        QVERIFY(edit->isReadOnly());
        QCOMPARE(edit->toPlainText(), notes);
        for (const QLabel* label : dialog.findChildren<QLabel*>())
            QCOMPARE(label->textFormat(), Qt::PlainText);
        QVERIFY(!dialog.shownText().contains(QStringLiteral("TITLE")));

        // A reason with markup (whyNot is GitBolt's text, but never
        // trusted to be markup-free): shown as written.
        const QString why = QStringLiteral("<b>no</b> & <i>never</i>");
        dialog.showAvailable(release(), v(0, 1, 0), why);
        auto* reason = child<QLabel>(dialog, "updateDialog.reason");
        QCOMPARE(reason->textFormat(), Qt::PlainText);
        QCOMPARE(reason->text(), why);
    }

    void emptyNotesSaySo() {
        UpdateDialog dialog;
        dialog.showAvailable(release(QStringLiteral("  \n")), v(0, 1, 0));
        auto* edit = child<QPlainTextEdit>(dialog, "updateDialog.notes");
        QVERIFY(edit->toPlainText().isEmpty());
        QCOMPARE(edit->placeholderText(), QStringLiteral("This release has no notes."));
    }

    void notInstallableSaysWhyAndLinksTheRelease() {
        UrlCatcher catcher;
        UpdateDialog dialog;
        const QString why = QStringLiteral("This copy of GitBolt wasn't installed from a GitBolt release package.");
        dialog.showAvailable(release(), v(0, 1, 0), why);
        QCOMPARE(dialog.state(), State::NotInstallable);
        QCOMPARE(shownButtons(dialog),
                 (QStringList{QStringLiteral("updateDialog.later"), QStringLiteral("updateDialog.openReleasePage"),
                              QStringLiteral("updateDialog.skip")}));
        QCOMPARE(defaultButton(dialog)->objectName(), QStringLiteral("updateDialog.openReleasePage"));
        QVERIFY(!child<QLabel>(dialog, "updateDialog.reason")->isHidden());
        QVERIFY(dialog.shownText().contains(why));
        // The notes are still there to read.
        QVERIFY(!child<QPlainTextEdit>(dialog, "updateDialog.notes")->isHidden());

        child<QPushButton>(dialog, "updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls, QList<QUrl>{release().pageUrl});
    }

    void viewOnGitHubOpensTheReleasePage() {
        UrlCatcher catcher;
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        child<QPushButton>(dialog, "updateDialog.viewOnGitHub")->click();
        QCOMPARE(catcher.urls, QList<QUrl>{release().pageUrl});
        QVERIFY(!child<QPushButton>(dialog, "updateDialog.viewOnGitHub")->autoDefault());
    }

    void downloadingShowsSizes() {
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        QSignalSpy cancel(&dialog, &UpdateDialog::cancelClicked);
        dialog.showDownloading(0, 2'000'000);
        QCOMPARE(dialog.state(), State::Downloading);
        QCOMPARE(headline(dialog), QStringLiteral("Downloading GitBolt 0.1.1…"));
        QCOMPARE(shownButtons(dialog), QStringList{QStringLiteral("updateDialog.cancel")});
        auto* progress = child<QProgressBar>(dialog, "updateDialog.progress");
        auto* text = child<QLabel>(dialog, "updateDialog.progressText");
        QCOMPARE(progress->value(), 0);

        dialog.showDownloading(1'000'000, 2'000'000);
        QCOMPARE(progress->maximum(), 1000);
        QCOMPARE(progress->value(), 500);
        const QLocale locale;
        QCOMPARE(text->text(), QStringLiteral("%1 of %2")
                                   .arg(locale.formattedDataSize(1'000'000, 1, QLocale::DataSizeSIFormat),
                                        locale.formattedDataSize(2'000'000, 1, QLocale::DataSizeSIFormat)));

        // Past 2 GiB, and past the end, the bar stays in range.
        dialog.showDownloading(3'000'000'000LL, 4'000'000'000LL);
        QCOMPARE(progress->value(), 750);
        dialog.showDownloading(5'000, 4'000);
        QCOMPARE(progress->value(), 1000);

        child<QPushButton>(dialog, "updateDialog.cancel")->click();
        QCOMPARE(cancel.count(), 1);
        QVERIFY(!child<QPlainTextEdit>(dialog, "updateDialog.notes")->isHidden());
    }

    void preparingShowsTheInstallersWord() {
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        dialog.showPreparing(QStringLiteral("Copying GitBolt…"));
        QCOMPARE(dialog.state(), State::Preparing);
        QCOMPARE(headline(dialog), QStringLiteral("Installing GitBolt 0.1.1…"));
        QCOMPARE(child<QLabel>(dialog, "updateDialog.progressText")->text(), QStringLiteral("Copying GitBolt…"));
        QCOMPARE(shownButtons(dialog), QStringList{QStringLiteral("updateDialog.cancel")});
        QCOMPARE(child<QProgressBar>(dialog, "updateDialog.progress")->maximum(), 0);
    }

    void checkFailedOffersRetryAndTheReleasesPage() {
        UrlCatcher catcher;
        UpdateDialog dialog;
        QSignalSpy retry(&dialog, &UpdateDialog::retryClicked);
        const QString why = QStringLiteral("GitHub is limiting requests from your network; try again later.");
        dialog.showCheckFailed(why);
        QCOMPARE(dialog.state(), State::Failed);
        QCOMPARE(headline(dialog), QStringLiteral("Couldn't check for updates"));
        QCOMPARE(child<QLabel>(dialog, "updateDialog.message")->text(), why);
        QCOMPARE(shownButtons(dialog),
                 (QStringList{QStringLiteral("updateDialog.close"), QStringLiteral("updateDialog.openReleasePage"),
                              QStringLiteral("updateDialog.retry")}));
        QCOMPARE(defaultButton(dialog)->objectName(), QStringLiteral("updateDialog.retry"));
        child<QPushButton>(dialog, "updateDialog.retry")->click();
        QCOMPARE(retry.count(), 1);
        // No release known: the releases page.
        child<QPushButton>(dialog, "updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls, QList<QUrl>{UpdateDialog::releasesPageUrl()});
        QCOMPARE(UpdateDialog::releasesPageUrl(), QUrl(QStringLiteral("https://github.com/Inverian/GitBolt/releases")));
    }

    void installFailedKeepsTheReleasePage() {
        UrlCatcher catcher;
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        dialog.showInstallFailed(QStringLiteral("The download didn't match its checksum."));
        QCOMPARE(dialog.state(), State::Failed);
        QCOMPARE(headline(dialog), QStringLiteral("Couldn't install GitBolt 0.1.1"));
        QVERIFY(dialog.shownText().contains(QStringLiteral("The download didn't match its checksum.")));
        QVERIFY(dialog.shownText().contains(QStringLiteral("GitBolt wasn't changed.")));
        child<QPushButton>(dialog, "updateDialog.openReleasePage")->click();
        QCOMPARE(catcher.urls, QList<QUrl>{release().pageUrl});
    }

    void buttonsSayWhatWasClicked() {
        UpdateDialog dialog;
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QSignalSpy install(&dialog, &UpdateDialog::installClicked);
        QSignalSpy skip(&dialog, &UpdateDialog::skipClicked);
        QSignalSpy finished(&dialog, &QDialog::finished);

        dialog.showAvailable(release(), v(0, 1, 0));
        child<QPushButton>(dialog, "updateDialog.install")->click();
        QCOMPARE(install.count(), 1);
        QVERIFY(dialog.isVisible());   // the controller moves it on

        // Skip says so, then closes.
        child<QPushButton>(dialog, "updateDialog.skip")->click();
        QCOMPARE(skip.count(), 1);
        QVERIFY(!dialog.isVisible());
        QCOMPARE(finished.count(), 1);

        // Later only closes.
        dialog.show();
        child<QPushButton>(dialog, "updateDialog.later")->click();
        QVERIFY(!dialog.isVisible());
        QCOMPARE(finished.count(), 2);
        QCOMPARE(skip.count(), 1);

        // And so does Escape.
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QVERIFY(!dialog.isVisible());
        QCOMPARE(finished.count(), 3);
    }

    // The notes take the room: a state without them is shorter, and
    // the shown text leaves out what is hidden.
    void fitsWhatItSays() {
        UpdateDialog dialog;
        dialog.showAvailable(release(), v(0, 1, 0));
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        const int withNotes = dialog.height();
        dialog.showCheckFailed(QStringLiteral("Couldn't reach GitHub: Connection refused."));
        QVERIFY2(dialog.height() < withNotes,
                 qPrintable(QStringLiteral("%1 vs %2").arg(dialog.height()).arg(withNotes)));
        QVERIFY(!dialog.shownText().contains(QStringLiteral("You have")));
        QVERIFY(dialog.shownText().contains(QStringLiteral("Connection refused")));
        QVERIFY(dialog.width() >= dialog.minimumWidth());
    }
};

QTEST_MAIN(TestUpdateDialog)
#include "TestUpdateDialog.moc"
