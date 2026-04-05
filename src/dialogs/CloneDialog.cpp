#include "dialogs/CloneDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QFileDialog>

namespace gitbolt::ui {

CloneDialog::CloneDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Clone Repository"));
    resize(500, 180);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Repository URL:"), this));
    auto* urlEdit = new QLineEdit(this);
    urlEdit->setPlaceholderText("https://github.com/user/repo.git");
    layout->addWidget(urlEdit);
    layout->addWidget(new QLabel(tr("Clone to:"), this));
    auto* pathLayout = new QHBoxLayout;
    auto* pathEdit = new QLineEdit(this);
    pathLayout->addWidget(pathEdit);
    auto* browseBtn = new QPushButton(tr("Browse..."), this);
    connect(browseBtn, &QPushButton::clicked, this, [pathEdit, this]() {
        QString dir = QFileDialog::getExistingDirectory(this, tr("Select Directory"));
        if (!dir.isEmpty()) pathEdit->setText(dir);
    });
    pathLayout->addWidget(browseBtn);
    layout->addLayout(pathLayout);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Clone"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString CloneDialog::url() const { return {}; }
QString CloneDialog::path() const { return {}; }

} // namespace gitbolt::ui
