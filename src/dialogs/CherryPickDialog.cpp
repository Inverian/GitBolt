#include "dialogs/CherryPickDialog.h"
#include "conf/SettingsService.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <chrono>

namespace gitbolt::dialogs {

CherryPickDialog::CherryPickDialog(QWidget* parent)
    : QDialog(parent)
    , hashEdit_(new QLineEdit(this))
    , browseBtn_(new QPushButton(tr("Browse..."), this))
    , summaryLabel_(new QLabel(this))
    , authorLabel_(new QLabel(this))
    , dateLabel_(new QLabel(this))
    , hashLabel_(new QLabel(this))
{
    setupUi();
}

void CherryPickDialog::setupUi() {
    setWindowTitle(tr("Cherry-Pick Commit"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "cherryPick");

    auto* mainLayout = new QVBoxLayout(this);

    // --- Input row ---
    auto* inputLayout = new QHBoxLayout;
    inputLayout->addWidget(new QLabel(tr("Commit:"), this));
    hashEdit_->setPlaceholderText(tr("Enter commit hash"));
    hashEdit_->setFont(QFont(QStringLiteral("Menlo,Consolas,monospace")));
    inputLayout->addWidget(hashEdit_, 1);
    inputLayout->addWidget(browseBtn_);
    mainLayout->addLayout(inputLayout);

    // --- Commit details group ---
    auto* detailsGroup = new QGroupBox(tr("Commit Details"), this);
    auto* detailsLayout = new QFormLayout(detailsGroup);

    hashLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hashLabel_->setFont(QFont(QStringLiteral("Menlo,Consolas,monospace")));
    detailsLayout->addRow(tr("Hash:"), hashLabel_);

    summaryLabel_->setWordWrap(true);
    detailsLayout->addRow(tr("Message:"), summaryLabel_);

    detailsLayout->addRow(tr("Author:"), authorLabel_);
    detailsLayout->addRow(tr("Date:"), dateLabel_);

    mainLayout->addWidget(detailsGroup);
    mainLayout->addStretch();

    // --- Dialog buttons ---
    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Cherry-Pick"));
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox_);

    // Connections
    connect(hashEdit_, &QLineEdit::textChanged,
            this, &CherryPickDialog::onHashEdited);
    connect(browseBtn_, &QPushButton::clicked,
            this, &CherryPickDialog::browseCommitsRequested);
}

QString CherryPickDialog::commitHash() const {
    return hashEdit_->text().trimmed();
}

void CherryPickDialog::setCommitDetails(const git::CommitData& commit) {
    hashLabel_->setText(QString::fromStdString(commit.id.toHex()));
    summaryLabel_->setText(QString::fromStdString(
        commit.summary.empty() ? commit.message : commit.summary));
    authorLabel_->setText(QStringLiteral("%1 <%2>")
                              .arg(QString::fromStdString(commit.author.name))
                              .arg(QString::fromStdString(commit.author.email)));

    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     commit.author.when.time_since_epoch())
                     .count();
    QDateTime dt = QDateTime::fromSecsSinceEpoch(
        static_cast<qint64>(epoch), Qt::LocalTime);
    dateLabel_->setText(dt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));

    if (buttonBox_)
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void CherryPickDialog::clearCommitDetails() {
    hashLabel_->clear();
    summaryLabel_->clear();
    authorLabel_->clear();
    dateLabel_->clear();

    if (buttonBox_)
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
}

void CherryPickDialog::onHashEdited() {
    QString hash = hashEdit_->text().trimmed();
    if (hash.length() >= 4)
        emit commitHashChanged(hash);
    else
        clearCommitDetails();
}

} // namespace gitbolt::dialogs
