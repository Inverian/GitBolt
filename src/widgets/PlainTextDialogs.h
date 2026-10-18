#pragma once

#include <QLineEdit>
#include <QMessageBox>
#include <QString>

class QInputDialog;
class QWidget;

namespace gitbolt::widgets {

/// QMessageBox's static information(), question() and warning(), with
/// the text shown as plain text. Use them for every box: nearly all of
/// them name a branch, a tag, a path or a submodule, or pass on what
/// git or a server said.
///
/// QMessageBox's own label is AutoText, and Qt::mightBeRichText() looks
/// for a tag anywhere in its first line, so a name such as
/// "<b>main</b>" or a remote's "fatal: remote error: <a href=…>" was
/// rendered: a confirmation could hide the real name behind another
/// one, and the label opens links (QMessageBox turns openExternalLinks
/// on), so one click on the text handed the server's URL to the
/// desktop. As plain text the box shows the characters it was given.
/// Its look is otherwise the same, and so is the question's outcome:
/// the buttons, their order and the default one are QMessageBox's.
class PlainMessageBox {
public:
    PlainMessageBox() = delete;

    static QMessageBox::StandardButton information(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
    static QMessageBox::StandardButton question(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::StandardButtons(
            QMessageBox::Yes | QMessageBox::No),
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
    static QMessageBox::StandardButton warning(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

private:
    static QMessageBox::StandardButton ask(
        QMessageBox::Icon icon, QWidget* parent, const QString& title,
        const QString& text, QMessageBox::StandardButtons buttons,
        QMessageBox::StandardButton defaultButton);
};

/// QInputDialog with its label shown as plain text, for a label that
/// names a branch or a remote, or quotes git (the askpass prompt). Its
/// label is AutoText too, with the same effect as a message box's
/// short of the links.
class PlainInputDialog {
public:
    PlainInputDialog() = delete;

    /// QInputDialog::getText().
    static QString getText(QWidget* parent, const QString& title, const QString& label,
                           QLineEdit::EchoMode mode = QLineEdit::Normal,
                           const QString& text = QString(), bool* ok = nullptr);

    /// dialog.setLabelText(text), shown as plain text.
    static void setLabelText(QInputDialog& dialog, const QString& text);
};

} // namespace gitbolt::widgets
