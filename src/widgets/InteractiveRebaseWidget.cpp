#include "widgets/InteractiveRebaseWidget.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDrag>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QListView>
#include <QMimeData>
#include <QPainter>
#include <QShortcut>
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
    // A reworded commit shows the subject it is getting.
    const bool reworded = op.type == git::RebaseOperationType::Reword && !op.newMessage.empty();
    const QString subject =
        QString::fromStdString(reworded ? op.newMessage : op.message).section('\n', 0, 0);

    switch (role) {
    case Qt::DisplayRole:
        return QStringLiteral("%1 %2 %3")
            .arg(QString::fromStdString(op.commitId.toShortHex()))
            .arg(QString::fromUtf8(" "))
            .arg(subject);
    case Qt::ToolTipRole:
        switch (op.type) {
        case git::RebaseOperationType::Pick:
            return tr("Keep this commit as it is.");
        case git::RebaseOperationType::Reword:
            return tr("Keep the changes, with a new message:\n\n%1")
                .arg(QString::fromStdString(op.newMessage).trimmed());
        case git::RebaseOperationType::Edit:
            return tr("Stop the rebase after this commit, to amend it or add commits; "
                      "then Continue.");
        case git::RebaseOperationType::Squash:
            return tr("Fold into the commit below it, keeping both messages.");
        case git::RebaseOperationType::Fixup:
            return tr("Fold into the commit below it, keeping only that commit's message.");
        case git::RebaseOperationType::Drop:
            return tr("Leave this commit out.");
        }
        return {};
    case OperationTypeRole:
        return static_cast<int>(op.type);
    case CommitHashRole:
        return QString::fromStdString(op.commitId.toShortHex());
    case CommitMessageRole:
        return subject;
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

void RebaseListModel::setNewMessage(int row, const std::string& message) {
    if (row < 0 || row >= static_cast<int>(ops_.size()))
        return;
    auto& op = ops_[static_cast<size_t>(row)];
    op.newMessage = message;
    op.type = message.empty() ? git::RebaseOperationType::Pick
                              : git::RebaseOperationType::Reword;
    emit dataChanged(index(row), index(row));
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
    , upButton_(new QPushButton(tr("Move Up"), this))
    , downButton_(new QPushButton(tr("Move Down"), this))
{
    setupUi();
}

void InteractiveRebaseWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar: what to do with the selected commit, and where it goes.
    // Each operation also has a key on the list, as in git's todo list.
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    using Type = git::RebaseOperationType;
    const struct {
        Type type;
        QString text;
        Qt::Key key;
        QString tip;
    } operations[] = {
        {Type::Pick, tr("Pick"), Qt::Key_P, tr("Keep the commit as it is")},
        {Type::Reword, tr("Reword…"), Qt::Key_R, tr("Keep the changes, with a new message")},
        {Type::Edit, tr("Edit"), Qt::Key_E,
         tr("Stop after the commit, to amend it or add commits")},
        {Type::Squash, tr("Squash"), Qt::Key_S,
         tr("Fold into the commit below, keeping both messages")},
        {Type::Fixup, tr("Fixup"), Qt::Key_F,
         tr("Fold into the commit below, keeping only its message")},
        {Type::Drop, tr("Drop"), Qt::Key_D, tr("Leave the commit out")},
    };
    for (const auto& op : operations) {
        auto* button = new QPushButton(op.text, this);
        button->setAutoDefault(false);
        button->setToolTip(QStringLiteral("%1 (%2)")
                               .arg(op.tip, QKeySequence(op.key).toString(QKeySequence::NativeText)));
        toolbar_->addWidget(button);
        operationButtons_.emplace_back(op.type, button);
        connect(button, &QPushButton::clicked, this,
                [this, type = op.type] { applyToSelected(type); });
        auto* shortcut = new QShortcut(QKeySequence(op.key), listView_);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this,
                [this, type = op.type] { applyToSelected(type); });
    }
    toolbar_->addSeparator();
    const QKeySequence upKey(Qt::CTRL | Qt::Key_Up);
    const QKeySequence downKey(Qt::CTRL | Qt::Key_Down);
    upButton_->setToolTip(tr("Move the commit up (%1)").arg(upKey.toString(QKeySequence::NativeText)));
    downButton_->setToolTip(
        tr("Move the commit down (%1)").arg(downKey.toString(QKeySequence::NativeText)));
    for (QPushButton* button : {upButton_, downButton_}) {
        button->setAutoDefault(false);
        toolbar_->addWidget(button);
    }
    connect(upButton_, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(downButton_, &QPushButton::clicked, this, [this] { moveSelected(1); });
    for (const auto& move : {std::pair{upKey, -1}, std::pair{downKey, 1}}) {
        auto* shortcut = new QShortcut(move.first, listView_);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this,
                [this, delta = move.second] { moveSelected(delta); });
    }

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
    listView_->setObjectName(QStringLiteral("rebase.planList"));
    // A double-click rewords, the one operation that needs more input.
    connect(listView_, &QListView::doubleClicked, this,
            [this] { applyToSelected(git::RebaseOperationType::Reword); });

    layout->addWidget(listView_, 1);

    connect(listView_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &InteractiveRebaseWidget::updateButtons);
    connect(model_, &QAbstractItemModel::rowsMoved, this, &InteractiveRebaseWidget::updateButtons);
    connect(model_, &QAbstractItemModel::modelReset, this, &InteractiveRebaseWidget::updateButtons);
    updateButtons();
}

