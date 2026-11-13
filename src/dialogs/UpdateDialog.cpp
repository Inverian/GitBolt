#include "dialogs/UpdateDialog.h"

#include "conf/ThemeService.h"
#include "update/UpdateConfig.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <algorithm>

namespace gitbolt::dialogs {

namespace {

// Plain text, wrapped, selectable: a headline or a sentence the user
// may want to copy (an error), never markup.
QLabel* plainLabel(QWidget* parent, const char* objectName)
{
    auto* label = new QLabel(parent);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QPushButton* button(QWidget* parent, const QString& text, const char* objectName)
{
    auto* b = new QPushButton(text, parent);
    b->setObjectName(QString::fromLatin1(objectName));
    return b;
}

QString bytesText(qint64 bytes)
{
    // "45.6 MB", as Finder and the release page count them.
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

} // namespace

QUrl UpdateDialog::releasesPageUrl()
{
    return QUrl(QStringLiteral("https://github.com/%1/releases").arg(QLatin1String(update::kOfficialRepo)));
}

QString UpdateDialog::stateName(State state)
{
    switch (state) {
    case State::Checking: return QStringLiteral("checking");
    case State::UpToDate: return QStringLiteral("up-to-date");
    case State::Available: return QStringLiteral("available");
    case State::NotInstallable: return QStringLiteral("not-installable");
    case State::Downloading: return QStringLiteral("downloading");
    case State::Preparing: return QStringLiteral("preparing");
    case State::Failed: return QStringLiteral("failed");
    case State::Restarting: return QStringLiteral("restarting");
    case State::QuitNeeded: return QStringLiteral("quit-needed");
    case State::Declined: return QStringLiteral("declined");
    }
    return {};
}

UpdateDialog::UpdateDialog(QWidget* parent)
    : QDialog(parent)
    , pageUrl_(releasesPageUrl())
{
    setObjectName(QStringLiteral("updateDialog"));
    setWindowTitle(tr("Software Update"));
    // Non-modal: GitBolt stays usable while it checks and downloads.
    setModal(false);
    // As the dialogs SettingsService::applyConfiguredSize sizes: Cmd+W
    // (Ctrl+W) closes it. Not that helper itself: its default size is
    // the big dialogs', and this one sizes itself to what it says.
    auto* closeShortcut = new QShortcut(QKeySequence::Close, this);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 16);
    root->setSpacing(14);

    auto* top = new QHBoxLayout;
    top_ = top;
    top->setSpacing(16);
    icon_ = new QLabel(this);
    icon_->setObjectName(QStringLiteral("updateDialog.icon"));
    icon_->setTextFormat(Qt::PlainText);
    icon_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    const QPixmap icon(QStringLiteral(":/icons/gitbolt-128.png"));
    if (!icon.isNull()) {
        QPixmap scaled = icon.scaled(QSize(64, 64) * icon_->devicePixelRatioF(), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(icon_->devicePixelRatioF());
        icon_->setPixmap(scaled);
    }
    icon_->setFixedWidth(72);
    icon_->setVisible(!icon.isNull());
    top->addWidget(icon_, 0, Qt::AlignTop);

    auto* column = new QVBoxLayout;
    column_ = column;
    column->setSpacing(8);
    headline_ = plainLabel(this, "updateDialog.headline");
    QFont headlineFont = headline_->font();
    headlineFont.setBold(true);
    if (headlineFont.pointSizeF() > 0)
        headlineFont.setPointSizeF(headlineFont.pointSizeF() * 1.25);
    headline_->setFont(headlineFont);
    column->addWidget(headline_);
    message_ = plainLabel(this, "updateDialog.message");
    column->addWidget(message_);
    // Why this copy can't install the release: a sentence or two of
    // its own, in the theme's text colour (it matters more than the
    // muted lines).
    reason_ = plainLabel(this, "updateDialog.reason");
    column->addWidget(reason_);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("updateDialog.progress"));
    progress_->setTextVisible(false);
    column->addWidget(progress_);
    progressText_ = plainLabel(this, "updateDialog.progressText");
    conf::ThemeService::keepMutedText(progressText_, QStringLiteral("QLabel { color: %1; }"));
    column->addWidget(progressText_);

    notesLabel_ = new QLabel(tr("What's new:"), this);
    notesLabel_->setObjectName(QStringLiteral("updateDialog.notesLabel"));
    notesLabel_->setTextFormat(Qt::PlainText);
    QFont notesFont = notesLabel_->font();
    notesFont.setBold(true);
    notesLabel_->setFont(notesFont);
    column->addSpacing(4);
    column->addWidget(notesLabel_);
    // The release's notes as GitHub has them: markdown source, shown
    // as written (Release::notes is untrusted).
    notes_ = new QPlainTextEdit(this);
    notes_->setObjectName(QStringLiteral("updateDialog.notes"));
    notes_->setReadOnly(true);
    notes_->setPlaceholderText(tr("This release has no notes."));
    notes_->setMinimumSize(420, 180);
    notes_->setTabChangesFocus(true);
    column->addWidget(notes_, 1);
    // Room the notes don't take (states without them) goes below the
    // text, which stays together at the top.
    column->addStretch(0);

    top->addLayout(column, 1);
    root->addLayout(top, 1);

    // Buttons. One row for every state, each showing its own: the
    // order puts the state's main action last (rightmost), as the
    // platforms' dialogs have it.
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    // A link, not a button: it leaves GitBolt for the browser.
    viewOnGitHub_ = button(this, tr("View on GitHub"), "updateDialog.viewOnGitHub");
    viewOnGitHub_->setFlat(true);
    viewOnGitHub_->setAutoDefault(false);
    viewOnGitHub_->setCursor(Qt::PointingHandCursor);
    conf::ThemeService::keepThemedStyleSheet(viewOnGitHub_, [](bool) {
        return QStringLiteral("QPushButton { color: %1; border: none; background: transparent; "
                              "padding: 4px 0; } QPushButton:hover { text-decoration: underline; }")
            .arg(QApplication::palette().color(QPalette::Link).name());
    });
    buttons->addWidget(viewOnGitHub_);
    buttons->addStretch(1);
    skip_ = button(this, tr("Skip This Version"), "updateDialog.skip");
    later_ = button(this, tr("Later"), "updateDialog.later");
    openReleasePage_ = button(this, tr("Open Release Page"), "updateDialog.openReleasePage");
    close_ = button(this, tr("Close"), "updateDialog.close");
    cancel_ = button(this, tr("Cancel"), "updateDialog.cancel");
    retry_ = button(this, tr("Try Again"), "updateDialog.retry");
    install_ = button(this, tr("Install and Restart"), "updateDialog.install");
    quit_ = button(this, tr("Quit GitBolt"), "updateDialog.quit");
    for (QPushButton* b : {skip_, later_, openReleasePage_, close_, cancel_, retry_, install_, quit_})
        buttons->addWidget(b);
    root->addLayout(buttons);

    connect(viewOnGitHub_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(pageUrl_); });
    connect(openReleasePage_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(pageUrl_); });
    connect(install_, &QPushButton::clicked, this, &UpdateDialog::installClicked);
    connect(skip_, &QPushButton::clicked, this, [this] {
        emit skipClicked();
        reject();
    });
    connect(later_, &QPushButton::clicked, this, &QDialog::reject);
    connect(close_, &QPushButton::clicked, this, &QDialog::reject);
    connect(cancel_, &QPushButton::clicked, this, &UpdateDialog::cancelClicked);
    connect(retry_, &QPushButton::clicked, this, &UpdateDialog::retryClicked);
    connect(quit_, &QPushButton::clicked, this, &UpdateDialog::quitClicked);

