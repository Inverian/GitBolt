#include "widgets/BranchTreeWidget.h"

#include <QAction>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QVBoxLayout>

namespace gitbolt::widgets {

BranchTreeWidget::BranchTreeWidget(QWidget* parent)
    : QWidget(parent)
    , treeView_(new QTreeView(this))
    , model_(new models::BranchModel(this))
    , filterProxy_(new QSortFilterProxyModel(this))
    , toolbar_(new QToolBar(this))
    , filterInput_(new QLineEdit(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    setupToolbar();
    layout->addWidget(toolbar_);

    // A reload resets the model, which collapses the tree and drops the
    // selection: kept across it. Slots run in the order they were
    // connected, so the state is saved before the proxy hands the reset
    // to the view (which then knows no rows) and put back after.
    connect(model_, &QAbstractItemModel::modelAboutToBeReset,
            this, &BranchTreeWidget::saveViewState);
    filterProxy_->setSourceModel(model_);
    connect(model_, &QAbstractItemModel::modelReset,
            this, &BranchTreeWidget::restoreViewState);
    filterProxy_->setRecursiveFilteringEnabled(true);
    filterProxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    treeView_->setModel(filterProxy_);
    treeView_->setObjectName(QStringLiteral("branchTree.view"));
    treeView_->setHeaderHidden(true);
    treeView_->setExpandsOnDoubleClick(false);
    treeView_->setContextMenuPolicy(Qt::CustomContextMenu);
    treeView_->setSelectionMode(QAbstractItemView::SingleSelection);
    treeView_->setAnimated(true);
    treeView_->setIndentation(16);

    layout->addWidget(treeView_);

    // Use `activated` rather than `doubleClicked` so both double-click
    // AND Enter trigger checkout. Activation also respects the user's
    // platform click preference and plays well with synthetic events
    // from tools/click.py during automated testing.
    connect(treeView_, &QAbstractItemView::activated,
            this, &BranchTreeWidget::onDoubleClicked);
    connect(treeView_, &QTreeView::customContextMenuRequested,
            this, &BranchTreeWidget::onCustomContextMenu);
    connect(filterInput_, &QLineEdit::textChanged,
            this, &BranchTreeWidget::onFilterChanged);

    expandLocalBranches();
}

void BranchTreeWidget::setBranches(std::vector<git::BranchInfo> branches) {
    model_->setBranches(std::move(branches));
}

void BranchTreeWidget::setTags(std::vector<git::TagInfo> tags) {
    model_->setTags(std::move(tags));
}

void BranchTreeWidget::setSubmodules(std::vector<git::SubmoduleInfo> submodules) {
    model_->setSubmodules(std::move(submodules));
}

void BranchTreeWidget::setStashes(std::vector<git::StashEntry> stashes) {
    model_->setStashes(std::move(stashes));
}

void BranchTreeWidget::clear() {
    if (filterInput_)
        filterInput_->clear();
    keepView_ = false;
    model_->clear();
    keepView_ = true;
}

void BranchTreeWidget::setCategoryVisible(int rootCategory, bool visible) {
    if (rootCategory < 0 ||
        rootCategory >= static_cast<int>(
            models::BranchModel::RootCategory::Count))
        return;
    if (visible)
        hiddenCategories_.remove(rootCategory);
    else
        hiddenCategories_.insert(rootCategory);
    applyHiddenCategories();
}

void BranchTreeWidget::applyHiddenCategories() {
    // The view forgets hidden rows on a reset, and a category the filter
    // took out and put back comes back shown: so set again each time.
    for (int cat = 0; cat < static_cast<int>(models::BranchModel::RootCategory::Count); ++cat) {
        const QModelIndex proxyIdx = filterProxy_->mapFromSource(model_->index(cat, 0));
        if (proxyIdx.isValid())
            treeView_->setRowHidden(proxyIdx.row(), QModelIndex(), hiddenCategories_.contains(cat));
    }
}

void BranchTreeWidget::saveViewState() {
    savedView_.reset();
    if (!keepView_)
        return;
    const auto key = [](const QModelIndex& index) {
        return index.data(models::BranchModel::ItemKeyRole).toString();
    };
    ViewState state;
    collectExpanded(QModelIndex(), state.expanded);
    for (int row = 0, rows = filterProxy_->rowCount(); row < rows; ++row)
        state.categories << key(filterProxy_->index(row, 0));
    state.current = key(treeView_->currentIndex());
    if (const QModelIndexList selected = treeView_->selectionModel()->selectedIndexes();
        !selected.isEmpty())
        state.selected = key(selected.first());
    if (const QModelIndex top = treeView_->indexAt(QPoint(0, 0)); top.isValid()) {
        state.top = key(top);
        state.topHidden = -treeView_->visualRect(top).top();
    }
    state.vScroll = treeView_->verticalScrollBar()->value();
    state.hScroll = treeView_->horizontalScrollBar()->value();
    savedView_ = std::move(state);
}

void BranchTreeWidget::restoreViewState() {
    applyHiddenCategories();
    if (!savedView_) {
        expandLocalBranches();
        return;
    }
    const ViewState state = std::move(*savedView_);
    savedView_.reset();

    for (const QString& key : state.expanded) {
        if (const QModelIndex index = viewIndexForKey(key); index.isValid())
            treeView_->expand(index);
    }
    // A category the filter lets in with this reload (its first match)
    // opens, as the filter opened the others: closed, it hid the match.
    for (int row = 0, rows = filterProxy_->rowCount(); row < rows; ++row) {
        const QModelIndex index = filterProxy_->index(row, 0);
        if (!state.categories.contains(index.data(models::BranchModel::ItemKeyRole).toString()))
            treeView_->expand(index);
    }
    // Without auto-scroll: making a row current scrolls to it, and
    // expands a collapsed category to show it.
    const bool autoScroll = treeView_->hasAutoScroll();
    treeView_->setAutoScroll(false);
    auto* selection = treeView_->selectionModel();
    if (const QModelIndex current = viewIndexForKey(state.current); current.isValid())
        selection->setCurrentIndex(current, QItemSelectionModel::NoUpdate);
    if (const QModelIndex selected = viewIndexForKey(state.selected); selected.isValid())
        selection->select(selected, QItemSelectionModel::ClearAndSelect);
    treeView_->setAutoScroll(autoScroll);

    // The row that was at the top stays there, rows added above it or
    // not; the old offset where it is gone.
    treeView_->doItemsLayout();
    QScrollBar* vBar = treeView_->verticalScrollBar();
    if (const QModelIndex top = viewIndexForKey(state.top); top.isValid()) {
        treeView_->scrollTo(top, QAbstractItemView::PositionAtTop);
        if (treeView_->verticalScrollMode() == QAbstractItemView::ScrollPerPixel)
            vBar->setValue(vBar->value() + state.topHidden);
    } else {
        vBar->setValue(state.vScroll);
    }
    treeView_->horizontalScrollBar()->setValue(state.hScroll);
}

void BranchTreeWidget::collectExpanded(const QModelIndex& parent, QStringList& keys) const {
    for (int row = 0, rows = filterProxy_->rowCount(parent); row < rows; ++row) {
        const QModelIndex index = filterProxy_->index(row, 0, parent);
        if (!treeView_->isExpanded(index))
            continue;
        keys << index.data(models::BranchModel::ItemKeyRole).toString();
        collectExpanded(index, keys);
    }
}

QModelIndex BranchTreeWidget::viewIndexForKey(const QString& key) const {
    return filterProxy_->mapFromSource(model_->indexForKey(key));
}

void BranchTreeWidget::setupToolbar() {
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    newBranchAction_ = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("list-add")),
        tr("New Branch"));
    newBranchAction_->setObjectName(QStringLiteral("branchTree.newBranch"));
    newBranchAction_->setEnabled(repoActionsEnabled_);
    connect(newBranchAction_, &QAction::triggered,
            this, &BranchTreeWidget::onNewBranchClicked);

