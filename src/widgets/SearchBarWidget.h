#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class SearchBarWidget : public QWidget {
    Q_OBJECT
public:
    explicit SearchBarWidget(QWidget* parent = nullptr);
signals:
    void searchRequested(const QString& query, const QString& type);

};

} // namespace gitbolt::widgets
