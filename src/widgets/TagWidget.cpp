#include "widgets/TagWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QMenu>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TagWidget::TagWidget(QWidget* parent)
    : QWidget(parent)
    , tableView_(new QTableView(this))
    , model_(new models::TagModel(this))
    , toolbar_(new QToolBar(this))
{
    setupUI();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void TagWidget::setTags(std::vector<git::TagInfo> tags) {
    model_->setTags(std::move(tags));
    tableView_->resizeColumnsToContents();
}

void TagWidget::clear() {
    model_->clear();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

QString TagWidget::selectedTagName() const {
    const auto indexes = tableView_->selectionModel()->selectedRows();
    if (indexes.isEmpty())
        return {};
    return model_->tagNameAtRow(indexes.first().row());
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

void TagWidget::onDeleteClicked() {
    const QString name = selectedTagName();
    if (!name.isEmpty())
        emit deleteTagRequested(name);
}

void TagWidget::onPushClicked() {
    const QString name = selectedTagName();
    if (!name.isEmpty())
        emit pushTagRequested(name);
}

void TagWidget::onContextMenu(const QPoint& pos) {
    const QModelIndex index = tableView_->indexAt(pos);
    if (!index.isValid())
        return;

    const QString name = model_->tagNameAtRow(index.row());
    if (name.isEmpty())
        return;

    QMenu menu(this);

    menu.addAction(tr("Delete Tag"), this, [this, name]() {
        emit deleteTagRequested(name);
    });

    menu.addAction(tr("Push to Remote"), this, [this, name]() {
        emit pushTagRequested(name);
    });

    menu.addAction(tr("Checkout (Detached HEAD)"), this, [this, name]() {
        emit checkoutTagRequested(name);
    });

    menu.addSeparator();

    menu.addAction(tr("Copy Tag Name"), this, [name]() {
        QApplication::clipboard()->setText(name);
    });

    menu.exec(tableView_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void TagWidget::setupUI() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    createAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("tag-new")),
        tr("Create Tag"));
    connect(createAction_, &QAction::triggered,
            this, &TagWidget::createTagRequested);

    deleteAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("edit-delete")),
        tr("Delete Tag"));
    connect(deleteAction_, &QAction::triggered,
            this, &TagWidget::onDeleteClicked);

    pushAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("go-up")),
        tr("Push Tag"));
    connect(pushAction_, &QAction::triggered,
            this, &TagWidget::onPushClicked);

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
    connect(tableView_, &QTableView::customContextMenuRequested,
            this, &TagWidget::onContextMenu);
}

} // namespace gitbolt::widgets
