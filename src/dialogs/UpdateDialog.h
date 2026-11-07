#pragma once

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
/// Page), which parseRelease checked is under GitBolt's releases, or
/// the releases page itself.
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
        Failed,          ///< the message; Try Again, Open Release Page, Close
    };

    /// The releases page, for Open Release Page before a release is known.
    static QUrl releasesPageUrl();
    /// "checking", "up-to-date", "available", "not-installable",
    /// "downloading", "preparing", "failed": for the test bridge.
    static QString stateName(State state);

    explicit UpdateDialog(QWidget* parent = nullptr);

    State state() const { return state_; }

    void showChecking();
    /// `latest` is what GitHub has; `current` the running version,
    /// which may be newer (a development build).
    void showUpToDate(const update::Version& latest, const update::Version& current);
    /// The release with its notes. An empty `whyNot`: Available, with
    /// Install and Restart. Otherwise NotInstallable: `whyNot` says why
    /// this copy can't install it and what to do, and Open Release Page
    /// takes Install's place.
    void showAvailable(const update::Release& release, const update::Version& current,
                       const QString& whyNot = {});
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

private:
    void enterState(State state);
    void setProgressText(const QString& text);

    State state_ = State::Checking;
    QUrl pageUrl_;
    QString version_;   ///< of the release last shown

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
};

} // namespace gitbolt::dialogs
