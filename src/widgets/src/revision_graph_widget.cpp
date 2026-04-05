#include "gitbolt/widgets/revision_graph_widget.h"
#include <QVBoxLayout>
#include <QTableView>
#include <QHeaderView>
#include "gitbolt/widgets/revision_graph_delegate.h"

namespace gitbolt::widgets {

RevisionGraphWidget::RevisionGraphWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    auto* table = new QTableView(this);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setItemDelegateForColumn(0, new RevisionGraphDelegate(table));
    layout->addWidget(table);
}
void RevisionGraphWidget::setModel(models::CommitLogModel*) {}


} // namespace gitbolt::widgets
