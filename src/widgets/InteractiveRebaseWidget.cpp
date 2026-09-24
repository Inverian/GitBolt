#include "widgets/InteractiveRebaseWidget.h"

#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QVBoxLayout>

namespace gitbolt::widgets {

// ===========================================================================
// RebaseListModel
// ===========================================================================

RebaseListModel::RebaseListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int RebaseListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(ops_.size());
}

QVariant RebaseListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    int row = index.row();
    if (row < 0 || row >= static_cast<int>(ops_.size()))
        return {};

    // row >= 0 checked above.
    const auto& op = ops_[static_cast<size_t>(row)];

    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("%1 %2 %3")
            .arg(QString::fromStdString(op.commitId.toShortHex()))
            .arg(QString::fromUtf8(" "))
            .arg(QString::fromStdString(op.message).section('\n', 0, 0));
    case OperationTypeRole:
        return static_cast<int>(op.type);
    case CommitHashRole:
        return QString::fromStdString(op.commitId.toShortHex());
    case CommitMessageRole:
        return QString::fromStdString(op.message).section('\n', 0, 0);
    default:
        return {};
    }
}

bool RebaseListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid())
        return false;
    int row = index.row();
    if (row < 0 || row >= static_cast<int>(ops_.size()))
        return false;

    if (role == OperationTypeRole) {
        // row >= 0 checked above.
        ops_[static_cast<size_t>(row)].type = static_cast<git::RebaseOperationType>(value.toInt());
        emit dataChanged(index, index, {role, Qt::DisplayRole});
        return true;
    }
    return false;
}

Qt::ItemFlags RebaseListModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags defaultFlags = QAbstractListModel::flags(index);
    if (index.isValid())
        return defaultFlags | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    return defaultFlags | Qt::ItemIsDropEnabled;
}

Qt::DropActions RebaseListModel::supportedDropActions() const {
    return Qt::MoveAction;
}

bool RebaseListModel::moveRows(const QModelIndex& sourceParent, int sourceRow, int count,
                                const QModelIndex& destinationParent, int destinationChild) {
    Q_UNUSED(sourceParent)
    Q_UNUSED(destinationParent)

    if (sourceRow < 0 || sourceRow + count > static_cast<int>(ops_.size()))
        return false;
    if (destinationChild < 0 || destinationChild > static_cast<int>(ops_.size()))
        return false;
    if (sourceRow == destinationChild || sourceRow + count == destinationChild)
        return false;

    // Qt's convention (see QStringListModel::moveRows):
    // destinationChild is the pre-move insert-before row — exactly
    // what dropMimeData hands us — and goes to beginMoveRows
    // UNMODIFIED. The old +count adjustment announced a destination
    // one slot too far on downward drags; the rows displayed right
    // (data() refetches) but persistent indexes — the selection —
    // remapped to the wrong row.
    beginMoveRows(QModelIndex(), sourceRow, sourceRow + count - 1,
                  QModelIndex(), destinationChild);

    // Collect the moved items
    std::vector<git::RebaseOperation> moved(
        ops_.begin() + sourceRow,
        ops_.begin() + sourceRow + count);
    ops_.erase(ops_.begin() + sourceRow, ops_.begin() + sourceRow + count);

    int insertAt = (destinationChild > sourceRow)
                       ? destinationChild - count
                       : destinationChild;
    ops_.insert(ops_.begin() + insertAt, moved.begin(), moved.end());

    endMoveRows();
    return true;
}

QStringList RebaseListModel::mimeTypes() const {
    return {QStringLiteral("application/x-gitbolt-rebase-row")};
}

QMimeData* RebaseListModel::mimeData(const QModelIndexList& indexes) const {
    auto* mimeData = new QMimeData;
    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    for (const auto& idx : indexes) {
        if (idx.isValid())
            stream << idx.row();
    }
    mimeData->setData(QStringLiteral("application/x-gitbolt-rebase-row"), encoded);
    return mimeData;
}

bool RebaseListModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
                                    int row, int /*column*/, const QModelIndex& parent) {
    if (action == Qt::IgnoreAction)
        return true;
    if (!data->hasFormat(QStringLiteral("application/x-gitbolt-rebase-row")))
        return false;

    int destinationRow = row;
    if (row == -1) {
        if (parent.isValid())
            destinationRow = parent.row();
        else
            destinationRow = rowCount();
    }

    QByteArray encoded = data->data(QStringLiteral("application/x-gitbolt-rebase-row"));
    QDataStream stream(&encoded, QIODevice::ReadOnly);
    int sourceRow = -1;
    stream >> sourceRow;
    if (sourceRow < 0)
        return false;

    return moveRows(QModelIndex(), sourceRow, 1, QModelIndex(), destinationRow);
}

