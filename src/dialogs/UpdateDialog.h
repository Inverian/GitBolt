#pragma once

#include "update/Package.h"
#include "update/Release.h"
#include "update/Version.h"

#include <QDialog>
#include <QString>
#include <QUrl>

class QBoxLayout;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace gitbolt::dialogs {

/// Help › Check for Updates, and the notice bar's View: what the
/// update check found, and the install it starts. Non-modal and one at
/// a time (ui::UpdateController owns it and moves it from state to
/// state); a pure view, which says what the user clicked and never
/// talks to the network itself.
///
/// Every text from GitHub is untrusted (update/Release.h): the release
/// notes are shown in a read-only QPlainTextEdit as written, markdown
/// and HTML included, and every label is plain text. The headline names
/// the version the updater parsed, not the release's title. The only
/// links followed are the release's page (View on GitHub, Open Release
/// Page), which parseRelease checked is under GitBolt's releases, the
/// releases page itself, or (an earlier install that failed) the page
/// of a version's tag there.
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    enum class State {
        Checking,        ///< asking GitHub; Close
        UpToDate,        ///< "GitBolt <v> is the latest version."; Close
        Available,       ///< notes; Install and Restart, Skip This Version, Later, View on GitHub
        NotInstallable,  ///< notes and why this copy can't install it; Open Release Page, Skip, Later
        Downloading,     ///< progress with sizes; Cancel
        Preparing,       ///< Installer::status() text; Cancel
        Failed,          ///< the message (and an earlier install's log); Try Again, Open Release Page, Close
        Restarting,      ///< the helper runs and GitBolt quits; no buttons: nothing is left to cancel
        QuitNeeded,      ///< GitBolt didn't quit: quit to finish; Quit GitBolt, Close
    };

    /// The releases page, for Open Release Page before a release is known.
    static QUrl releasesPageUrl();
    /// "checking", "up-to-date", "available", "not-installable",
    /// "downloading", "preparing", "failed", "restarting",
    /// "quit-needed": for the test bridge.
    static QString stateName(State state);

    explicit UpdateDialog(QWidget* parent = nullptr);

    State state() const { return state_; }

    void showChecking();
    /// `latest` is what GitHub has; `current` the running version,
    /// which may be newer (a development build).
    void showUpToDate(const update::Version& latest, const update::Version& current);
    /// The release with its notes. An empty `whyNot`: Available, with
    /// Install and Restart, and a word on the prompt this `kind` of
    /// install brings up once GitBolt has quit (UAC on Windows, polkit's
    /// password for a deb), so that it doesn't come out of nowhere.
    /// Otherwise NotInstallable: `whyNot` says why this copy can't
    /// install it and what to do, and Open Release Page takes Install's
    /// place.
    void showAvailable(const update::Release& release, const update::Version& current,
                       const QString& whyNot = {},
                       update::PackageKind kind = update::PackageKind::Unknown);
    /// The installer of the release last shown, `received` bytes of
    /// `total` so far. Call again as bytes arrive.
    void showDownloading(qint64 received, qint64 total);
    /// Getting the download ready (Installer::status()), then
    /// restarting.
    void showPreparing(const QString& status);
    /// The check failed: `message` says why (Checker::failed()).
    void showCheckFailed(const QString& message);
    /// Downloading or installing failed: `message` says why.
    void showInstallFailed(const QString& message);
    /// The update to `version` that an earlier GitBolt left to its
    /// helper didn't happen (UpdateController reads the helper's
    /// install-result at startup): `message` says why and what to do
    /// instead, and `log`, the end of the helper's install.log, is
    /// shown as written where the notes go (hidden when empty). Open
    /// Release Page goes to that version's release.
    void showEarlierInstallFailed(const QString& version, const QString& message, const QString& log);
    /// The helper is running and GitBolt quits: nothing can be
    /// cancelled any more, so no button is offered.
    void showRestarting();
    /// GitBolt was asked to quit and didn't (a dialog refused to close,
    /// work still running): the update goes in once it does.
    void showQuitNeeded();

    /// What the dialog says, every visible line of it but the notes:
    /// for tests and the test bridge.
    QString shownText() const;

signals:
    void installClicked();
    /// Skip This Version was clicked; the dialog closes itself.
    void skipClicked();
    /// Cancel, while downloading or preparing.
    void cancelClicked();
    /// Try Again, after a failure.
    void retryClicked();
    /// Quit GitBolt, when it didn't quit by itself after the helper
    /// started.
    void quitClicked();

private:
    void enterState(State state);
    void setProgressText(const QString& text);

    State state_ = State::Checking;
    QUrl pageUrl_;
    QString version_;   ///< of the release last shown
    /// The Failed state shows an earlier install's log in the notes.
    bool showsLog_ = false;

    QBoxLayout* top_ = nullptr;      ///< icon | column
    QBoxLayout* column_ = nullptr;   ///< the texts, the bar, the notes
    QLabel* icon_ = nullptr;
    QLabel* headline_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* reason_ = nullptr;
    QLabel* notesLabel_ = nullptr;
    QPlainTextEdit* notes_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* progressText_ = nullptr;

    QPushButton* viewOnGitHub_ = nullptr;
    QPushButton* openReleasePage_ = nullptr;
    QPushButton* skip_ = nullptr;
    QPushButton* later_ = nullptr;
    QPushButton* install_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* retry_ = nullptr;
    QPushButton* close_ = nullptr;
    QPushButton* quit_ = nullptr;
};

} // namespace gitbolt::dialogs
