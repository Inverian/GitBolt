#include "ui/RepositoryView.h"
#include "widgets/RevisionGraphWidget.h"
#include "widgets/DiffViewerWidget.h"
#include "widgets/CommitEditorWidget.h"
#include <QSplitter>
#include <QVBoxLayout>

namespace gitbolt::ui {

RepositoryView::RepositoryView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(new widgets::RevisionGraphWidget(this));
    auto* bottomSplitter = new QSplitter(Qt::Horizontal, this);
    bottomSplitter->addWidget(new widgets::DiffViewerWidget(this));
    bottomSplitter->addWidget(new widgets::CommitEditorWidget(this));
    splitter->addWidget(bottomSplitter);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter);
}

} // namespace gitbolt::ui