    setMinimumWidth(560);
    showChecking();
}

void UpdateDialog::enterState(State state)
{
    const bool notesBefore = !notes_->isHidden();
    state_ = state;
    // For the test bridge's dump-state, without it knowing this class.
    setProperty("updateState", stateName(state));

    // The release's notes, or (Failed after an earlier GitBolt's
    // install) the end of the helper's log.
    const bool withRelease = state == State::Available || state == State::NotInstallable
                             || state == State::Downloading || state == State::Preparing
                             || (state == State::Failed && showsLog_);
    notesLabel_->setVisible(withRelease);
    notes_->setVisible(withRelease);
    reason_->setVisible(state == State::NotInstallable);
    progress_->setVisible(state == State::Checking || state == State::Downloading
                          || state == State::Preparing || state == State::Restarting);
    progressText_->setVisible(state == State::Downloading || state == State::Preparing);
    message_->setVisible(true);

    viewOnGitHub_->setVisible(state == State::Available);
    skip_->setVisible(state == State::Available || state == State::NotInstallable);
    later_->setVisible(state == State::Available || state == State::NotInstallable);
    openReleasePage_->setVisible(state == State::NotInstallable || state == State::Failed
                                 || state == State::Declined);
    close_->setVisible(state == State::Checking || state == State::UpToDate || state == State::Failed
                       || state == State::QuitNeeded || state == State::Declined);
    cancel_->setVisible(state == State::Downloading || state == State::Preparing);
    retry_->setVisible(state == State::Failed || state == State::Declined);
    install_->setVisible(state == State::Available);
    quit_->setVisible(state == State::QuitNeeded);

    // Enter does the state's main action.
    QPushButton* primary = close_;
    switch (state) {
    case State::Checking:
    case State::UpToDate: primary = close_; break;
    case State::Available: primary = install_; break;
    case State::NotInstallable: primary = openReleasePage_; break;
    case State::Downloading:
    case State::Preparing: primary = cancel_; break;
    case State::Failed:
    case State::Declined: primary = retry_; break;
    case State::Restarting: primary = nullptr; break;   // nothing to do but wait
    case State::QuitNeeded: primary = quit_; break;
    }
    for (QPushButton* b : {skip_, later_, openReleasePage_, close_, cancel_, retry_, install_, quit_}) {
        b->setAutoDefault(b == primary);
        b->setDefault(b == primary);
    }

    // The notes take most of the room. A state without them fits what it
    // says, as does one that just gained them; one that keeps them keeps
    // the size the user gave it.
    if (!withRelease || notesBefore != withRelease || !isVisible()) {
        // Hiding a widget invalidates only the dialog's own layout; the
        // nested ones would size it with what they hid.
        column_->invalidate();
        top_->invalidate();
        adjustSize();
        // adjustSize takes the height for the width of the wrapped
        // labels' own hint, narrower than the dialog's minimum: a long
        // message would leave a gap under it at the width it gets.
        if (!withRelease && layout()->hasHeightForWidth())
            resize(width(), std::max(minimumSizeHint().height(), layout()->totalHeightForWidth(width())));
    }
}

