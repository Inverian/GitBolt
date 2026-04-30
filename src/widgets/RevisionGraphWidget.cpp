#include "widgets/RevisionGraphWidget.h"
#include "editor/RevisionGraphDelegate.h"
#include "widgets/CommitFilterProxy.h"

#include <QEvent>
#include <QHeaderView>
#include <QMouseEvent>
#include <QTableView>
#include <QVBoxLayout>

namespace gitbolt::widgets {

RevisionGraphWidget::RevisionGraphWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    tableView_ = new QTableView(this);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->setShowGrid(false);
    tableView_->setAlternatingRowColors(true);
    // Don't stretch the last column. Hash is the last column in the
    // model enum, so stretchLastSection(true) would force Hash to
    // expand into all remaining horizontal space (the screenshot
    // showed hashes floating in 600+ pixels of whitespace). We want
    // Message to be the stretchy column instead, which is configured
    // explicitly in setModel() via setSectionResizeMode(Message,
    // Stretch). Leaving stretchLastSection at its Qt default (false)
    // lets that Message-Stretch take effect and keeps Hash sized to
    // its content via the meta-column resizer.
    tableView_->horizontalHeader()->setStretchLastSection(false);
    // Don't bold the header section that corresponds to the current
    // selection. Qt enables this by default for QTableView, and the
    // bolding is jumpy and confusing here — selecting a commit row
    // shouldn't restyle the column titles above it.
    tableView_->horizontalHeader()->setHighlightSections(false);
    tableView_->setItemDelegateForColumn(0, new editor::RevisionGraphDelegate(tableView_));

    // Filter proxy sits between the source CommitLogModel and the
    // view. Defaults to a no-op (no criteria == match everything).
    // CommitFilterProxy supports multi-criterion filtering across
    // message / author / SHA / date range; the toolbar Filter
    // input drives just the message field via setMessageFilter.
    proxy_ = new CommitFilterProxy(this);

    // Watch viewport mouse events so we can implement "modifier-click
    // the selected row to deselect it". QAbstractItemView in
    // SingleSelection mode normally won't let you reach the
    // zero-rows-selected state by clicking — the table always keeps
    // exactly one row chosen once anything is picked. That's
    // surprising when the user wants to back out and see no
    // commit-detail panel; eventFilter() below handles the toggle.
    tableView_->viewport()->installEventFilter(this);

    layout->addWidget(tableView_);
}

void RevisionGraphWidget::setModel(models::CommitLogModel* model)
{
    model_ = model;
    proxy_->setSourceModel(model);
    tableView_->setModel(proxy_);

    if (!model)
        return;

    // Connect selection changes
    connect(tableView_->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, &RevisionGraphWidget::onSelectionChanged);

    // Connect model reset to resize graph column after data arrives
    connect(model, &QAbstractItemModel::modelReset,
            this, &RevisionGraphWidget::resizeGraphColumn);
    connect(model, &QAbstractItemModel::rowsInserted,
            this, &RevisionGraphWidget::resizeGraphColumn);
    connect(model, &QAbstractItemModel::modelReset,
            this, &RevisionGraphWidget::resizeMetaColumns);
    connect(model, &QAbstractItemModel::rowsInserted,
            this, &RevisionGraphWidget::resizeMetaColumns);

    // Configure column sizing. Author/Date/Hash are Interactive because
    // ResizeToContents jams text right up against the next column — we
    // compute "contents + padding" in resizeMetaColumns() so each column
    // has visible breathing room between it and its neighbor.
    auto* header = tableView_->horizontalHeader();
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Graph),
        QHeaderView::Fixed);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Message),
        QHeaderView::Stretch);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Author),
        QHeaderView::Interactive);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Date),
        QHeaderView::Interactive);
    header->setSectionResizeMode(
        static_cast<int>(models::CommitLogColumn::Hash),
        QHeaderView::Interactive);

    resizeGraphColumn();
    resizeMetaColumns();
}

void RevisionGraphWidget::resizeMetaColumns()
{
    if (!model_ || model_->rowCount() == 0)
        return;

    // Pixel padding added on top of the natural "fit contents" width for
    // Author/Date/Hash. Roughly two characters' worth of space for Date
    // and Hash so the text doesn't butt up against the next column's
    // content. Author gets a larger pad — the column was reading too
    // tight against the SHA next to it, and giving Author extra room
    // pulls width away from Message (which is Stretch and absorbs the
    // leftover horizontal space). Net effect: a more readable Author
    // column at the cost of a slightly narrower Message column.
    constexpr int kColumnPadding       = 24;
    constexpr int kAuthorColumnPadding = 80;

    struct MetaCol { int column; int padding; };
    const MetaCol metaColumns[] = {
        { static_cast<int>(models::CommitLogColumn::Author),
          kAuthorColumnPadding },
        { static_cast<int>(models::CommitLogColumn::Date),
          kColumnPadding },
        { static_cast<int>(models::CommitLogColumn::Hash),
          kColumnPadding },
    };
    for (const auto& meta : metaColumns) {
        tableView_->resizeColumnToContents(meta.column);
        tableView_->setColumnWidth(
            meta.column,
            tableView_->columnWidth(meta.column) + meta.padding);
    }
}

