#pragma once
#include <QWidget>

class QPlainTextEdit;

namespace gitbolt::widgets {

/// Read-only console output panel. Used in the bottom inspector
/// "Console" tab to surface git command output and errors. The
/// widget itself does no command execution — callers (typically
/// GitService or its consumers) push text in via the append API.
class ConsoleOutputWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConsoleOutputWidget(QWidget* parent = nullptr);

    void appendOutput(const QString& text);
    void appendError(const QString& text);
    void clear();

private:
    QPlainTextEdit* textEdit_ = nullptr;
};

} // namespace gitbolt::widgets
