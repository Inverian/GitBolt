#include "widgets/GitNoticeBar.h"
#include "widgets/NoteBarColors.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QUrl>

namespace gitbolt::widgets {

GitNoticeBar::GitNoticeBar(QWidget* parent)
    : QFrame(parent)
    , message_(new QLabel(this))
    , downloadButton_(new QPushButton(tr("Get Git…"), this))
    , dismissButton_(new QPushButton(tr("Dismiss"), this))
{
    setObjectName(QStringLiteral("gitNotice"));
    message_->setObjectName(QStringLiteral("gitNotice.message"));
    downloadButton_->setObjectName(QStringLiteral("gitNotice.download"));
    dismissButton_->setObjectName(QStringLiteral("gitNotice.dismiss"));
    setAutoFillBackground(true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 7);   // + the bottom line
    // Plain text: it names a path on the user's disk.
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(message_, 1);
    for (QPushButton* button : {downloadButton_, dismissButton_}) {
        button->setAutoDefault(false);
        row->addWidget(button);
    }
    downloadButton_->setToolTip(QString::fromLatin1(kDownloadsUrl));

    connect(downloadButton_, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QString::fromLatin1(kDownloadsUrl)));
    });
    connect(dismissButton_, &QPushButton::clicked, this, [this] {
        hide();
        emit dismissed(key_);
    });

    applyColors();
    hide();
}

std::optional<GitNotice> GitNoticeBar::noticeFor(const git::GitProbe& probe,
                                                 git::GitPlatform platform,
                                                 const git::GitAdvisories& table)
{
    if (probe.path.isEmpty()) {
        return GitNotice{QStringLiteral("\nmissing"),
                         tr("GitBolt didn't find git, which it needs to clone, commit, fetch "
                            "and push. Install git, then restart GitBolt.")};
    }
    const QString where = QDir::toNativeSeparators(probe.path);
    if (probe.appleToolsMissing) {
        return GitNotice{probe.path + QStringLiteral("\ntools missing"),
                         tr("The git at %1 needs Apple's Command Line Tools, which aren't "
                            "installed. Install them (xcode-select --install in Terminal) or "
                            "git from Homebrew, then restart GitBolt.").arg(where)};
    }
    if (!probe.version)
        return std::nullopt;
    const git::GitVersion& version = *probe.version;
    const git::GitAssessment assessment = git::assessGitVersion(version, platform, table);
    if (assessment.ok())
        return std::nullopt;

    QString advice;
    if (version.build == git::GitVersion::Build::Apple) {
        // Xcode 26 and its tools need macOS 15.6; Homebrew's git runs on
        // every macOS GitBolt does.
        advice = tr("Install git from Homebrew (brew install git), or on macOS 15.6 or later "
                    "update Apple's Command Line Tools to version 26 (in Software Update), "
                    "then restart GitBolt.");
    } else if (version.build == git::GitVersion::Build::Windows) {
        advice = tr("Update Git for Windows, then restart GitBolt.");
    } else if (platform == git::GitPlatform::MacOS) {
        advice = tr("Update git (brew upgrade git, if it came from Homebrew), then restart "
                    "GitBolt.");
    } else {
        advice = tr("Update git, then restart GitBolt.");
    }

    QStringList key{probe.path, version.text};
    if (assessment.advisories.empty()) {
        key << QStringLiteral("floor");
        const QString floor =
            QStringLiteral("%1.%2").arg(table.floor.major).arg(table.floor.minor);
        return GitNotice{key.join(QLatin1Char('\n')),
                         tr("git %1, which GitBolt runs from %2, is too old: GitBolt needs "
                            "git %3 or later, and some of its commands fail with this one. %4")
                             .arg(version.text, where, floor, advice)};
    }

    QStringList ids;
    bool runsCode = false;
    for (const git::GitAdvisory* advisory : assessment.advisories) {
        ids << advisory->ids.first();
        runsCode = runsCode || advisory->risk == git::GitAdvisory::Risk::RunsCode;
    }
    key << ids;
    const QString named = ids.size() == 1 ? ids.first()
                        : ids.size() == 2 ? tr("%1 and %2").arg(ids.at(0), ids.at(1))
                                          : tr("%1, %2 and others").arg(ids.at(0), ids.at(1));
    // The worst of what they let a repository or server do.
    const QString risk = runsCode
        ? tr("a repository you clone could run programs on your computer")
        : tr("a repository or server could get your Windows password hash");
    return GitNotice{key.join(QLatin1Char('\n')),
                     tr("git %1, which GitBolt runs from %2, has known security flaws (%3): "
                        "%4. %5")
                         .arg(version.text, where, named, risk, advice)};
}

void GitNoticeBar::showNotice(const GitNotice& notice)
{
    key_ = notice.key;
    message_->setText(notice.text);
    show();
}

QString GitNoticeBar::text() const
{
    return message_->text();
}

void GitNoticeBar::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    // As RepoOperationBar: a theme switch reaches the bar as the
    // PaletteChange its window passes down (applyColors() sends one
    // too, which is let go).
    if (event->type() == QEvent::PaletteChange && !applyingColors_)
        applyColors();
}

void GitNoticeBar::applyColors()
{
    const QScopedValueRollback<bool> applying(applyingColors_, true);
    const NoteBarColors colors = noteBarColors(QApplication::palette());
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, colors.background);
    palette.setColor(QPalette::WindowText, colors.text);
    setPalette(palette);
    border_ = colors.border;
    update();
}

void GitNoticeBar::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(QRect(0, height() - 1, width(), 1), border_);
}

} // namespace gitbolt::widgets
