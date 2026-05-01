#include "dialogs/WorktreeDialog.h"
#include "conf/SettingsService.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WorktreeDialog::WorktreeDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUI();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void WorktreeDialog::setBranches(const QStringList& branches) {
    branchCombo_->clear();
    branchCombo_->addItems(branches);
}

QString WorktreeDialog::worktreeName() const {
    return nameEdit_->text().trimmed();
}

QString WorktreeDialog::worktreePath() const {
    return pathEdit_->text().trimmed();
}

QString WorktreeDialog::branch() const {
    return branchCombo_->currentText();
}

bool WorktreeDialog::createNewBranch() const {
    return createBranchCheckBox_->isChecked();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void WorktreeDialog::onBrowseClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Worktree Directory"));
    if (!dir.isEmpty())
        pathEdit_->setText(dir);
}

void WorktreeDialog::onCreateNewBranchToggled(bool checked) {
    if (checked) {
        // Allow typing a new branch name directly into the combo
        branchCombo_->setEditable(true);
        branchCombo_->setToolTip(tr("Enter new branch name or select existing"));
    } else {
        branchCombo_->setEditable(false);
        branchCombo_->setToolTip(tr("Select a branch to checkout"));
    }
}

void WorktreeDialog::validateInput() {
    const bool valid = !nameEdit_->text().trimmed().isEmpty()
                    && !pathEdit_->text().trimmed().isEmpty();
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void WorktreeDialog::setupUI() {
    setWindowTitle(tr("Add Worktree"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "worktree");

    auto* mainLayout = new QVBoxLayout(this);

    // --- Form fields -------------------------------------------------------
    auto* formLayout = new QFormLayout;
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // Worktree name
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setPlaceholderText(tr("my-worktree"));
    formLayout->addRow(tr("Name:"), nameEdit_);

    // Path with browse button
    auto* pathLayout = new QHBoxLayout;
    pathEdit_ = new QLineEdit(this);
    pathEdit_->setPlaceholderText(tr("/path/to/worktree"));
    pathLayout->addWidget(pathEdit_);

    browseBtn_ = new QPushButton(tr("Browse..."), this);
    pathLayout->addWidget(browseBtn_);
    formLayout->addRow(tr("Path:"), pathLayout);

    // Branch combo
    branchCombo_ = new QComboBox(this);
    branchCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    branchCombo_->setToolTip(tr("Select a branch to checkout"));
    formLayout->addRow(tr("Branch:"), branchCombo_);

    mainLayout->addLayout(formLayout);

    // --- Create new branch checkbox ----------------------------------------
    createBranchCheckBox_ = new QCheckBox(tr("Create new branch"), this);
    mainLayout->addWidget(createBranchCheckBox_);

    mainLayout->addStretch();

    // --- Button box --------------------------------------------------------
    buttonBox_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttonBox_->button(QDialogButtonBox::Ok)->setText(tr("Add"));
    buttonBox_->button(QDialogButtonBox::Ok)->setEnabled(false);
    mainLayout->addWidget(buttonBox_);

    // --- Connections -------------------------------------------------------
    connect(browseBtn_, &QPushButton::clicked,
            this, &WorktreeDialog::onBrowseClicked);
    connect(createBranchCheckBox_, &QCheckBox::toggled,
            this, &WorktreeDialog::onCreateNewBranchToggled);
    connect(nameEdit_, &QLineEdit::textChanged,
            this, &WorktreeDialog::validateInput);
    connect(pathEdit_, &QLineEdit::textChanged,
            this, &WorktreeDialog::validateInput);
    connect(buttonBox_, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
}

} // namespace gitbolt::dialogs
