#pragma once
#include "models/CommitLogModel.h"
#include <QWidget>

namespace gitbolt::widgets {

class RevisionGraphWidget : public QWidget {
    Q_OBJECT
public:
    explicit RevisionGraphWidget(QWidget* parent = nullptr);
    void setModel(models::CommitLogModel* model);

};

} // namespace gitbolt::widgets
