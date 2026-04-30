#include "dialogs/RemotesDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace gitbolt::dialogs {

RemotesDialog::RemotesDialog(QWidget* parent)
    : QDialog(parent) {
    setupUi();
}

void RemotesDialog::setRemotes(
        const std::vector<gitbolt::git::RemoteInfo>& remotes) {
    table_->setRowCount(0);
    for (const auto& r : remotes) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* nameItem = new QTableWidgetItem(
            QString::fromStdString(r.name));
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);

        // Most remotes have one URL used for both fetch and push.
        // When pushUrl is empty libgit2 means "same as fetch URL".
        // Only annotate the cell when they actually differ — most
        // users will never see the (push: ...) suffix.
        QString urlText = QString::fromStdString(r.url);
        if (!r.pushUrl.empty() && r.pushUrl != r.url) {
            urlText += QStringLiteral("\n(push: %1)")
                .arg(QString::fromStdString(r.pushUrl));
        }
        auto* urlItem = new QTableWidgetItem(urlText);
        urlItem->setFlags(urlItem->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, 0, nameItem);
        table_->setItem(row, 1, urlItem);
    }
    table_->resizeRowsToContents();
    onSelectionChanged();
}

void RemotesDialog::setupUi() {
    setWindowTitle(tr("Remote Repositories"));
    resize(640, 360);

    auto* layout = new QVBoxLayout(this);

    auto* header = new QLabel(
        tr("Configured remotes for this repository:"), this);
    layout->addWidget(header);

    table_ = new QTableWidget(this);
    table_->setColumnCount(2);
    table_->setHorizontalHeaderLabels({tr("Name"), tr("URL")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    layout->addWidget(table_, /*stretch=*/1);
    connect(table_, &QTableWidget::itemSelectionChanged,
            this, &RemotesDialog::onSelectionChanged);

    auto* btnRow = new QHBoxLayout();
    addBtn_    = new QPushButton(tr("&Add..."), this);
    editBtn_   = new QPushButton(tr("&Edit URL..."), this);
    removeBtn_ = new QPushButton(tr("&Remove"), this);
    closeBtn_  = new QPushButton(tr("&Close"), this);
    btnRow->addWidget(addBtn_);
    btnRow->addWidget(editBtn_);
    btnRow->addWidget(removeBtn_);
    btnRow->addStretch(1);
    btnRow->addWidget(closeBtn_);
    layout->addLayout(btnRow);

    connect(addBtn_, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Add Remote"),
            tr("Remote name (e.g. origin):"),
            QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        const QString url = QInputDialog::getText(
            this, tr("Add Remote"),
            tr("Fetch URL:"),
            QLineEdit::Normal, QString(), &ok);
        if (!ok || url.trimmed().isEmpty()) return;
        emit addRequested(name.trimmed(), url.trimmed());
    });

    connect(editBtn_, &QPushButton::clicked, this, [this]() {
        const QString name = selectedName();
        if (name.isEmpty()) return;
        const QString currentUrl = selectedUrl();
        bool ok = false;
        const QString newUrl = QInputDialog::getText(
            this, tr("Edit Remote URL"),
            tr("New URL for \"%1\":").arg(name),
            QLineEdit::Normal, currentUrl, &ok);
        if (!ok || newUrl.trimmed().isEmpty() ||
            newUrl.trimmed() == currentUrl)
            return;
        emit editUrlRequested(name, newUrl.trimmed());
    });

    connect(removeBtn_, &QPushButton::clicked, this, [this]() {
        const QString name = selectedName();
        if (name.isEmpty()) return;
        emit removeRequested(name);
    });

    connect(closeBtn_, &QPushButton::clicked,
            this, &QDialog::accept);

    onSelectionChanged();
}

void RemotesDialog::onSelectionChanged() {
    const bool any = !selectedName().isEmpty();
    editBtn_->setEnabled(any);
    removeBtn_->setEnabled(any);
}

QString RemotesDialog::selectedName() const {
    auto* item = table_->item(table_->currentRow(), 0);
    return item ? item->text() : QString();
}

QString RemotesDialog::selectedUrl() const {
    auto* item = table_->item(table_->currentRow(), 1);
    return item ? item->text() : QString();
}

} // namespace gitbolt::dialogs
