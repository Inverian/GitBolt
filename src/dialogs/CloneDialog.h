#pragma once
#include <QDialog>

namespace gitbolt::ui {

class CloneDialog : public QDialog {
    Q_OBJECT
public:
    explicit CloneDialog(QWidget* parent = nullptr);
    QString url() const;
    QString path() const;
};

} // namespace gitbolt::ui