models::CommitLogModel* RevisionGraphWidget::model() const
{
    return model_;
}

int RevisionGraphWidget::currentSourceRow() const
{
    if (!tableView_->selectionModel()) return -1;
    const QModelIndex proxyIdx =
        tableView_->selectionModel()->currentIndex();
    if (!proxyIdx.isValid()) return -1;
    const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
    return srcIdx.isValid() ? srcIdx.row() : -1;
}

bool RevisionGraphWidget::selectCommit(const QString& commitHash)
{
    if (!model_ || commitHash.isEmpty())
        return false;
    const std::string needle = commitHash.toLower().toStdString();
    const int rowCount = model_->rowCount();
    for (int row = 0; row < rowCount; ++row) {
        const auto* c = model_->commitAt(row);
        if (c && c->id.toHex() == needle) {
            // Map the source row to the proxy. If the proxy is
            // currently filtering this row out (target hidden by
            // active filter), mapFromSource returns invalid; clear
            // the filter for the seek so the user actually sees
            // the target — better UX than silently failing.
            QModelIndex srcIdx = model_->index(row, 0);
            QModelIndex proxyIdx = proxy_->mapFromSource(srcIdx);
            if (!proxyIdx.isValid()) {
                // Reset every criterion. mapFromSource returns
                // invalid when the row is filtered out; clearing
                // criteria unconditionally ensures the seek lands.
                proxy_->setCriteria({});
                proxyIdx = proxy_->mapFromSource(srcIdx);
            }
            if (!proxyIdx.isValid()) return false;
            tableView_->setCurrentIndex(proxyIdx);
            tableView_->scrollTo(proxyIdx,
                QAbstractItemView::PositionAtCenter);
            return true;
        }
    }
    return false;
}

QString RevisionGraphWidget::currentCommitHash() const
{
    if (!model_) return {};
    const int row = currentSourceRow();
    if (row < 0) return {};
    const auto* c = model_->commitAt(row);
    if (!c) return {};
    return QString::fromStdString(c->id.toHex());
}

bool RevisionGraphWidget::selectFirstParent()
{
    if (!model_) return false;
    const int row = currentSourceRow();
    if (row < 0) return false;
    const auto* c = model_->commitAt(row);
    if (!c || c->parentIds.empty()) return false;
    return selectCommit(QString::fromStdString(c->parentIds[0].toHex()));
}

bool RevisionGraphWidget::selectLastParent()
{
    if (!model_) return false;
    const int row = currentSourceRow();
    if (row < 0) return false;
    const auto* c = model_->commitAt(row);
    if (!c || c->parentIds.empty()) return false;
    return selectCommit(QString::fromStdString(
        c->parentIds.back().toHex()));
}

void RevisionGraphWidget::setColumnVisible(int column, bool visible)
{
    if (!tableView_) return;
    if (column < 0 || column >= tableView_->horizontalHeader()->count())
        return;
    tableView_->setColumnHidden(column, !visible);
}

void RevisionGraphWidget::setFilterText(const QString& text)
{
    if (proxy_) proxy_->setMessageFilter(text);
}

bool RevisionGraphWidget::selectNextMatch()
{
    if (!proxy_ || !tableView_->selectionModel()) return false;
    const int proxyRowCount = proxy_->rowCount();
    if (proxyRowCount == 0) return false;
    const QModelIndex cur =
        tableView_->selectionModel()->currentIndex();
    int nextRow = 0;
    if (cur.isValid()) {
        nextRow = cur.row() + 1;
        if (nextRow >= proxyRowCount) return false;
    }
    const QModelIndex tgt = proxy_->index(nextRow, 0);
    tableView_->setCurrentIndex(tgt);
    tableView_->scrollTo(tgt, QAbstractItemView::PositionAtCenter);
    return true;
}

