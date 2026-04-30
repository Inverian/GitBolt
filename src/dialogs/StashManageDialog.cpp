#include "dialogs/StashManageDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

StashManageDialog::StashManageDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
}

void StashManageDialog::setStashes(
        const std::vector<gitbolt::git::StashEntry>& stashes) {
    list_->clear();
    for (const auto& s : stashes) {
        // Display: "stash@{0}: WIP on main: 7841a88 fix bug"
        // The index is what `git stash apply N` takes, so we keep
        // it as the QListWidgetItem's data role for the buttons to
        // resolve back to a number.
        const QString label = QStringLiteral("stash@{%1}: %2")
            .arg(s.index)
            .arg(QString::fromStdString(s.message));
        auto* item = new QListWidgetItem(label, list_);
        item->setData(Qt::UserRole, static_cast<qulonglong>(s.index));
    }
    onSelectionChanged();
}

void StashManageDialog::setupUi() {
    setWindowTitle(tr("Manage Stashes"));
    resize(640, 380);

    auto* layout = new QVBoxLayout(this);

    auto* header = new QLabel(
        tr("Existing stashes (most recent first):"), this);
    layout->addWidget(header);

    list_ = new QListWidget(this);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(list_, /*stretch=*/1);
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &StashManageDialog::onSelectionChanged);

    // Action button row. Apply/Pop/Drop operate on the selected
    // stash; New saves the current changes; Close dismisses the
    // dialog. Drop is colored as a destructive action via setting
    // its role on the eventual button box, but we use a flat
    // QPushButton row so we can keep the action buttons grouped
    // tightly on the left and Close on the right.
    auto* btnRow = new QHBoxLayout();
    applyBtn_ = new QPushButton(tr("&Apply"), this);
    applyBtn_->setToolTip(tr("Apply selected stash to the working tree, "
                             "keeping it in the stash list."));
    popBtn_   = new QPushButton(tr("&Pop"), this);
    popBtn_->setToolTip(tr("Apply selected stash to the working tree "
                           "and drop it on success."));
    dropBtn_  = new QPushButton(tr("&Drop"), this);
    dropBtn_->setToolTip(tr("Discard the selected stash without "
                            "applying it. Cannot be undone."));
    newBtn_   = new QPushButton(tr("&New stash..."), this);
    newBtn_->setToolTip(tr("Save the current working tree as a new stash."));
    closeBtn_ = new QPushButton(tr("&Close"), this);

    btnRow->addWidget(applyBtn_);
    btnRow->addWidget(popBtn_);
    btnRow->addWidget(dropBtn_);
    btnRow->addStretch(1);
    btnRow->addWidget(newBtn_);
    btnRow->addWidget(closeBtn_);
    layout->addLayout(btnRow);

    connect(applyBtn_, &QPushButton::clicked, this, [this]() {
        const int i = selectedIndex();
        if (i >= 0) emit applyRequested(static_cast<size_t>(i));
    });
    connect(popBtn_, &QPushButton::clicked, this, [this]() {
        const int i = selectedIndex();
        if (i >= 0) emit popRequested(static_cast<size_t>(i));
    });
    connect(dropBtn_, &QPushButton::clicked, this, [this]() {
        const int i = selectedIndex();
        if (i >= 0) emit dropRequested(static_cast<size_t>(i));
    });
    connect(newBtn_, &QPushButton::clicked, this, [this]() {
        emit newStashRequested();
    });
    connect(closeBtn_, &QPushButton::clicked,
            this, &QDialog::accept);

    onSelectionChanged();
}

void StashManageDialog::onSelectionChanged() {
    const bool any = list_->currentItem() != nullptr;
    applyBtn_->setEnabled(any);
    popBtn_->setEnabled(any);
    dropBtn_->setEnabled(any);
}

int StashManageDialog::selectedIndex() const {
    auto* item = list_->currentItem();
    if (!item) return -1;
    bool ok = false;
    const qulonglong idx = item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) return -1;
    return static_cast<int>(idx);
}

} // namespace gitbolt::dialogs
