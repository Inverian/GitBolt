#pragma once
#include <QDialog>

namespace gitbolt::views {

class CloneDialog : public QDialog {
    Q_OBJECT
public:
    explicit CloneDialog(QWidget* parent = nullptr);
    QString url() const;
    QString path() const;
};

} // namespace gitbolt::views
