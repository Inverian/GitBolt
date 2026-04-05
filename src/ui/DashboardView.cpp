#include "ui/DashboardView.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>

namespace gitbolt::ui {

DashboardView::DashboardView(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(tr("<h1>GitBolt</h1><p>Fast cross-platform Git GUI</p>"), this);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    layout->addSpacing(20);
    auto* btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    auto* openBtn = new QPushButton(tr("Open Repository"), this);
    auto* cloneBtn = new QPushButton(tr("Clone Repository"), this);
    auto* initBtn = new QPushButton(tr("Init New Repository"), this);
    btnLayout->addWidget(openBtn);
    btnLayout->addWidget(cloneBtn);
    btnLayout->addWidget(initBtn);
    btnLayout->addStretch();
    layout->addLayout(btnLayout);
    layout->addSpacing(20);
    layout->addWidget(new QLabel(tr("Recent Repositories:"), this));
    layout->addWidget(new QListWidget(this));
}

} // namespace gitbolt::ui
