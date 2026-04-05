#include "gitbolt/widgets/staging_widget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListView>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>

namespace gitbolt::widgets {

StagingWidget::StagingWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    auto* unstaged = new QWidget(this);
    auto* ul = new QVBoxLayout(unstaged);
    ul->addWidget(new QLabel(tr("Unstaged Changes"), unstaged));
    ul->addWidget(new QListView(unstaged));
    splitter->addWidget(unstaged);
    auto* staged = new QWidget(this);
    auto* sl = new QVBoxLayout(staged);
    sl->addWidget(new QLabel(tr("Staged Changes"), staged));
    sl->addWidget(new QListView(staged));
    splitter->addWidget(staged);
    layout->addWidget(splitter);
    auto* btnLayout = new QHBoxLayout;
    btnLayout->addWidget(new QPushButton(tr("Stage All"), this));
    btnLayout->addWidget(new QPushButton(tr("Unstage All"), this));
    layout->addLayout(btnLayout);
}


} // namespace gitbolt::widgets
