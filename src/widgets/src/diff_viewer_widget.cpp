#include "gitbolt/widgets/diff_viewer_widget.h"
#include <QSplitter>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QToolBar>
#include "gitbolt/core/diff.h"

namespace gitbolt::widgets {

DiffViewerWidget::DiffViewerWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    auto* toolbar = new QToolBar(this);
    toolbar->addAction("Unified");
    toolbar->addAction("Side-by-Side");
    layout->addWidget(toolbar);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(new QPlainTextEdit(this));
    splitter->addWidget(new QPlainTextEdit(this));
    layout->addWidget(splitter);
}
void DiffViewerWidget::setDiff(const gitbolt::core::DiffResult&, int) {}
void DiffViewerWidget::clear() {}


} // namespace gitbolt::widgets
