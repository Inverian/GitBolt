#include "dialogs/BranchPickerDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace gitbolt::dialogs {

BranchPickerDialog::BranchPickerDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
}

void BranchPickerDialog::setBranches(const QStringList& allBranches,
                                     const QStringList& selected) {
    branches_ = allBranches;
    // Sort alphabetically for stable display order. The walk
    // doesn't care about order, but users expect predictable
    // navigation when scanning the list.
    std::sort(branches_.begin(), branches_.end(),
              [](const QString& a, const QString& b) {
                  return a.compare(b, Qt::CaseInsensitive) < 0;
              });
    selected_ = selected;
    rebuildList();
}

QStringList BranchPickerDialog::selectedBranches() const {
    QStringList out;
    if (!list_) return out;
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        if (it && it->checkState() == Qt::Checked)
            out << it->text();
    }
    return out;
}

void BranchPickerDialog::setupUi() {
    setWindowTitle(tr("Filter Branches"));
    setMinimumWidth(420);
    setMinimumHeight(440);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 16);
    root->setSpacing(12);

    auto* intro = new QLabel(tr(
        "Pick which local branches the revision graph should walk. "
        "Only commits reachable from a checked branch will appear "
        "in the log. If nothing is checked, the log falls back to "
        "the current HEAD."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search branches..."));
    search_->setClearButtonEnabled(true);
    root->addWidget(search_);

    list_ = new QListWidget(this);
    // Keyboard-friendly: arrow keys move highlight, space toggles
    // the active row's checkbox. SingleSelection is enough since
    // the checkbox is the actual data, not the selection.
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setAlternatingRowColors(true);
    root->addWidget(list_, /*stretch=*/1);

    auto* btnRow = new QHBoxLayout();
    auto* selectAll = new QPushButton(tr("Select All"), this);
    auto* clearAll = new QPushButton(tr("Clear All"), this);
    btnRow->addWidget(selectAll);
    btnRow->addWidget(clearAll);
    btnRow->addStretch(1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Apply)->setDefault(true);
    btnRow->addWidget(buttons);
    root->addLayout(btnRow);

    connect(buttons->button(QDialogButtonBox::Apply),
            &QPushButton::clicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    connect(search_, &QLineEdit::textChanged,
            this, &BranchPickerDialog::applyFilter);

    // Select All / Clear All operate only on rows currently
    // visible in the list (i.e. after the search filter). Hidden
    // rows keep their previous check state, so a user who has
    // pre-selected "main" + "develop" and then types "feature/"
    // can Select All to add the feature branches without losing
    // their earlier picks.
    connect(selectAll, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem* it = list_->item(i);
            if (it && !it->isHidden())
                it->setCheckState(Qt::Checked);
        }
    });
    connect(clearAll, &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem* it = list_->item(i);
            if (it && !it->isHidden())
                it->setCheckState(Qt::Unchecked);
        }
    });
}

void BranchPickerDialog::rebuildList() {
    if (!list_) return;
    list_->clear();
    const QSet<QString> sel(selected_.begin(), selected_.end());
    for (const QString& name : branches_) {
        auto* it = new QListWidgetItem(name, list_);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(sel.contains(name) ? Qt::Checked : Qt::Unchecked);
    }
    if (search_) applyFilter(search_->text());
}

void BranchPickerDialog::applyFilter(const QString& needle) {
    if (!list_) return;
    const QString n = needle.trimmed();
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        if (!it) continue;
        const bool show = n.isEmpty() ||
                          it->text().contains(n, Qt::CaseInsensitive);
        it->setHidden(!show);
    }
}

} // namespace gitbolt::dialogs
