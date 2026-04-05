#include "widgets/SearchBarWidget.h"
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

namespace gitbolt::widgets {

SearchBarWidget::SearchBarWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    auto* combo = new QComboBox(this);
    combo->addItems({"Message", "Author", "Date", "File Content", "File Path"});
    layout->addWidget(combo);
    auto* input = new QLineEdit(this);
    input->setPlaceholderText(tr("Search..."));
    layout->addWidget(input);
    layout->addWidget(new QPushButton(tr("Search"), this));
}


} // namespace gitbolt::widgets
