#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class ConsoleOutputWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConsoleOutputWidget(QWidget* parent = nullptr);
    void appendOutput(const QString& text);
    void appendError(const QString& text);
    void clear();

};

} // namespace gitbolt::widgets
