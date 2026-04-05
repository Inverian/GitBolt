#pragma once

#include <QWidget>

class QCheckBox;
class QPlainTextEdit;
class QPushButton;
class QShortcut;

namespace gitbolt::widgets {

/// A plain-text editor for composing commit messages, with Commit / Commit &
/// Push buttons and an optional Amend checkbox.
class CommitMessageEdit;   // forward declaration of inner helper

class CommitEditorWidget : public QWidget {
    Q_OBJECT

public:
    explicit CommitEditorWidget(QWidget* parent = nullptr);

    /// Current commit message text.
    QString message() const;

    /// Load a message (e.g. for amend -- loads previous commit message).
    void setMessage(const QString& msg);

    /// Clear the message editor after a successful commit.
    void clear();

    /// Whether the Amend checkbox is checked.
    bool isAmend() const;

signals:
    /// User pressed Commit (or Ctrl+Enter).
    void commitRequested(const QString& message, bool amend);

    /// User pressed Commit & Push.
    void commitAndPushRequested(const QString& message);

private slots:
    void onCommitClicked();
    void onCommitAndPushClicked();

private:
    void setupUi();

    CommitMessageEdit* editor_      = nullptr;
    QCheckBox*         amendCheck_  = nullptr;
    QPushButton*       commitBtn_   = nullptr;
    QPushButton*       commitPushBtn_ = nullptr;
    QShortcut*         commitShortcut_ = nullptr;
};

// ---------------------------------------------------------------------------
// CommitMessageEdit -- a QPlainTextEdit that paints a 72-char guide line
// ---------------------------------------------------------------------------

class CommitMessageEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CommitMessageEdit(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
};

} // namespace gitbolt::widgets