void UpdateDialog::setProgressText(const QString& text)
{
    progressText_->setText(text);
}

void UpdateDialog::showChecking()
{
    showsLog_ = false;
    headline_->setText(tr("Checking for updates…"));
    message_->setText(tr("GitBolt is asking GitHub for its latest release."));
    progress_->setRange(0, 0);   // busy, no end known
    enterState(State::Checking);
}

void UpdateDialog::showUpToDate(const update::Version& latest, const update::Version& current)
{
    showsLog_ = false;
    headline_->setText(tr("GitBolt is up to date"));
    QString text = tr("GitBolt %1 is the latest version.").arg(latest.toString());
    if (current > latest)
        text += QLatin1Char(' ') + tr("This copy, %1, is newer.").arg(current.toString());
    message_->setText(text);
    enterState(State::UpToDate);
}

void UpdateDialog::showAvailable(const update::Release& release, const update::Version& current,
                                 const QString& whyNot, update::PackageKind kind)
{
    showsLog_ = false;
    version_ = release.version.toString();
    pageUrl_ = release.pageUrl.isValid() ? release.pageUrl : releasesPageUrl();
    headline_->setText(tr("GitBolt %1 is available").arg(version_));
    if (whyNot.isEmpty()) {
        QString text = tr("You have %1. Install and Restart downloads the new version, checks it, "
                          "and restarts GitBolt with it.")
                           .arg(current.toString());
        // The system's prompt: said here, it doesn't come out of nowhere.
        // Windows' comes once the download is checked, and names the
        // program that runs the installer, Windows PowerShell (from
        // Microsoft), not GitBolt; a deb's once GitBolt has quit.
        if (kind == update::PackageKind::WindowsInstall)
            text += QLatin1Char(' ') + tr("Windows will ask for permission for Windows PowerShell to install it.");
        else if (kind == update::PackageKind::Deb)
            text += QLatin1Char(' ') + tr("You'll be asked for an administrator's password.");
        message_->setText(text);
    } else {
        message_->setText(tr("You have %1.").arg(current.toString()));
    }
    reason_->setText(whyNot);
    notesLabel_->setText(tr("What's new:"));
    // setPlainText keeps every character as GitHub sent it.
    notes_->setPlainText(release.notes.trimmed());
    enterState(whyNot.isEmpty() ? State::Available : State::NotInstallable);
}

