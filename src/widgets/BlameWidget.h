#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class BlameWidget : public QWidget {
    Q_OBJECT
public:
    explicit BlameWidget(QWidget* parent = nullptr);

};

} // namespace gitbolt::widgets
