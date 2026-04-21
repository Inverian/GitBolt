#include "widgets/RevisionGraphWidget.h"
#include "editor/RevisionGraphDelegate.h"

#include <QHeaderView>
#include <QTableView>
#include <QVBoxLayout>

namespace gitbolt::widgets {

RevisionGraphWidget::RevisionGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tableView_ = new QTableView(this);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->setShowGrid(false);
    tableView_->setAlternatingRowColors(true);
    tableView_->horizontalHeader()->setStretchLastSection(true);
    tableView_->setItemDelegateForColumn(0, new editor::RevisionGraphDelegate(tableView_));

    layout->addWidget(tableView_);
}

void RevisionGraphWidget::setModel(models::CommitLogModel* model)
{
    model_ = model;
    tableView_->setModel(model);

    if (!model)
        return;

    // Connect selection changes
    connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &RevisionGraphWidget::onSelectionChanged);

    // Connect model reset to resize graph column after data arrives
    connect(model, &QAbstractItemModel::modelReset,
            this, &RevisionGraphWidget::resizeGraphColumn);
    connect(model, &QAbstractItemModel::rowsInserted,
            this, &RevisionGraphWidget::resizeGraphColumn);
    connect(model, &QAbstractItemModel::modelReset,
            this, &RevisionGraphWidget::resizeMetaColumns);
    connect(model, &QAbstractItemModel::rowsInserted,
            this, &RevisionGraphWidget::resizeMetaColumns);

    // Configure column sizing. Author/Date/Hash are Interactive because
    // ResizeToContents jams text right up against the next column — we
    // compute "contents + padding" in resizeMetaColumns() so each column
    // has visible breathing room between it and its neighbor.
    auto* header = tableView_->horizontalHeader();
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Graph),
        QHeaderView::Fixed);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Message),
        QHeaderView::Stretch);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Author),
        QHeaderView::Interactive);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Date),
        QHeaderView::Interactive);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Hash),
        QHeaderView::Interactive);

    resizeGraphColumn();
    resizeMetaColumns();
}

void RevisionGraphWidget::resizeMetaColumns()
{
    if (!model_ || model_->rowCount() == 0)
        return;

    // Pixel padding added on top of the natural "fit contents" width for
    // Author/Date/Hash. Roughly two characters' worth of space so the
    // text doesn't butt up against the next column's content.
    constexpr int kColumnPadding = 24;

    const int metaColumns[] = {
        static_cast<int>(models::CommitLogColumn::Author),
        static_cast<int>(models::CommitLogColumn::Date),
        static_cast<int>(models::CommitLogColumn::Hash),
    };
    for (int col : metaColumns) {
        tableView_->resizeColumnToContents(col);
        tableView_->setColumnWidth(col, tableView_->columnWidth(col) + kColumnPadding);
    }
}

models::CommitLogModel* RevisionGraphWidget::model() const
{
    return model_;
}

void RevisionGraphWidget::onSelectionChanged()
{
    auto current = tableView_->selectionModel()->currentIndex();
    if (!current.isValid() || !model_)
        return;

    const auto* commit = model_->commitAt(current.row());
    if (commit) {
        emit commitSelected(QString::fromStdString(commit->id.toHex()));
    }
}

void RevisionGraphWidget::resizeGraphColumn()
{
    if (!model_ || model_->rowCount() == 0)
        return;

    // Scan visible rows to determine the widest graph column needed
    int firstVisible = tableView_->rowAt(0);
    int lastVisible = tableView_->rowAt(tableView_->viewport()->height());
    if (firstVisible < 0) firstVisible = 0;
    if (lastVisible < 0) lastVisible = model_->rowCount() - 1;
    lastVisible = std::min(lastVisible, model_->rowCount() - 1);

    int maxLane = 0;
    for (int row = firstVisible; row <= lastVisible; ++row) {
        const auto* g = model_->graphAt(row);
        if (g) maxLane = std::max(maxLane, g->maxLane);
    }

    int width = std::max(80, (maxLane + 2) * editor::RevisionGraphDelegate::LANE_WIDTH);
    tableView_->setColumnWidth(
        static_cast<int>(models::CommitLogColumn::Graph), width);
}

} // namespace gitbolt::widgets
