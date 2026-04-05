#include "gitbolt/widgets/merge_conflict_widget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>

namespace gitbolt::widgets {

MergeConflictWidget::MergeConflictWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* ours = new QPlainTextEdit(this);
    ours->setReadOnly(true);
    auto* base = new QPlainTextEdit(this);
    base->setReadOnly(true);
    auto* theirs = new QPlainTextEdit(this);
    theirs->setReadOnly(true);
    splitter->addWidget(ours);
    splitter->addWidget(base);
    splitter->addWidget(theirs);
    layout->addWidget(splitter);
    auto* btnLayout = new QHBoxLayout;
    btnLayout->addWidget(new QPushButton(tr("Accept Ours"), this));
    btnLayout->addWidget(new QPushButton(tr("Accept Theirs"), this));
    btnLayout->addWidget(new QPushButton(tr("Accept Both"), this));
    layout->addLayout(btnLayout);
}


} // namespace gitbolt::widgets
