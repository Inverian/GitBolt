#include "widgets/BranchTreeWidget.h"

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
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

    // Filter proxy wraps the tree model
    filterProxy_->setSourceModel(model_);
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

    // Expand local branches by default when model resets
    connect(model_, &QAbstractItemModel::modelReset,
            this, &BranchTreeWidget::expandLocalBranches);
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
    model_->clear();
}

void BranchTreeWidget::setCategoryVisible(int rootCategory, bool visible) {
    // Top-level categories live as direct children of the invisible
    // root model index. The proxy's row order matches the source
    // model since we don't reorder. Map the source row to its
    // proxy row and call setRowHidden on the tree view.
    if (!model_ || !filterProxy_ || !treeView_) return;
    if (rootCategory < 0 ||
        rootCategory >= static_cast<int>(
            models::BranchModel::RootCategory::Count))
        return;
    const QModelIndex srcIdx = model_->index(rootCategory, 0,
                                             QModelIndex());
    if (!srcIdx.isValid()) return;
    const QModelIndex proxyIdx = filterProxy_->mapFromSource(srcIdx);
    if (!proxyIdx.isValid()) return;
    treeView_->setRowHidden(proxyIdx.row(), QModelIndex(), !visible);
}

void BranchTreeWidget::setupToolbar() {
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* newBranchAction = toolbar_->addAction(
        QIcon::fromTheme(QStringLiteral("list-add")),
        tr("New Branch"));
    newBranchAction->setObjectName(QStringLiteral("branchTree.newBranch"));
    connect(newBranchAction, &QAction::triggered,
            this, &BranchTreeWidget::onNewBranchClicked);

    toolbar_->addSeparator();

    filterInput_->setObjectName(QStringLiteral("branchTree.filter"));
    filterInput_->setPlaceholderText(tr("Filter branches..."));
    filterInput_->setClearButtonEnabled(true);
    toolbar_->addWidget(filterInput_);
}

void BranchTreeWidget::onDoubleClicked(const QModelIndex& proxyIndex) {
    QModelIndex sourceIndex = filterProxy_->mapToSource(proxyIndex);
    if (model_->isCategoryIndex(sourceIndex))
        return;

    QString name = model_->branchNameAt(sourceIndex);
    if (!name.isEmpty()) {
        emit checkoutRequested(name);
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

    // Determine which category this item belongs to
    QModelIndex parentIdx = index.parent();
    bool isLocal = parentIdx.isValid()
        && parentIdx.row() == static_cast<int>(models::BranchModel::RootCategory::LocalBranches);
    bool isRemote = parentIdx.isValid()
        && parentIdx.row() == static_cast<int>(models::BranchModel::RootCategory::RemoteBranches);
    bool isTag = parentIdx.isValid()
        && parentIdx.row() == static_cast<int>(models::BranchModel::RootCategory::Tags);

    QMenu menu(this);

    // Checkout
    auto* checkoutAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-jump")),
                                          tr("Checkout '%1'").arg(branchName));
    connect(checkoutAction, &QAction::triggered, this, [this, branchName]() {
        emit checkoutRequested(branchName);
    });

    menu.addSeparator();

    if (isLocal) {
        // New branch from this one
        auto* newAction = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                         tr("New Branch from '%1'...").arg(branchName));
        connect(newAction, &QAction::triggered, this, [this, branchName]() {
            bool ok = false;
            QString name = QInputDialog::getText(this, tr("New Branch"),
                                                  tr("Branch name:"),
                                                  QLineEdit::Normal,
                                                  QString(), &ok);
            if (ok && !name.trimmed().isEmpty()) {
                emit createBranchRequested(name.trimmed());
            }
        });

        // Rename
        auto* renameAction = menu.addAction(tr("Rename '%1'...").arg(branchName));
        connect(renameAction, &QAction::triggered, this, [this, branchName]() {
            bool ok = false;
            QString newName = QInputDialog::getText(this, tr("Rename Branch"),
                                                     tr("New name for '%1':").arg(branchName),
                                                     QLineEdit::Normal,
                                                     branchName, &ok);
            if (ok && !newName.trimmed().isEmpty() && newName.trimmed() != branchName) {
                emit renameBranchRequested(branchName, newName.trimmed());
            }
        });

        // Merge into current
        auto* mergeAction = menu.addAction(tr("Merge '%1' into current").arg(branchName));
        connect(mergeAction, &QAction::triggered, this, [this, branchName]() {
            emit mergeRequested(branchName);
        });

        menu.addSeparator();

        // Push
        auto* pushAction = menu.addAction(QIcon::fromTheme(QStringLiteral("go-up")),
                                          tr("Push '%1'").arg(branchName));
        connect(pushAction, &QAction::triggered, this, [this, branchName]() {
            emit pushRequested(QStringLiteral("origin"), branchName);
        });

        // Set upstream
        auto* upstreamAction = menu.addAction(tr("Set Upstream..."));
        connect(upstreamAction, &QAction::triggered, this, [this, branchName]() {
            bool ok = false;
            QString upstream = QInputDialog::getText(
                this, tr("Set Upstream"),
                tr("Upstream for '%1' (e.g., origin/%1):").arg(branchName),
                QLineEdit::Normal,
                QStringLiteral("origin/") + branchName, &ok);
            if (ok && !upstream.trimmed().isEmpty()) {
                emit setUpstreamRequested(branchName, upstream.trimmed());
            }
        });

        menu.addSeparator();

        // Delete
        auto* deleteAction = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                            tr("Delete '%1'").arg(branchName));
        connect(deleteAction, &QAction::triggered, this, [this, branchName]() {
            auto answer = QMessageBox::question(
                this, tr("Delete Branch"),
                tr("Are you sure you want to delete branch '%1'?").arg(branchName),
                QMessageBox::Yes | QMessageBox::No);
            if (answer == QMessageBox::Yes)
                emit deleteBranchRequested(branchName);
        });
    }

    if (isRemote) {
        // Checkout as local tracking branch
        auto* trackAction = menu.addAction(tr("Checkout as local branch"));
        connect(trackAction, &QAction::triggered, this, [this, branchName]() {
            // Remote branch names are like "origin/feature" - extract just the branch part
            QString localName = branchName;
            int slashPos = localName.indexOf(QLatin1Char('/'));
            if (slashPos >= 0)
                localName = localName.mid(slashPos + 1);
            emit checkoutRequested(localName);
        });
    }

    if (isTag) {
        auto* checkoutTag = menu.addAction(tr("Checkout tag '%1'").arg(branchName));
        connect(checkoutTag, &QAction::triggered, this, [this, branchName]() {
            emit checkoutRequested(branchName);
        });
    }

    menu.exec(globalPos);
}

void BranchTreeWidget::onFilterChanged(const QString& text) {
    filterProxy_->setFilterFixedString(text);

    // When filtering, expand everything so matches are visible
    if (!text.isEmpty()) {
        treeView_->expandAll();
    } else {
        treeView_->collapseAll();
        expandLocalBranches();
    }
}

void BranchTreeWidget::onNewBranchClicked() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Branch"),
                                          tr("Branch name:"),
                                          QLineEdit::Normal,
                                          QString(), &ok);
    if (ok && !name.trimmed().isEmpty()) {
        emit createBranchRequested(name.trimmed());
    }
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
