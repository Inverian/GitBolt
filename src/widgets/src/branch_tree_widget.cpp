#include "gitbolt/widgets/branch_tree_widget.h"
#include <QVBoxLayout>
#include <QTreeView>

namespace gitbolt::widgets {

BranchTreeWidget::BranchTreeWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->addWidget(new QTreeView(this));
}


} // namespace gitbolt::widgets