void RebaseListModel::setOperations(std::vector<git::RebaseOperation> ops) {
    beginResetModel();
    ops_ = std::move(ops);
    endResetModel();
}

void RebaseListModel::setOperationType(int row, git::RebaseOperationType type) {
    if (row < 0 || row >= static_cast<int>(ops_.size()))
        return;
    setData(index(row), static_cast<int>(type), OperationTypeRole);
}

// ===========================================================================
// RebaseOperationDelegate
// ===========================================================================

RebaseOperationDelegate::RebaseOperationDelegate(QObject* parent)
    : QStyledItemDelegate(parent) {}

QColor RebaseOperationDelegate::colorForOperation(git::RebaseOperationType type) {
    switch (type) {
    case git::RebaseOperationType::Pick:    return QColor(0x4C, 0xAF, 0x50); // green
    case git::RebaseOperationType::Reword:  return QColor(0xFF, 0x98, 0x00); // orange
    case git::RebaseOperationType::Squash:  return QColor(0x21, 0x96, 0xF3); // blue
    case git::RebaseOperationType::Fixup:   return QColor(0x9C, 0x27, 0xB0); // purple
    case git::RebaseOperationType::Edit:    return QColor(0xFF, 0xEB, 0x3B); // yellow
    case git::RebaseOperationType::Drop:    return QColor(0xF4, 0x43, 0x36); // red
    }
    return QColor(Qt::gray);
}

QString RebaseOperationDelegate::labelForOperation(git::RebaseOperationType type) {
    switch (type) {
    case git::RebaseOperationType::Pick:    return QStringLiteral("pick");
    case git::RebaseOperationType::Reword:  return QStringLiteral("reword");
    case git::RebaseOperationType::Squash:  return QStringLiteral("squash");
    case git::RebaseOperationType::Fixup:   return QStringLiteral("fixup");
    case git::RebaseOperationType::Edit:    return QStringLiteral("edit");
    case git::RebaseOperationType::Drop:    return QStringLiteral("drop");
    }
    return QStringLiteral("pick");
}

void RebaseOperationDelegate::paint(QPainter* painter,
                                     const QStyleOptionViewItem& option,
                                     const QModelIndex& index) const {
    painter->save();

    // Draw background (selection highlight)
    if (option.state & QStyle::State_Selected) {
        painter->fillRect(option.rect, option.palette.highlight());
        painter->setPen(option.palette.highlightedText().color());
    } else {
        painter->fillRect(option.rect, option.palette.base());
        painter->setPen(option.palette.text().color());
    }

    auto opType = static_cast<git::RebaseOperationType>(
        index.data(RebaseListModel::OperationTypeRole).toInt());
    QString hash = index.data(RebaseListModel::CommitHashRole).toString();
    QString message = index.data(RebaseListModel::CommitMessageRole).toString();
    QString label = labelForOperation(opType);

    int x = option.rect.left() + 6;
    int y = option.rect.top();
    int h = option.rect.height();

    // Operation label badge
    QFont boldFont = option.font;
    boldFont.setBold(true);
    boldFont.setPointSize(boldFont.pointSize() - 1);
    QFontMetrics boldFm(boldFont);
    int labelWidth = boldFm.horizontalAdvance(label) + 12;
    int badgeHeight = h - 6;
    int badgeY = y + 3;

    QRect badgeRect(x, badgeY, labelWidth, badgeHeight);
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(colorForOperation(opType));
    painter->setPen(Qt::NoPen);
    painter->drawRoundedRect(badgeRect, 3, 3);

    painter->setPen(Qt::white);
    painter->setFont(boldFont);
    painter->drawText(badgeRect, Qt::AlignCenter, label);

    x += labelWidth + 8;

    // Commit hash (monospace)
    QFont monoFont = option.font;
    monoFont.setFamily(QStringLiteral("Menlo,Consolas,monospace"));
    monoFont.setPointSize(monoFont.pointSize() - 1);
    QFontMetrics monoFm(monoFont);
    int hashWidth = monoFm.horizontalAdvance(hash);

    painter->setFont(monoFont);
    QColor hashColor = (option.state & QStyle::State_Selected)
                           ? option.palette.highlightedText().color()
                           : QColor(0x79, 0x86, 0xCB); // muted blue
    painter->setPen(hashColor);
    painter->drawText(x, y, hashWidth, h, Qt::AlignVCenter, hash);

    x += hashWidth + 8;

    // Commit message (first line)
    painter->setFont(option.font);
    painter->setPen((option.state & QStyle::State_Selected)
                        ? option.palette.highlightedText().color()
                        : option.palette.text().color());
    int remaining = option.rect.right() - x - 4;
    if (remaining > 0) {
        QString elided = option.fontMetrics.elidedText(message, Qt::ElideRight, remaining);
        painter->drawText(x, y, remaining, h, Qt::AlignVCenter, elided);
    }

    painter->restore();
}

