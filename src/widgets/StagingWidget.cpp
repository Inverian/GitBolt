#include "widgets/StagingWidget.h"
#include "models/FileStatusModel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StagingWidget::StagingWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void StagingWidget::setEntries(std::vector<gitbolt::git::StatusEntry> entries)
{
    // Both models share the same full list; the staged filter selects the view
    unstagedModel_->setEntries(entries);
    stagedModel_->setEntries(std::move(entries));

    // Update count labels
    const int uc = unstagedModel_->rowCount();
    const int sc = stagedModel_->rowCount();
    unstagedLabel_->setText(tr("Unstaged Changes (%1)").arg(uc));
    stagedLabel_->setText(tr("Staged Changes (%1)").arg(sc));
}

void StagingWidget::clear()
{
    unstagedModel_->clear();
    stagedModel_->clear();
    unstagedLabel_->setText(tr("Unstaged Changes"));
    stagedLabel_->setText(tr("Staged Changes"));
}

// ---------------------------------------------------------------------------
// Slots -- double-click to stage / unstage
// ---------------------------------------------------------------------------

void StagingWidget::onUnstagedDoubleClicked(const QModelIndex& index)
{
    const QString path = unstagedModel_->pathAt(index.row());
    if (!path.isEmpty())
        emit stageRequested(path);
}

void StagingWidget::onStagedDoubleClicked(const QModelIndex& index)
{
    const QString path = stagedModel_->pathAt(index.row());
    if (!path.isEmpty())
        emit unstageRequested(path);
}

// ---------------------------------------------------------------------------
// Slots -- selection changed -> emit fileSelected
// ---------------------------------------------------------------------------

void StagingWidget::onUnstagedSelectionChanged()
{
    const auto indexes = unstagedView_->selectionModel()->selectedIndexes();
    if (indexes.isEmpty())
        return;
    const QString path = unstagedModel_->pathAt(indexes.first().row());
    if (!path.isEmpty())
        emit fileSelected(path);
}

void StagingWidget::onStagedSelectionChanged()
{
    const auto indexes = stagedView_->selectionModel()->selectedIndexes();
    if (indexes.isEmpty())
        return;
    const QString path = stagedModel_->pathAt(indexes.first().row());
    if (!path.isEmpty())
        emit fileSelected(path);
}

// ---------------------------------------------------------------------------
// Slots -- context menus
// ---------------------------------------------------------------------------

void StagingWidget::showUnstagedContextMenu(const QPoint& pos)
{
    const QModelIndex idx = unstagedView_->indexAt(pos);
    if (!idx.isValid())
        return;

    const QString path = unstagedModel_->pathAt(idx.row());
    if (path.isEmpty())
        return;

    QMenu menu(this);
    menu.addAction(tr("Stage"), this, [this, path]() {
        emit stageRequested(path);
    });
    menu.addAction(tr("Discard Changes"), this, [this, path]() {
        emit discardRequested(path);
    });
    menu.addAction(tr("Open in Editor"), this, [this, path]() {
        emit openInEditorRequested(path);
    });
    menu.exec(unstagedView_->viewport()->mapToGlobal(pos));
}

void StagingWidget::showStagedContextMenu(const QPoint& pos)
{
    const QModelIndex idx = stagedView_->indexAt(pos);
    if (!idx.isValid())
        return;

    const QString path = stagedModel_->pathAt(idx.row());
    if (path.isEmpty())
        return;

    QMenu menu(this);
    menu.addAction(tr("Unstage"), this, [this, path]() {
        emit unstageRequested(path);
    });
    menu.addAction(tr("Open in Editor"), this, [this, path]() {
        emit openInEditorRequested(path);
    });
    menu.exec(stagedView_->viewport()->mapToGlobal(pos));
}

// ---------------------------------------------------------------------------
// UI setup
// ---------------------------------------------------------------------------

void StagingWidget::setupUi()
{
    // ---- Models -----------------------------------------------------------
    unstagedModel_ = new models::FileStatusModel(this);
    unstagedModel_->setStagedFilter(false);

    stagedModel_ = new models::FileStatusModel(this);
    stagedModel_->setStagedFilter(true);

    // ---- Unstaged panel ---------------------------------------------------
    auto* unstagedWidget = new QWidget(this);
    auto* unstagedLayout = new QVBoxLayout(unstagedWidget);
    unstagedLayout->setContentsMargins(0, 0, 0, 0);

    unstagedLabel_ = new QLabel(tr("Unstaged Changes"), unstagedWidget);
    QFont labelFont = unstagedLabel_->font();
    labelFont.setBold(true);
    unstagedLabel_->setFont(labelFont);
    unstagedLayout->addWidget(unstagedLabel_);

    unstagedView_ = new QListView(unstagedWidget);
    unstagedView_->setModel(unstagedModel_);
    unstagedView_->setModelColumn(models::FileStatusModel::Path);
    unstagedView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    unstagedView_->setContextMenuPolicy(Qt::CustomContextMenu);
    unstagedLayout->addWidget(unstagedView_);

    connect(unstagedView_, &QListView::doubleClicked,
            this, &StagingWidget::onUnstagedDoubleClicked);
    connect(unstagedView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &StagingWidget::onUnstagedSelectionChanged);
    connect(unstagedView_, &QWidget::customContextMenuRequested,
            this, &StagingWidget::showUnstagedContextMenu);

    // ---- Staged panel -----------------------------------------------------
    auto* stagedWidget = new QWidget(this);
    auto* stagedLayout = new QVBoxLayout(stagedWidget);
    stagedLayout->setContentsMargins(0, 0, 0, 0);

    stagedLabel_ = new QLabel(tr("Staged Changes"), stagedWidget);
    stagedLabel_->setFont(labelFont);
    stagedLayout->addWidget(stagedLabel_);

    stagedView_ = new QListView(stagedWidget);
    stagedView_->setModel(stagedModel_);
    stagedView_->setModelColumn(models::FileStatusModel::Path);
    stagedView_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    stagedView_->setContextMenuPolicy(Qt::CustomContextMenu);
    stagedLayout->addWidget(stagedView_);

    connect(stagedView_, &QListView::doubleClicked,
            this, &StagingWidget::onStagedDoubleClicked);
    connect(stagedView_->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &StagingWidget::onStagedSelectionChanged);
    connect(stagedView_, &QWidget::customContextMenuRequested,
            this, &StagingWidget::showStagedContextMenu);

    // ---- Splitter ---------------------------------------------------------
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(unstagedWidget);
    splitter->addWidget(stagedWidget);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    // ---- Buttons ----------------------------------------------------------
    auto* btnLayout = new QHBoxLayout;
    stageAllBtn_ = new QPushButton(tr("Stage All"), this);
    unstageAllBtn_ = new QPushButton(tr("Unstage All"), this);
    btnLayout->addWidget(stageAllBtn_);
    btnLayout->addWidget(unstageAllBtn_);

    connect(stageAllBtn_, &QPushButton::clicked, this, &StagingWidget::stageAllRequested);
    connect(unstageAllBtn_, &QPushButton::clicked, this, &StagingWidget::unstageAllRequested);

    // ---- Root layout ------------------------------------------------------
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->addWidget(splitter, 1);
    rootLayout->addLayout(btnLayout);
}

} // namespace gitbolt::widgets
