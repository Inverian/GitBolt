#include "gitbolt/widgets/blame_widget.h"
#include <QVBoxLayout>
#include <QTableView>

namespace gitbolt::widgets {

BlameWidget::BlameWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->addWidget(new QTableView(this));
}


} // namespace gitbolt::widgets
