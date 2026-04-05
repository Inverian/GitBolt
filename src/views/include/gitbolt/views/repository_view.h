#pragma once
#include <QWidget>

namespace gitbolt::views {

class RepositoryView : public QWidget {
    Q_OBJECT
public:
    explicit RepositoryView(QWidget* parent = nullptr);
};

} // namespace gitbolt::views
