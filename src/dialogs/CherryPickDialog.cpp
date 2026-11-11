#include "dialogs/CherryPickDialog.h"
#include "conf/SettingsService.h"
#include "util/MessageText.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimeZone>
#include <QVBoxLayout>
#include <chrono>

namespace gitbolt::dialogs {

CherryPickDialog::CherryPickDialog(QWidget* parent, Mode mode)
    : QDialog(parent)
    , mode_(mode)
    , hashEdit_(new QLineEdit(this))
    , summaryLabel_(new QLabel(this))
    , authorLabel_(new QLabel(this))
    , dateLabel_(new QLabel(this))
    , hashLabel_(new QLabel(this))
{
    setupUi();
}

void CherryPickDialog::setupUi() {
    const bool revert = mode_ == Mode::Revert;
    setWindowTitle(revert ? tr("Revert Commit") : tr("Cherry-Pick Commit"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, revert ? "revertCommit" : "cherryPick");

    auto* mainLayout = new QVBoxLayout(this);

    // --- Input row ---
    auto* inputLayout = new QHBoxLayout;
    inputLayout->addWidget(new QLabel(tr("Commit:"), this));
    hashEdit_->setObjectName(revert ? QStringLiteral("revert.commit")
                                    : QStringLiteral("cherryPick.commit"));
    hashEdit_->setPlaceholderText(tr("Enter commit hash"));
    hashEdit_->setFont(QFont(QStringLiteral("Menlo,Consolas,monospace")));
    inputLayout->addWidget(hashEdit_, 1);
    mainLayout->addLayout(inputLayout);

    // --- Commit details group ---
    auto* detailsGroup = new QGroupBox(tr("Commit Details"), this);
    auto* detailsLayout = new QFormLayout(detailsGroup);
    detailsForm_ = detailsLayout;

    hashLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hashLabel_->setFont(QFont(QStringLiteral("Menlo,Consolas,monospace")));
    detailsLayout->addRow(tr("Hash:"), hashLabel_);

    // The commit's message and author are its author's to write (a
    // fetched commit is anyone's): plain text, or a summary such as
    // "<b>…</b>" was rendered.
    summaryLabel_->setTextFormat(Qt::PlainText);
    authorLabel_->setTextFormat(Qt::PlainText);
    summaryLabel_->setWordWrap(true);
    detailsLayout->addRow(tr("Message:"), summaryLabel_);

    detailsLayout->addRow(tr("Author:"), authorLabel_);
    detailsLayout->addRow(tr("Date:"), dateLabel_);

    // A merge is picked or reverted as seen from one of its parents
    // (git cherry-pick -m, git revert -m): a row shown only for a merge.
    parentCombo_ = new QComboBox(this);
    parentCombo_->setObjectName(revert ? QStringLiteral("revert.parent")
                                       : QStringLiteral("cherryPick.parent"));
    parentCombo_->setToolTip(
        revert ? tr("The revert undoes the changes the merge brought in, as seen from this "
                    "parent. The first parent is usually the branch that was merged into.")
               : tr("The pick applies the changes the merge brought in, as seen from this "
                    "parent. The first parent is usually the branch that was merged into."));
    detailsLayout->addRow(tr("Parent:"), parentCombo_);
    detailsLayout->setRowVisible(parentCombo_, false);

    mainLayout->addWidget(detailsGroup);
    if (revert) {
        auto* note = new QLabel(
            tr("Revert stages the changes that undo this commit; it doesn't commit them. "
               "Commit them to finish the revert, or use Abort on the bar that appears "
               "to go back."),
            this);
        note->setObjectName(QStringLiteral("revert.note"));
        note->setWordWrap(true);
        mainLayout->addWidget(note);
    }
    mainLayout->addStretch();

    // --- Dialog buttons ---
    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(revert ? tr("Revert") : tr("Cherry-Pick"));
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox_);

    // Connections
    connect(hashEdit_, &QLineEdit::textChanged,
            this, &CherryPickDialog::onHashEdited);
}

QString CherryPickDialog::commitHash() const {
    return hashEdit_->text().trimmed();
}

// Selected, so that typing or pasting a hash replaces the one filled in.
void CherryPickDialog::setCommitHash(const QString& hash) {
    hashEdit_->setText(hash);
    hashEdit_->selectAll();
}

unsigned CherryPickDialog::mainline() const {
    if (!merge_ || parentCombo_->currentIndex() < 0)
        return 0;
    return static_cast<unsigned>(parentCombo_->currentIndex()) + 1;
}

void CherryPickDialog::setCommitDetails(const git::CommitData& commit,
                                        const QStringList& parentSummaries) {
    hashLabel_->setText(QString::fromStdString(commit.id.toHex()));
    // At most util::kMaxLineChars: the label wraps its text, and laying
    // out a subject of one line of megabytes froze the dialog. The same
    // for the author, whose ident git takes at any length.
    summaryLabel_->setText(util::elided(
        commit.summary.empty() ? commit.message : commit.summary));
    // In one arg(): chained, a "%2" in the name took the email.
    authorLabel_->setText(QStringLiteral("%1 <%2>")
                              .arg(util::elided(commit.author.name),
                                   util::elided(commit.author.email)));

    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     commit.author.when.time_since_epoch())
                     .count();
    QDateTime dt = QDateTime::fromSecsSinceEpoch(
        static_cast<qint64>(epoch), QTimeZone::LocalTime);
    dateLabel_->setText(dt.toString(QStringLiteral("yyyy-MM-dd hh:mm:ss")));

    // The parents' subjects are their authors' text: a combo shows
    // items as plain text, at most util::kMaxLineChars of each. The
    // parent chosen stays while the field's edits name the same merge.
    const int chosen = previewed_ == commit.id ? parentCombo_->currentIndex() : 0;
    previewed_ = commit.id;
    merge_ = commit.isMerge();
    parentCombo_->clear();
    for (size_t i = 0; merge_ && i < commit.parentIds.size(); ++i) {
        const QString line = i < static_cast<size_t>(parentSummaries.size())
            ? util::elided(parentSummaries.at(static_cast<qsizetype>(i)))
            : QString::fromStdString(commit.parentIds[i].toShortHex());
        parentCombo_->addItem(QStringLiteral("%1: %2").arg(i + 1).arg(line));
    }
    if (chosen > 0 && chosen < parentCombo_->count())
        parentCombo_->setCurrentIndex(chosen);
    detailsForm_->setRowVisible(parentCombo_, merge_);

    if (buttonBox_)
        buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void CherryPickDialog::clearCommitDetails() {
    hashLabel_->clear();
    summaryLabel_->clear();
    authorLabel_->clear();
    dateLabel_->clear();
    previewed_.reset();
    merge_ = false;
    parentCombo_->clear();
    detailsForm_->setRowVisible(parentCombo_, false);

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
