#include "widgets/RecordTablePanel.h"

#include <QHeaderView>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>

namespace gitbolt::widgets {

RecordTablePanel::RecordTablePanel(QWidget* parent)
    : QWidget(parent)
    , table_(new QTableView(this))
    , toolbar_(new QToolBar(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(toolbar_);
    layout->addWidget(table_, 1);
}

void RecordTablePanel::initPanel(QAbstractTableModel* model)
{
    table_->setModel(model);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setShowGrid(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);

    auto* hdr = table_->horizontalHeader();
    hdr->setStretchLastSection(true);
    hdr->setSectionResizeMode(QHeaderView::ResizeToContents);
}

int RecordTablePanel::selectedRow() const
{
    if (!table_->selectionModel())
        return -1;
    const auto indexes = table_->selectionModel()->selectedRows();
    return indexes.isEmpty() ? -1 : indexes.first().row();
}

} // namespace gitbolt::widgets
