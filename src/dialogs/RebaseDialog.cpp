#include "dialogs/RebaseDialog.h"
#include "conf/SettingsService.h"
#include "widgets/InteractiveRebaseWidget.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

RebaseDialog::RebaseDialog(QWidget* parent)
    : QDialog(parent)
    , branchCombo_(new QComboBox(this))
    , refEdit_(new QLineEdit(this))
    , previewLabel_(new QLabel(tr("Commits to rebase:"), this))
    , previewList_(new QListWidget(this))
    , rebaseWidget_(new widgets::InteractiveRebaseWidget(this))
{
    setupUi();
}

void RebaseDialog::setupUi() {
    setWindowTitle(tr("Interactive Rebase"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "rebase");

    auto* mainLayout = new QVBoxLayout(this);

    // --- Target selection ---
    auto* targetGroup = new QGroupBox(tr("Rebase Target"), this);
    auto* targetLayout = new QVBoxLayout(targetGroup);

    auto* branchRow = new QHBoxLayout;
    branchRow->addWidget(new QLabel(tr("Branch:"), this));
    branchCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    branchRow->addWidget(branchCombo_, 1);
    targetLayout->addLayout(branchRow);

    auto* refRow = new QHBoxLayout;
    refRow->addWidget(new QLabel(tr("Or ref:"), this));
    refEdit_->setPlaceholderText(tr("e.g. HEAD~5, origin/main, a1b2c3d"));
    refRow->addWidget(refEdit_, 1);
    targetLayout->addLayout(refRow);

    mainLayout->addWidget(targetGroup);

    // --- Preview / rebase widget ---
    auto* splitter = new QSplitter(Qt::Vertical, this);

    // Preview (compact list of commits that will be rebased)
    auto* previewContainer = new QWidget(this);
    auto* previewLayout = new QVBoxLayout(previewContainer);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLabel_->setStyleSheet(
        QStringLiteral("QLabel { font-weight: bold; padding: 2px 0; }"));
    previewLayout->addWidget(previewLabel_);
    previewList_->setMaximumHeight(120);
    previewList_->setAlternatingRowColors(true);
    previewLayout->addWidget(previewList_);
    splitter->addWidget(previewContainer);

    // Interactive rebase widget
    splitter->addWidget(rebaseWidget_);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter, 1);

    // --- Dialog buttons ---
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Rebase"));
    connect(buttons, &QDialogButtonBox::accepted, this, &RebaseDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttons);

    // Connections
    // A pick, not an index change: picking the branch that is picked
    // already lists its commits again, as a refusal for a moved HEAD
    // asks ("Pick the target again").
    connect(branchCombo_, &QComboBox::activated,
            this, &RebaseDialog::onBranchSelected);
    connect(refEdit_, &QLineEdit::editingFinished,
            this, &RebaseDialog::onRefEdited);
}

void RebaseDialog::setBranches(const std::vector<git::BranchInfo>& branches) {
    branchCombo_->clear();
    branchCombo_->addItem(tr("(select branch)"), QString());
    for (const auto& b : branches) {
        QString name = QString::fromStdString(b.name);
        branchCombo_->addItem(name, name);
    }
}

void RebaseDialog::setCommitsToRebase(const std::vector<git::CommitData>& commits,
                                       const git::ObjectId& onto, const git::ObjectId& head,
                                       const std::string& branch) {
    // Fill preview list
    previewList_->clear();
    int replayed = 0;
    for (const auto& c : commits) {
        QString entry = QStringLiteral("%1  %2")
                            .arg(QString::fromStdString(c.id.toShortHex()))
                            .arg(QString::fromStdString(
                                c.summary.empty() ? c.message : c.summary));
        if (!c.isMerge()) {
            previewList_->addItem(entry);
            ++replayed;
            continue;
        }
        // Listed, so the history still reads as it is, but not in the
        // plan below: git leaves merges out of a rebase.
        auto* item = new QListWidgetItem(tr("%1  (merge, not replayed)").arg(entry), previewList_);
        item->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
        item->setToolTip(tr("A rebase leaves merge commits out: the commits under the merge "
                            "are replayed in a line instead."));
    }
    previewLabel_->setText(tr("Commits to rebase (%1):").arg(replayed));

    // Feed into the interactive rebase widget
    rebaseWidget_->setCommits(commits, onto, head, branch);
}

git::RebasePlan RebaseDialog::rebasePlan() const {
    return rebaseWidget_->rebasePlan();
}

void RebaseDialog::refuse(const QString& reason) {
    refusal_ = reason;
}

void RebaseDialog::onBranchSelected(int index) {
    if (index <= 0)
        return;
    QString ref = branchCombo_->itemData(index).toString();
    if (!ref.isEmpty()) {
        refEdit_->clear();
        emit targetRefChanged(ref);
    }
}

void RebaseDialog::onRefEdited() {
    QString ref = refEdit_->text().trimmed();
    if (!ref.isEmpty()) {
        branchCombo_->setCurrentIndex(0);
        emit targetRefChanged(ref);
    }
}

void RebaseDialog::onAccepted() {
    auto plan = rebasePlan();
    if (plan.operations.empty()) {
        // Nothing to rebase
        reject();
        return;
    }
    // git would stop at once on this one; say why here, with the
    // plan still open to fix.
    if (const QString problem = rebaseWidget_->planProblem(); !problem.isEmpty()) {
        QMessageBox::warning(this, tr("Interactive Rebase"), problem);
        return;
    }
    // Turned down (a rebase step still running, HEAD moved since the
    // plan was made): said here, and the plan stays, to try again. It
    // used to close first, the reason coming after, the plan gone.
    refusal_.clear();
    emit rebaseRequested(plan);
    if (!refusal_.isEmpty()) {
        QMessageBox::warning(this, tr("Interactive Rebase"), refusal_);
        return;
    }
    accept();
}

} // namespace gitbolt::dialogs
