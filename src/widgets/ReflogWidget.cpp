#include "widgets/ReflogWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ReflogWidget::ReflogWidget(QWidget* parent)
    : QWidget(parent)
    , tableView_(new QTableView(this))
    , model_(new models::ReflogModel(this))
    , toolbar_(new QToolBar(this))
{
    setupUI();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void ReflogWidget::setEntries(std::vector<models::ReflogEntry> entries) {
    model_->setEntries(std::move(entries));
    tableView_->resizeColumnsToContents();
}

void ReflogWidget::clear() {
    model_->clear();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void ReflogWidget::onRowDoubleClicked(const QModelIndex& index) {
    if (!index.isValid())
        return;

    const auto id = model_->objectIdAtRow(index.row());
    if (!id.isZero())
        emit commitSelected(id);
}

void ReflogWidget::onContextMenu(const QPoint& pos) {
    const QModelIndex index = tableView_->indexAt(pos);
    if (!index.isValid())
        return;

    const auto id = model_->objectIdAtRow(index.row());
    if (id.isZero())
        return;

    const QString shortHash = QString::fromStdString(id.toShortHex());
    const QString fullHash  = QString::fromStdString(id.toHex());

    QMenu menu(this);

    menu.addAction(
        tr("Checkout this commit (%1)").arg(shortHash),
        this, [this, id]() {
            emit checkoutRequested(id);
        });

    menu.addAction(
        tr("Reset to here (%1)").arg(shortHash),
        this, [this, id]() {
            emit resetRequested(id);
        });

    menu.addSeparator();

    menu.addAction(tr("Copy Hash"), this, [fullHash]() {
        QApplication::clipboard()->setText(fullHash);
    });

    menu.exec(tableView_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void ReflogWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    refreshAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("view-refresh")),
        tr("Refresh"));
    connect(refreshAction_, &QAction::triggered,
            this, &ReflogWidget::refreshRequested);

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

    // Connections
    connect(tableView_, &QTableView::doubleClicked,
            this, &ReflogWidget::onRowDoubleClicked);
    connect(tableView_, &QTableView::customContextMenuRequested,
            this, &ReflogWidget::onContextMenu);
}

} // namespace gitbolt::widgets
