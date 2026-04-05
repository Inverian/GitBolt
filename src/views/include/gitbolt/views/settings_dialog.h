#pragma once
#include <QDialog>

namespace gitbolt::views {

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);
};

} // namespace gitbolt::views
