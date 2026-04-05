#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class DiffViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit DiffViewerWidget(QWidget* parent = nullptr);
    void setDiff(const gitbolt::core::DiffResult& diff, int fileIndex = 0);
    void clear();

};

} // namespace gitbolt::widgets
