#include "dialogs/ReflogDialog.h"
#include "conf/SettingsService.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

ReflogDialog::ReflogDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
}

void ReflogDialog::setRefs(const QStringList& refs) {
    // Block signals during repopulation so we don't fire
    // `refSelected` for every Add — we'll fire it manually for
    // the final selected entry below.
    refCombo_->blockSignals(true);
    refCombo_->clear();
    refCombo_->addItems(refs);
    refCombo_->blockSignals(false);
    if (!refs.isEmpty())
        emit refSelected(refs.first());
}

void ReflogDialog::setEntries(
        const std::vector<gitbolt::git::ReflogEntry>& entries) {
    table_->setRowCount(0);
    for (const auto& e : entries) {
        const int row = table_->rowCount();
        table_->insertRow(row);

        // Old SHA — shorten to 8 hex chars for display, full SHA
        // is in the cell tooltip via setToolTip.
        const QString oldHex = QString::fromStdString(e.oldId.toHex());
        const QString newHex = QString::fromStdString(e.newId.toHex());
        auto* oldItem = new QTableWidgetItem(
            oldHex.left(8));
        oldItem->setToolTip(oldHex);
        auto* newItem = new QTableWidgetItem(
            newHex.left(8));
        newItem->setToolTip(newHex);
        // Full SHA for the context-menu actions (checkout / reset
        // target). Stored on the row's first cell so the handler
        // doesn't have to parse it back out of a tooltip.
        newItem->setData(Qt::UserRole, newHex);

        const auto t = std::chrono::system_clock::to_time_t(
            e.committer.when);
        const QString when = QDateTime::fromSecsSinceEpoch(
            static_cast<qint64>(t)).toString("yyyy-MM-dd hh:mm");

        auto* whoItem = new QTableWidgetItem(
            QString::fromStdString(e.committer.name));
        auto* whenItem = new QTableWidgetItem(when);
        auto* msgItem = new QTableWidgetItem(
            QString::fromStdString(e.message));

        for (auto* it : {oldItem, newItem, whoItem, whenItem, msgItem})
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);

        table_->setItem(row, 0, oldItem);
        table_->setItem(row, 1, newItem);
        table_->setItem(row, 2, whoItem);
        table_->setItem(row, 3, whenItem);
        table_->setItem(row, 4, msgItem);
    }
    table_->resizeColumnsToContents();
    // Stretch the message column to fill the rest after we've
    // measured the fixed-width columns. Order matters: stretch
    // before resize would zero-out the contents widths first.
    table_->horizontalHeader()->setSectionResizeMode(
        4, QHeaderView::Stretch);
}

