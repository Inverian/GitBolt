#include "widgets/BlameWidget.h"

#include <QAction>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>

namespace gitbolt::widgets {

BlameWidget::BlameWidget(QWidget* parent)
    : QWidget(parent)
    , tableView_(new QTableView(this))
    , model_(new models::BlameModel(this))
    , toolbar_(new QToolBar(this))
    , filePathLabel_(new QLabel(this))
    , blameBeforeBtn_(new QPushButton(tr("Blame Before"), this))
{
    setupUI();
}

void BlameWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    filePathLabel_->setStyleSheet(
        QStringLiteral("QLabel { padding: 2px 8px; font-weight: bold; }"));
    toolbar_->addWidget(filePathLabel_);
    toolbar_->addSeparator();

    blameBeforeBtn_->setEnabled(false);
    blameBeforeBtn_->setToolTip(tr("Re-blame at the parent of the selected commit"));
    toolbar_->addWidget(blameBeforeBtn_);

    layout->addWidget(toolbar_);

    // Table view
    tableView_->setModel(model_);
    tableView_->setAlternatingRowColors(false);  // we use hunk-based coloring
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->setShowGrid(false);
    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->verticalHeader()->setDefaultSectionSize(20);

    // Column widths
    auto* hdr = tableView_->horizontalHeader();
    hdr->setStretchLastSection(true);
    hdr->setSectionResizeMode(QHeaderView::ResizeToContents);

    layout->addWidget(tableView_, 1);

    // Connections
    connect(tableView_, &QTableView::clicked,
            this, &BlameWidget::onRowClicked);
    connect(tableView_, &QTableView::customContextMenuRequested,
            this, &BlameWidget::onContextMenu);
    connect(blameBeforeBtn_, &QPushButton::clicked,
            this, &BlameWidget::onBlameBeforeClicked);
}

void BlameWidget::setBlameResult(git::BlameResult result) {
    filePathLabel_->setText(QString::fromStdString(result.path));
    model_->setBlameResult(std::move(result));

    // Reset selection state
    selectedCommitId_ = git::ObjectId();
    blameBeforeBtn_->setEnabled(false);
}

void BlameWidget::clear() {
    model_->clear();
    filePathLabel_->clear();
    selectedCommitId_ = git::ObjectId();
    blameBeforeBtn_->setEnabled(false);
}

void BlameWidget::onRowClicked(const QModelIndex& index) {
    if (!index.isValid())
        return;

    int row = index.row();
    selectedCommitId_ = model_->commitIdAtRow(row);
    blameBeforeBtn_->setEnabled(!selectedCommitId_.isZero());

    emit commitSelected(selectedCommitId_);
}

void BlameWidget::onContextMenu(const QPoint& pos) {
    QModelIndex index = tableView_->indexAt(pos);
    if (!index.isValid())
        return;

    int row = index.row();
    auto commitId = model_->commitIdAtRow(row);
    if (commitId.isZero())
        return;

    QMenu menu(this);

    auto* viewCommitAction = menu.addAction(
        tr("View commit %1").arg(QString::fromStdString(commitId.toShortHex())));
    connect(viewCommitAction, &QAction::triggered, this, [this, commitId]() {
        emit commitSelected(commitId);
    });

    auto* blameBeforeAction = menu.addAction(
        tr("Blame before this commit"));
    connect(blameBeforeAction, &QAction::triggered, this, [this, commitId]() {
        selectedCommitId_ = commitId;
        onBlameBeforeClicked();
    });

    menu.exec(tableView_->viewport()->mapToGlobal(pos));
}

void BlameWidget::onBlameBeforeClicked() {
    if (selectedCommitId_.isZero())
        return;

    QString filePath = model_->filePathDisplayed();
    emit blameBeforeRequested(selectedCommitId_, filePath);
}

} // namespace gitbolt::widgets