QSize RebaseOperationDelegate::sizeHint(const QStyleOptionViewItem& option,
                                         const QModelIndex& /*index*/) const {
    return QSize(option.rect.width(), qMax(28, option.fontMetrics.height() + 10));
}

// ===========================================================================
// InteractiveRebaseWidget
// ===========================================================================

InteractiveRebaseWidget::InteractiveRebaseWidget(QWidget* parent)
    : QWidget(parent)
    , model_(new RebaseListModel(this))
    , listView_(new QListView(this))
    , delegate_(new RebaseOperationDelegate(this))
    , toolbar_(new QToolBar(this))
    , startBtn_(new QPushButton(tr("Start Rebase"), this))
    , abortBtn_(new QPushButton(tr("Abort"), this))
    , continueBtn_(new QPushButton(tr("Continue"), this))
    , skipBtn_(new QPushButton(tr("Skip"), this))
{
    setupUi();
}

void InteractiveRebaseWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    toolbar_->addWidget(startBtn_);
    toolbar_->addSeparator();
    toolbar_->addWidget(continueBtn_);
    toolbar_->addWidget(skipBtn_);
    toolbar_->addWidget(abortBtn_);

    abortBtn_->setEnabled(false);
    continueBtn_->setEnabled(false);
    skipBtn_->setEnabled(false);

    layout->addWidget(toolbar_);

    // List view with drag-and-drop
    listView_->setModel(model_);
    listView_->setItemDelegate(delegate_);
    listView_->setDragEnabled(true);
    listView_->setAcceptDrops(true);
    listView_->setDropIndicatorShown(true);
    listView_->setDragDropMode(QAbstractItemView::InternalMove);
    listView_->setDefaultDropAction(Qt::MoveAction);
    listView_->setSelectionMode(QAbstractItemView::SingleSelection);
    listView_->setAlternatingRowColors(false);

    layout->addWidget(listView_, 1);

    // Connections
    connect(startBtn_, &QPushButton::clicked,
            this, &InteractiveRebaseWidget::onStartRebase);
    connect(abortBtn_, &QPushButton::clicked,
            this, &InteractiveRebaseWidget::rebaseAbortRequested);
    connect(continueBtn_, &QPushButton::clicked,
            this, &InteractiveRebaseWidget::rebaseContinueRequested);
    connect(skipBtn_, &QPushButton::clicked,
            this, &InteractiveRebaseWidget::rebaseSkipRequested);
}

void InteractiveRebaseWidget::setCommits(const std::vector<git::CommitData>& commits,
                                          const git::ObjectId& onto) {
    onto_ = onto;

    std::vector<git::RebaseOperation> ops;
    ops.reserve(commits.size());
    for (const auto& c : commits) {
        git::RebaseOperation op;
        op.type = git::RebaseOperationType::Pick;
        op.commitId = c.id;
        op.message = c.summary.empty() ? c.message : c.summary;
        ops.push_back(std::move(op));
    }
    model_->setOperations(std::move(ops));

    startBtn_->setEnabled(true);
    abortBtn_->setEnabled(false);
    continueBtn_->setEnabled(false);
    skipBtn_->setEnabled(false);
}

git::RebasePlan InteractiveRebaseWidget::rebasePlan() const {
    git::RebasePlan plan;
    plan.onto = onto_;
    plan.operations = model_->operations();
    return plan;
}

void InteractiveRebaseWidget::clear() {
    model_->setOperations({});
    onto_ = git::ObjectId();
    startBtn_->setEnabled(false);
    abortBtn_->setEnabled(false);
    continueBtn_->setEnabled(false);
    skipBtn_->setEnabled(false);
}

void InteractiveRebaseWidget::onStartRebase() {
    auto plan = rebasePlan();
    if (plan.operations.empty())
        return;

    startBtn_->setEnabled(false);
    abortBtn_->setEnabled(true);
    continueBtn_->setEnabled(true);
    skipBtn_->setEnabled(true);

    emit rebaseRequested(plan);
}

} // namespace gitbolt::widgets
