#include "dialogs/StashDialog.h"
#include "conf/SettingsService.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

StashDialog::StashDialog(QWidget* parent)
    : QDialog(parent)
    , messageEdit_(new QLineEdit(this))
    , untrackedCheck_(new QCheckBox(tr("Include untracked files"), this))
    , keepIndexCheck_(new QCheckBox(tr("Keep index (stash but keep staged changes)"), this))
{
    setupUi();
}

void StashDialog::setupUi() {
    setWindowTitle(tr("Save Stash"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "stash");

    auto* layout = new QVBoxLayout(this);

    // Message
    layout->addWidget(new QLabel(tr("Stash message:"), this));
    messageEdit_->setPlaceholderText(tr("Optional descriptive message for this stash"));
    layout->addWidget(messageEdit_);

    // Checkboxes
    layout->addSpacing(8);
    layout->addWidget(untrackedCheck_);
    layout->addWidget(keepIndexCheck_);

    layout->addStretch();

    // Buttons
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Save Stash"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString StashDialog::message() const {
    return messageEdit_->text().trimmed();
}

bool StashDialog::includeUntracked() const {
    return untrackedCheck_->isChecked();
}

bool StashDialog::keepIndex() const {
    return keepIndexCheck_->isChecked();
}

} // namespace gitbolt::dialogs