bool RevisionGraphWidget::selectPreviousMatch()
{
    if (!proxy_ || !tableView_->selectionModel()) return false;
    const QModelIndex cur =
        tableView_->selectionModel()->currentIndex();
    if (!cur.isValid()) return false;
    const int prevRow = cur.row() - 1;
    if (prevRow < 0) return false;
    const QModelIndex tgt = proxy_->index(prevRow, 0);
    tableView_->setCurrentIndex(tgt);
    tableView_->scrollTo(tgt, QAbstractItemView::PositionAtCenter);
    return true;
}

bool RevisionGraphWidget::selectFirstChild()
{
    if (!model_) return false;
    const int curRow = currentSourceRow();
    if (curRow < 0) return false;
    const auto* current = model_->commitAt(curRow);
    if (!current) return false;
    const std::string needle = current->id.toHex();
    // Walk newer rows (lower index) for any commit whose parentIds
    // contains us. We stop at the first hit — branch points have
    // multiple children but a single deterministic pick keeps the
    // navigation predictable; if the user wants a specific branch
    // they can click on the lane themselves.
    for (int row = curRow - 1; row >= 0; --row) {
        const auto* candidate = model_->commitAt(row);
        if (!candidate) continue;
        for (const auto& pid : candidate->parentIds) {
            if (pid.toHex() == needle) {
                return selectCommit(
                    QString::fromStdString(candidate->id.toHex()));
            }
        }
    }
    return false;
}

void RevisionGraphWidget::onSelectionChanged()
{
    if (!model_) return;
    const int row = currentSourceRow();
    if (row < 0) {
        // Selection was cleared (e.g. by modifier-click on the
        // selected row). Emit an empty hash so RepositoryView
        // can blank the inspector tabs — without this signal the
        // tabs would keep showing details for the previously
        // selected commit.
        emit commitSelected(QString());
        return;
    }
    const auto* commit = model_->commitAt(row);
    if (commit) {
        emit commitSelected(QString::fromStdString(commit->id.toHex()));
    }
}

bool RevisionGraphWidget::eventFilter(QObject* watched, QEvent* event)
{
    // We only filter the table viewport's mouse press to implement
    // modifier-click-to-deselect. Anything else falls through to
    // QWidget::eventFilter which is a no-op pass-through.
    if (watched == tableView_->viewport() &&
        event->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(event);
        // Treat both Ctrl (literal) and Meta (Cmd on macOS, since
        // Qt's ControlModifier already maps to Cmd on macOS but
        // some users rely on the literal Ctrl key via Karabiner
        // / external keyboards). MetaModifier covers Mac's
        // physical Control key in that case.
        const bool modifierHeld =
            me->modifiers().testFlag(Qt::ControlModifier) ||
            me->modifiers().testFlag(Qt::MetaModifier);
        if (modifierHeld && me->button() == Qt::LeftButton) {
            const QModelIndex idx =
                tableView_->indexAt(me->pos());
            // Click landed in empty space — let the default handler
            // run (which does nothing in single-selection mode).
            if (!idx.isValid()) return false;
            auto* sm = tableView_->selectionModel();
            if (!sm) return false;
            const QModelIndex cur = sm->currentIndex();
            // If the modifier-click is on the currently-selected
            // row, clear the selection so the user can reach a
            // "nothing selected" state. Consume the event so the
            // default click handler doesn't immediately reselect
            // the row underneath.
            if (cur.isValid() && cur.row() == idx.row()) {
                sm->clearSelection();
                sm->clearCurrentIndex();
                return true;
            }
            // Otherwise the user is modifier-clicking a different
            // row — fall through and let the table select it
            // normally. SingleSelection means the previous row is
            // dropped automatically.
        }
    }
    return QWidget::eventFilter(watched, event);
}

void RevisionGraphWidget::resizeGraphColumn()
{
    if (!model_ || model_->rowCount() == 0)
        return;

    // Scan visible rows to determine the widest graph column needed
    int firstVisible = tableView_->rowAt(0);
    int lastVisible = tableView_->rowAt(tableView_->viewport()->height());
    if (firstVisible < 0) firstVisible = 0;
    if (lastVisible < 0) lastVisible = model_->rowCount() - 1;
    lastVisible = std::min(lastVisible, model_->rowCount() - 1);

    int maxLane = 0;
    for (int row = firstVisible; row <= lastVisible; ++row) {
        const auto* g = model_->graphAt(row);
        if (g) maxLane = std::max(maxLane, g->maxLane);
    }

    int width = std::max(80, (maxLane + 2) * editor::RevisionGraphDelegate::LANE_WIDTH);
    tableView_->setColumnWidth(
        static_cast<int>(models::CommitLogColumn::Graph), width);
}

} // namespace gitbolt::widgets
