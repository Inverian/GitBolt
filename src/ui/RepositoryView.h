#pragma once
#include <QWidget>

namespace gitbolt::ui {

class RepositoryView : public QWidget {
    Q_OBJECT
public:
    explicit RepositoryView(QWidget* parent = nullptr);
};

} // namespace gitbolt::ui
