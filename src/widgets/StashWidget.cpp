#include "widgets/StashWidget.h"
#include "models/StashModel.h"
#include "widgets/DiffViewerWidget.h"

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

namespace gitbolt::widgets {

StashWidget::StashWidget(QWidget* parent)
    : QWidget(parent)
    , tableView_(new QTableView(this))
    , model_(new models::StashModel(this))
    , toolbar_(new QToolBar(this))
    , saveBtn_(new QPushButton(tr("Save Stash"), this))
    , applyBtn_(new QPushButton(tr("Apply"), this))
    , popBtn_(new QPushButton(tr("Pop"), this))
    , dropBtn_(new QPushButton(tr("Drop"), this))
{
    setupUi();
}

void StashWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    toolbar_->addWidget(saveBtn_);
    toolbar_->addSeparator();
    toolbar_->addWidget(applyBtn_);
    toolbar_->addWidget(popBtn_);
    toolbar_->addWidget(dropBtn_);

    applyBtn_->setEnabled(false);
    popBtn_->setEnabled(false);
    dropBtn_->setEnabled(false);

    layout->addWidget(toolbar_);

    // Table view
    tableView_->setModel(model_);
    tableView_->setAlternatingRowColors(true);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->setShowGrid(false);
    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->verticalHeader()->setDefaultSectionSize(22);

    auto* hdr = tableView_->horizontalHeader();
    hdr->setStretchLastSection(true);
    hdr->setSectionResizeMode(QHeaderView::ResizeToContents);

    layout->addWidget(tableView_, 1);

    // Connections -- toolbar buttons
    connect(saveBtn_, &QPushButton::clicked,
            this, &StashWidget::onSaveStash);
    connect(applyBtn_, &QPushButton::clicked,
            this, &StashWidget::onApplyStash);
    connect(popBtn_, &QPushButton::clicked,
            this, &StashWidget::onPopStash);
    connect(dropBtn_, &QPushButton::clicked,
            this, &StashWidget::onDropStash);

    // Context menu
    connect(tableView_, &QTableView::customContextMenuRequested,
            this, &StashWidget::showContextMenu);

    // Enable/disable buttons based on selection
    connect(tableView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this]() {
                bool hasSelection = tableView_->selectionModel()->hasSelection();
                applyBtn_->setEnabled(hasSelection);
                popBtn_->setEnabled(hasSelection);
                dropBtn_->setEnabled(hasSelection);
            });
}

void StashWidget::setStashes(std::vector<git::StashEntry> stashes) {
    model_->setStashes(std::move(stashes));

    // Reset button state
    bool hasRows = model_->rowCount() > 0;
    applyBtn_->setEnabled(false);
    popBtn_->setEnabled(false);
    dropBtn_->setEnabled(false);
    Q_UNUSED(hasRows)
}

void StashWidget::clear() {
    model_->clear();
    applyBtn_->setEnabled(false);
    popBtn_->setEnabled(false);
    dropBtn_->setEnabled(false);
}

int StashWidget::selectedStashIndex() const {
    auto indexes = tableView_->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return -1;
    return static_cast<int>(model_->stashIndexAtRow(indexes.first().row()));
}

void StashWidget::onSaveStash() {
    // Build a small dialog with message + "include untracked" checkbox
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save Stash"));
    auto* dlgLayout = new QVBoxLayout(&dlg);

    dlgLayout->addWidget(new QLabel(tr("Stash message:"), &dlg));
    auto* messageEdit = new QLineEdit(&dlg);
    messageEdit->setPlaceholderText(tr("Optional stash message"));
    dlgLayout->addWidget(messageEdit);

    auto* untrackedCheck = new QCheckBox(tr("Include untracked files"), &dlg);
    dlgLayout->addWidget(untrackedCheck);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Save"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlgLayout->addWidget(buttons);

    if (dlg.exec() == QDialog::Accepted) {
        emit stashSaveRequested(messageEdit->text(), untrackedCheck->isChecked());
    }
}

void StashWidget::onApplyStash() {
    int idx = selectedStashIndex();
    if (idx >= 0)
        emit stashApplyRequested(idx);
}

void StashWidget::onPopStash() {
    int idx = selectedStashIndex();
    if (idx >= 0)
        emit stashPopRequested(idx);
}

void StashWidget::onDropStash() {
    int idx = selectedStashIndex();
    if (idx < 0)
        return;

    auto reply = QMessageBox::question(
        this, tr("Drop Stash"),
        tr("Are you sure you want to drop stash@{%1}? This cannot be undone.").arg(idx),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes)
        emit stashDropRequested(idx);
}

void StashWidget::onViewChanges() {
    auto indexes = tableView_->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return;

    // Open a DiffViewerWidget in a popup dialog
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Stash Changes"));
    dlg->resize(700, 500);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* viewer = new DiffViewerWidget(dlg);
    layout->addWidget(viewer);

    // Note: the actual diff data would be fetched by the service layer
    // and passed back. For now we display an empty viewer that the caller
    // can populate via the DiffViewerWidget API.
    dlg->show();
}

void StashWidget::showContextMenu(const QPoint& pos) {
    QModelIndex index = tableView_->indexAt(pos);
    if (!index.isValid())
        return;

    int stashIdx = static_cast<int>(model_->stashIndexAtRow(index.row()));

    QMenu menu(this);

    auto* applyAction = menu.addAction(tr("Apply stash@{%1}").arg(stashIdx));
    connect(applyAction, &QAction::triggered, this, [this, stashIdx]() {
        emit stashApplyRequested(stashIdx);
    });

    auto* popAction = menu.addAction(tr("Pop stash@{%1}").arg(stashIdx));
    connect(popAction, &QAction::triggered, this, [this, stashIdx]() {
        emit stashPopRequested(stashIdx);
    });

    auto* dropAction = menu.addAction(tr("Drop stash@{%1}").arg(stashIdx));
    connect(dropAction, &QAction::triggered, this, [this, stashIdx]() {
        auto reply = QMessageBox::question(
            this, tr("Drop Stash"),
            tr("Are you sure you want to drop stash@{%1}?").arg(stashIdx),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes)
            emit stashDropRequested(stashIdx);
    });

    menu.addSeparator();

    auto* viewAction = menu.addAction(tr("View Changes"));
    connect(viewAction, &QAction::triggered, this, &StashWidget::onViewChanges);

    menu.exec(tableView_->viewport()->mapToGlobal(pos));
}

} // namespace gitbolt::widgets