    toolbar_->addSeparator();

    filterInput_->setObjectName(QStringLiteral("branchTree.filter"));
    filterInput_->setPlaceholderText(tr("Filter branches..."));
    filterInput_->setClearButtonEnabled(true);
    toolbar_->addWidget(filterInput_);
}

void BranchTreeWidget::setRepoActionsEnabled(bool on) {
    repoActionsEnabled_ = on;
    if (newBranchAction_)
        newBranchAction_->setEnabled(on);
}

void BranchTreeWidget::setRepositoryKeySource(std::function<std::string()> source) {
    keySource_ = std::move(source);
}

std::string BranchTreeWidget::repositoryKey() const {
    return keySource_ ? keySource_() : std::string();
}

void BranchTreeWidget::onDoubleClicked(const QModelIndex& proxyIndex) {
    if (!repoActionsEnabled_)
        return;
    QModelIndex sourceIndex = filterProxy_->mapToSource(proxyIndex);
    if (model_->isCategoryIndex(sourceIndex))
        return;

    QString ref = model_->checkoutRefAt(sourceIndex);
    if (!ref.isEmpty()) {
        emit checkoutRequested(ref, repositoryKey());
    }
}

void BranchTreeWidget::onCustomContextMenu(const QPoint& pos) {
    QModelIndex proxyIndex = treeView_->indexAt(pos);
    if (!proxyIndex.isValid())
        return;

    QModelIndex sourceIndex = filterProxy_->mapToSource(proxyIndex);
    if (model_->isCategoryIndex(sourceIndex))
        return;

    QPoint globalPos = treeView_->viewport()->mapToGlobal(pos);
    setupContextMenu(sourceIndex, globalPos);
}