void ReflogDialog::setupUi() {
    setWindowTitle(tr("Reflog"));
    // Global dialog default + per-dialog restore — see
    // Settings → UI Design → Default Dialog Size. Helper sets
    // initial size and wires up save-on-close.
    conf::SettingsService::applyConfiguredSize(this, "reflog");

    auto* layout = new QVBoxLayout(this);

    auto* refRow = new QHBoxLayout();
    refRow->addWidget(new QLabel(tr("Reference:"), this));
    refCombo_ = new QComboBox(this);
    // Cap the combo to a sensible width — full ref names like
    // refs/heads/feature/some-long-thing fit at ~280px without
    // expanding the field across the entire dialog.
    refCombo_->setSizePolicy(QSizePolicy::Preferred,
                             QSizePolicy::Fixed);
    refCombo_->setMaximumWidth(280);
    refRow->addWidget(refCombo_);

    // Push the info button to the right edge so the combo stays
    // tight against its label on the left.
    refRow->addStretch(1);

    // Info button — uses the same Google Material Symbols icon
    // pack the rest of the app pulls from. `about.svg` IS the
    // Material "info" glyph (circle + "i"); reusing it keeps the
    // visual language consistent with the rest of the menus.
    // QToolButton + autoRaise gives the standard "flat until
    // hover" affordance.
    auto* infoBtn = new QToolButton(this);
    infoBtn->setIcon(QIcon(QStringLiteral(":/icons/menu/about.svg")));
    infoBtn->setIconSize(QSize(20, 20));
    infoBtn->setAutoRaise(true);
    infoBtn->setCursor(Qt::PointingHandCursor);
    infoBtn->setToolTip(tr(
        "The reflog records every time this ref moved: commits, "
        "resets, rebases, checkouts. Click for more detail."));
    connect(infoBtn, &QToolButton::clicked, this, [this]() {
        // Custom dialog instead of QMessageBox::information so we
        // can use the Material Symbols info glyph (the macOS
        // QMessageBox icon renders as a yellow alert and looks
        // wrong for an explainer popup), control spacing, and
        // structure the body into clear sections. No em dashes
        // anywhere in the copy: ordinary punctuation reads better
        // on smaller line widths and avoids the visual noise of
        // a long horizontal stroke.
        QDialog dlg(this);
        dlg.setWindowTitle(tr("About the reflog"));
        dlg.setMinimumWidth(600);

        auto* root = new QVBoxLayout(&dlg);
        root->setContentsMargins(20, 20, 20, 16);
        root->setSpacing(14);

        // Header: large Material info icon next to a bold title.
        auto* headerRow = new QHBoxLayout();
        headerRow->setSpacing(14);
        auto* icon = new QLabel(&dlg);
        icon->setPixmap(QIcon(QStringLiteral(":/icons/menu/about.svg"))
                            .pixmap(40, 40));
        icon->setAlignment(Qt::AlignTop);
        headerRow->addWidget(icon);
        auto* title = new QLabel(
            QStringLiteral("<span style='font-size:16pt;"
                           "font-weight:600;'>%1</span>")
                .arg(tr("About the reflog")), &dlg);
        title->setTextFormat(Qt::RichText);
        title->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        headerRow->addWidget(title, 1);
        root->addLayout(headerRow);

        // Body. Sections are separate QLabels so each gets its
        // own paragraph spacing without us hard-coding HTML
        // line-height. setWordWrap(true) handles narrow widths
        // gracefully; OpenExternalLinks lets the SHA recovery
        // command be copyable.
        auto makeBody = [&](const QString& text) {
            auto* lbl = new QLabel(text, &dlg);
            lbl->setTextFormat(Qt::RichText);
            lbl->setWordWrap(true);
            lbl->setTextInteractionFlags(
                Qt::TextSelectableByMouse |
                Qt::TextSelectableByKeyboard);
            return lbl;
        };

        root->addWidget(makeBody(tr(
            "<p style='margin:0;'>Git keeps a private, "
            "local-only log of every operation that moved a "
            "reference such as <code>HEAD</code> or a branch "
            "tip. That includes commits, resets, rebases, "
            "checkouts, merges, and cherry-picks. Each entry "
            "records the old SHA, the new SHA, who performed "
            "the action, when it happened, and a short message "
            "describing it.</p>")));

        root->addWidget(makeBody(tr(
            "<p style='margin:0;'><b>It is your safety net.</b> "
            "If a rebase or reset looks like it lost work, the "
            "commit is not gone. The reflog still references "
            "it, and you can recover the state with "
            "<code>git reset --hard &lt;old&nbsp;SHA&gt;</code>."
            "</p>")));

        root->addWidget(makeBody(tr(
            "<p style='margin:0;'>Reflog entries live in "
            "<code>.git/logs/&lt;ref&gt;</code> on your local "
            "machine. They are never pushed or fetched, and "
            "they expire after 90 days for reachable entries "
            "(30 days for unreachable) by default.</p>")));

        root->addStretch(1);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Close, &dlg);
        connect(buttons, &QDialogButtonBox::rejected,
                &dlg, &QDialog::accept);
        root->addWidget(buttons);

        dlg.exec();
    });
    refRow->addWidget(infoBtn);

    layout->addLayout(refRow);

    table_ = new QTableWidget(this);
    table_->setObjectName(QStringLiteral("reflog.table"));
    table_->setColumnCount(5);
    table_->setHorizontalHeaderLabels({
        tr("Old"), tr("New"), tr("Committer"),
        tr("When"), tr("Message"),
    });
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->verticalHeader()->setVisible(false);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested,
            this, &ReflogDialog::onTableContextMenu);
    layout->addWidget(table_, /*stretch=*/1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Close, this);
    closeBtn_ = buttons->button(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::accept);
    layout->addWidget(buttons);

    // Use the modern overload for combo selection to avoid the
    // deprecated currentIndexChanged(QString) ambiguity.
    connect(refCombo_,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        if (idx < 0) return;
        emit refSelected(refCombo_->itemText(idx));
    });
}

void ReflogDialog::refreshCurrentRef()
{
    if (refCombo_->currentIndex() >= 0)
        emit refSelected(refCombo_->currentText());
}

void ReflogDialog::onTableContextMenu(const QPoint& pos)
{
    auto* item = table_->itemAt(pos);
    if (!item)
        return;
    // The full "new" SHA is stored as UserRole data on the row's
    // "New" cell (column 1) — see setEntries. Actions act on that
    // SHA: the state the ref moved to, i.e. what you'd recover.
    auto* anchor = table_->item(item->row(), 1);
    if (!anchor)
        return;
    const QString sha = anchor->data(Qt::UserRole).toString();
    if (sha.isEmpty())
        return;
    const QString shortSha = sha.left(8);

    QMenu menu(this);
    menu.addAction(tr("Checkout %1 (detached HEAD)").arg(shortSha),
                   this, [this, sha]() {
        emit checkoutRequested(sha);
    });
    menu.addSeparator();
    auto* resetMenu = menu.addMenu(
        tr("Reset current branch to %1").arg(shortSha));
    resetMenu->addAction(tr("Soft — keep index and working tree"),
                         this, [this, sha]() {
        emit resetRequested(sha, QStringLiteral("soft"));
    });
    resetMenu->addAction(tr("Mixed — keep working tree"),
                         this, [this, sha]() {
        emit resetRequested(sha, QStringLiteral("mixed"));
    });
    resetMenu->addAction(tr("Hard — discard all local changes"),
                         this, [this, sha]() {
        emit resetRequested(sha, QStringLiteral("hard"));
    });

    menu.exec(table_->viewport()->mapToGlobal(pos));
}

} // namespace gitbolt::dialogs
