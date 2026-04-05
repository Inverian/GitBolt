#pragma once
#include <QDialog>

namespace gitbolt::ui {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
};

} // namespace gitbolt::ui