void BranchTreeWidget::setupContextMenu(const QModelIndex& index, const QPoint& globalPos) {
    QString branchName = model_->branchNameAt(index);
    if (branchName.isEmpty())
        return;
    // Labels show the short name; checkout gets the unambiguous ref.
    const QString checkoutRef = model_->checkoutRefAt(index);

    // Determine which category this item belongs to
    QModelIndex parentIdx = index.parent();
    bool isLocal = parentIdx.isValid()
        && parentIdx.row() == static_cast<int>(models::BranchModel::RootCategory::LocalBranches);
    bool isRemote = parentIdx.isValid()
        && parentIdx.row() == static_cast<int>(models::BranchModel::RootCategory::RemoteBranches);
    bool isTag = parentIdx.isValid()
        && parentIdx.row() == static_cast<int>(models::BranchModel::RootCategory::Tags);

    QMenu menu(this);
    // The repository the menu is for: its entries are picked once its
    // event loop is done, by when a second launch may have handed
    // another over (setRepositoryKeySource()).
    const std::string key = repositoryKey();

    // Checkout
    auto* checkoutAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-jump")),
                                          tr("Checkout '%1'").arg(branchName));
    checkoutAction->setObjectName(QStringLiteral("branches.checkout"));
    connect(checkoutAction, &QAction::triggered, this, [this, checkoutRef, key]() {
        emit checkoutRequested(checkoutRef, key);
    });

    menu.addSeparator();

    if (isLocal) {
        // New branch from this one
        auto* newAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                         tr("New Branch from '%1'...").arg(branchName));
        newAction->setObjectName(QStringLiteral("branches.new-branch"));
        // The name, and Rename's and Set Upstream's below, are asked
        // for by MainWindow, as Delete's confirmation is: it knows the
        // repository the question is about, and whether that is still
        // the one open once the answer comes (a second launch can hand
        // another over meanwhile). From this branch: it started at
        // HEAD, whatever the entry said.
        connect(newAction, &QAction::triggered, this, [this, branchName, key]() {
            emit createBranchRequested(branchName, key);
        });

        // Rename
        auto* renameAction = menu.addAction(tr("Rename '%1'...").arg(branchName));
        renameAction->setObjectName(QStringLiteral("branches.rename"));
        connect(renameAction, &QAction::triggered, this, [this, branchName, key]() {
            emit renameBranchRequested(branchName, key);
        });

        // Merge into current
        auto* mergeAction = menu.addAction(tr("Merge '%1' into current").arg(branchName));
        mergeAction->setObjectName(QStringLiteral("branches.merge"));
        connect(mergeAction, &QAction::triggered, this, [this, branchName, key]() {
            emit mergeRequested(branchName, key);
        });

        menu.addSeparator();

        // Push
        auto* pushAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-up")),
                                          tr("Push '%1'").arg(branchName));
        pushAction->setObjectName(QStringLiteral("branches.push"));
        connect(pushAction, &QAction::triggered, this, [this, branchName, key]() {
            emit pushRequested(QStringLiteral("origin"), branchName, key);
        });

        // Set upstream
        auto* upstreamAction = menu.addAction(tr("Set Upstream..."));
        upstreamAction->setObjectName(QStringLiteral("branches.set-upstream"));
        connect(upstreamAction, &QAction::triggered, this, [this, branchName, key]() {
            emit setUpstreamRequested(branchName, key);
        });

        menu.addSeparator();

        // Delete
        auto* deleteAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                            tr("Delete '%1'").arg(branchName));
        deleteAction->setObjectName(QStringLiteral("branches.delete"));
        // MainWindow asks, as for the remote Delete below: it knows the
        // repository the question is about, and that it's still open
        // once the answer comes.
        connect(deleteAction, &QAction::triggered, this, [this, branchName, key]() {
            emit deleteBranchRequested(branchName, key);
        });
    }

    if (isRemote) {
        // Checkout as local tracking branch
        auto* trackAction = menu.addAction(tr("Checkout as local branch"));
        trackAction->setObjectName(QStringLiteral("branches.checkout-as-local"));
        connect(trackAction, &QAction::triggered, this, [this, branchName, key]() {
            emit checkoutRemoteRequested(branchName, key);
        });

        menu.addSeparator();

        // Delete on the server. MainWindow owns the confirmation, so
        // this entry and Commands > Delete remote branch ask the same.
        auto* deleteRemoteAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("edit-delete")),
            tr("Delete '%1' from remote...").arg(branchName));
        deleteRemoteAction->setObjectName(QStringLiteral("branches.delete-from-remote"));
        connect(deleteRemoteAction, &QAction::triggered, this, [this, branchName, key]() {
            emit deleteRemoteBranchRequested(branchName, key);
        });
    }

    if (isTag) {
        auto* checkoutTag = menu.addAction(tr("Checkout tag '%1'").arg(branchName));
        checkoutTag->setObjectName(QStringLiteral("branches.checkout-tag"));
        connect(checkoutTag, &QAction::triggered, this, [this, checkoutRef, key]() {
            emit checkoutRequested(checkoutRef, key);
        });
    }

    // Every entry runs git on the repository: shown, so the menu still
    // says what there is, but off while the window's actions are.
    if (!repoActionsEnabled_) {
        for (QAction* action : menu.actions())
            action->setEnabled(false);
    }

    menu.exec(globalPos);
}

void BranchTreeWidget::onFilterChanged(const QString& text) {
    filterProxy_->setFilterFixedString(text);
    applyHiddenCategories();

    // When filtering, expand everything so matches are visible
    if (!text.isEmpty()) {
        treeView_->expandAll();
    } else {
        treeView_->collapseAll();
        expandLocalBranches();
    }
}

void BranchTreeWidget::onNewBranchClicked() {
    if (!repoActionsEnabled_)
        return;
    emit createBranchRequested(QString(), repositoryKey());
}

void BranchTreeWidget::expandLocalBranches() {
    // Expand "Local Branches" root node by default
    QModelIndex localRoot = model_->index(
        static_cast<int>(models::BranchModel::RootCategory::LocalBranches), 0);
    QModelIndex proxyLocalRoot = filterProxy_->mapFromSource(localRoot);
    if (proxyLocalRoot.isValid())
        treeView_->expand(proxyLocalRoot);
}

} // namespace gitbolt::widgets