void InteractiveRebaseWidget::setCommits(const std::vector<git::CommitData>& commits,
                                          const git::ObjectId& onto, const git::ObjectId& head,
                                          const std::string& branch) {
    onto_ = onto;
    head_ = head;
    branch_ = branch;

    std::vector<git::RebaseOperation> ops;
    ops.reserve(commits.size());
    for (const auto& c : commits) {
        // Merges stay out, as they do of git's own todo list: git can't
        // pick one ("is a merge but no -m option was given"), and puts
        // it back at the top of the list each time, so Continue and
        // Skip both failed on it and only Abort got out. The commits
        // under it are replayed in a line.
        if (c.isMerge())
            continue;
        git::RebaseOperation op;
        op.type = git::RebaseOperationType::Pick;
        op.commitId = c.id;
        // The whole message, for Reword to start from; the list shows
        // its first line.
        op.message = c.message.empty() ? c.summary : c.message;
        ops.push_back(std::move(op));
    }
    model_->setOperations(std::move(ops));
    if (model_->rowCount() > 0)
        listView_->setCurrentIndex(model_->index(0));
}

git::RebasePlan InteractiveRebaseWidget::rebasePlan() const {
    git::RebasePlan plan;
    plan.onto = onto_;
    plan.head = head_;
    plan.branch = branch_;
    plan.operations = model_->operations();
    return plan;
}

QString InteractiveRebaseWidget::planProblem() const {
    // Oldest first, as git runs them: a squash or fixup goes into the
    // last commit kept before it.
    const auto& ops = model_->operations();
    bool kept = false;
    for (auto op = ops.rbegin(); op != ops.rend(); ++op) {
        switch (op->type) {
        case git::RebaseOperationType::Drop:
            break;
        case git::RebaseOperationType::Squash:
        case git::RebaseOperationType::Fixup:
            if (!kept)
                return tr("\"%1\" can't be squashed or fixed up: no commit below it is kept "
                          "for it to go into.")
                    .arg(QString::fromStdString(op->message).section(QLatin1Char('\n'), 0, 0));
            break;
        case git::RebaseOperationType::Pick:
        case git::RebaseOperationType::Reword:
        case git::RebaseOperationType::Edit:
            kept = true;
            break;
        }
    }
    return {};
}

void InteractiveRebaseWidget::clear() {
    model_->setOperations({});
    onto_ = git::ObjectId();
    head_ = git::ObjectId();
    branch_.clear();
}

void InteractiveRebaseWidget::applyToSelected(git::RebaseOperationType type) {
    const int row = selectedRow();
    if (row < 0)
        return;
    if (type != git::RebaseOperationType::Reword) {
        model_->setOperationType(row, type);
        return;
    }
    const auto& op = model_->operations()[static_cast<size_t>(row)];
    const QString original = QString::fromStdString(op.message).trimmed();
    const bool reworded = op.type == git::RebaseOperationType::Reword;

    QInputDialog prompt(this);
    prompt.setWindowTitle(tr("Reword Commit"));
    prompt.setLabelText(
        tr("New message for %1:").arg(QString::fromStdString(op.commitId.toShortHex())));
    prompt.setOption(QInputDialog::UsePlainTextEditForTextInput);
    prompt.setTextValue(op.newMessage.empty() ? original
                                              : QString::fromStdString(op.newMessage).trimmed());
    // git makes no commit with an empty message: it stops the rebase
    // instead. So OK waits for some text. (The prompt builds its
    // buttons when it is shown, before anyone can type.)
    connect(&prompt, &QInputDialog::textValueChanged, &prompt, [&prompt](const QString& text) {
        if (auto* buttons = prompt.findChild<QDialogButtonBox*>())
            buttons->button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty());
    });
    if (prompt.exec() != QDialog::Accepted)
        return;
    const QString text = prompt.textValue().trimmed();
    if (text.isEmpty())
        return;
    if (text != original)
        model_->setNewMessage(row, text.toStdString() + "\n");
    else if (reworded)
        model_->setNewMessage(row, {});
}

void InteractiveRebaseWidget::moveSelected(int delta) {
    const int row = selectedRow();
    const int target = row + delta;
    if (row < 0 || delta == 0 || target < 0 || target >= model_->rowCount())
        return;
    // moveRows wants the row to insert before, counted before the move.
    model_->moveRows(QModelIndex(), row, 1, QModelIndex(), delta > 0 ? target + 1 : target);
    listView_->setCurrentIndex(model_->index(target));
}

void InteractiveRebaseWidget::updateButtons() {
    const int row = selectedRow();
    for (const auto& [type, button] : operationButtons_)
        button->setEnabled(row >= 0);
    upButton_->setEnabled(row > 0);
    downButton_->setEnabled(row >= 0 && row < model_->rowCount() - 1);
}

int InteractiveRebaseWidget::selectedRow() const {
    const QModelIndex current = listView_->currentIndex();
    return current.isValid() ? current.row() : -1;
}

} // namespace gitbolt::widgets
