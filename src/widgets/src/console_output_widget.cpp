#include "gitbolt/widgets/console_output_widget.h"
#include <QVBoxLayout>
#include <QPlainTextEdit>

namespace gitbolt::widgets {

ConsoleOutputWidget::ConsoleOutputWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    auto* output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    output->setFont(QFont("Menlo", 11));
    layout->addWidget(output);
}
void ConsoleOutputWidget::appendOutput(const QString&) {}
void ConsoleOutputWidget::appendError(const QString&) {}
void ConsoleOutputWidget::clear() {}


} // namespace gitbolt::widgets
