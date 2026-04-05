#include "gitbolt/views/settings_dialog.h"
#include <QVBoxLayout>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QLabel>

namespace gitbolt::views {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    resize(600, 400);
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(new QLabel(tr("General settings"), this), tr("General"));
    tabs->addTab(new QLabel(tr("Git configuration"), this), tr("Git Config"));
    tabs->addTab(new QLabel(tr("Appearance settings"), this), tr("Appearance"));
    tabs->addTab(new QLabel(tr("Keyboard shortcuts"), this), tr("Shortcuts"));
    layout->addWidget(tabs);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

} // namespace gitbolt::views
