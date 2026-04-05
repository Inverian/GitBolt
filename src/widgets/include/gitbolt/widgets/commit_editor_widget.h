#pragma once
#include <QWidget>

namespace gitbolt::widgets {

class CommitEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit CommitEditorWidget(QWidget* parent = nullptr);
    QString message() const;
    void setMessage(const QString& msg);
    void clear();
signals:
    void commitRequested(const QString& message);

};

} // namespace gitbolt::widgets