void UpdateDialog::showDownloading(qint64 received, qint64 total)
{
    headline_->setText(tr("Downloading GitBolt %1…").arg(version_));
    message_->setText(tr("GitBolt restarts once the download is checked and installed."));
    // Per mille, so a size over 2 GiB can't overflow the bar's int.
    progress_->setRange(0, 1000);
    progress_->setValue(total > 0 ? static_cast<int>(std::clamp<qint64>(received, 0, total) * 1000 / total) : 0);
    setProgressText(total > 0 ? tr("%1 of %2").arg(bytesText(received), bytesText(total)) : bytesText(received));
    if (state_ != State::Downloading)
        enterState(State::Downloading);
}

void UpdateDialog::showPreparing(const QString& status)
{
    headline_->setText(tr("Installing GitBolt %1…").arg(version_));
    message_->setText(tr("GitBolt restarts once the new version is in place."));
    progress_->setRange(0, 0);
    setProgressText(status);
    if (state_ != State::Preparing)
        enterState(State::Preparing);
}

void UpdateDialog::showCheckFailed(const QString& message)
{
    showsLog_ = false;
    headline_->setText(tr("Couldn't check for updates"));
    message_->setText(message);
    pageUrl_ = releasesPageUrl();
    enterState(State::Failed);
}

void UpdateDialog::showInstallFailed(const QString& message)
{
    showsLog_ = false;
    headline_->setText(tr("Couldn't install GitBolt %1").arg(version_));
    message_->setText(message + QLatin1Char('\n') + QLatin1Char('\n')
                      + tr("GitBolt wasn't changed. You can try again, or download the new version "
                           "from the release page."));
    enterState(State::Failed);
}

void UpdateDialog::showEarlierInstallFailed(const QString& version, const QString& message,
                                            const QString& log)
{
    version_ = version;
    // That version's own release page. Built here, from a version
    // Version::parse accepted: no text from GitHub goes into it.
    pageUrl_ = QUrl(releasesPageUrl().toString() + QStringLiteral("/tag/v") + version);
    headline_->setText(tr("Couldn't install GitBolt %1").arg(version_));
    message_->setText(message);
    showsLog_ = !log.isEmpty();
    notesLabel_->setText(tr("The end of the installer's log:"));
    // As written: the log quotes what the system's tools said.
    notes_->setPlainText(log);
    enterState(State::Failed);
}

void UpdateDialog::showRestarting()
{
    showsLog_ = false;
    headline_->setText(tr("Restarting GitBolt to finish the update…"));
    message_->setText(tr("GitBolt %1 is put in place once GitBolt has quit, and then it starts.")
                          .arg(version_));
    progress_->setRange(0, 0);   // busy, no end known
    enterState(State::Restarting);
}

void UpdateDialog::showQuitNeeded()
{
    showsLog_ = false;
    headline_->setText(tr("Quit GitBolt to finish installing %1.").arg(version_));
    message_->setText(tr("GitBolt was asked to quit and is still running: something it is doing, or a "
                         "window that is still open, kept it. GitBolt %1 is put in place when GitBolt "
                         "quits, if that is within 10 minutes; after that the update is abandoned and "
                         "nothing changes.")
                          .arg(version_));
    enterState(State::QuitNeeded);
}

void UpdateDialog::showDeclined(const QString& howToInstall)
{
    showsLog_ = false;
    headline_->setText(tr("GitBolt %1 wasn't installed").arg(version_));
    QString text = tr("Windows didn't give permission (the prompt was declined, or not answered in time), so "
                      "nothing was installed.");
    if (!howToInstall.isEmpty())
        text += QStringLiteral("\n\n") + howToInstall;
    message_->setText(text);
    enterState(State::Declined);
}

QString UpdateDialog::shownText() const
{
    QStringList lines;
    for (const QLabel* label : {headline_, message_, reason_, progressText_}) {
        if (!label->isHidden() && !label->text().isEmpty())
            lines << label->text();
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace gitbolt::dialogs
