#include "widgets/PlainTextDialogs.h"

#include <QInputDialog>
#include <QLabel>
#include <QPointer>
#include <QPushButton>

namespace gitbolt::widgets {

QMessageBox::StandardButton PlainMessageBox::information(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)
{
    return ask(QMessageBox::Information, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton PlainMessageBox::question(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)
{
    return ask(QMessageBox::Question, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton PlainMessageBox::warning(
    QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)
{
    return ask(QMessageBox::Warning, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton PlainMessageBox::ask(
    QMessageBox::Icon icon, QWidget* parent, const QString& title, const QString& text,
    QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)
{
    // Built as QMessageBox's static functions build theirs, on the
    // stack, with the format set before the text.
    QMessageBox box(icon, title, QString(), QMessageBox::NoButton, parent);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    // The buttons in the order of their values; the default one is
    // `defaultButton`, or else the first that accepts.
    for (auto mask = static_cast<uint>(QMessageBox::FirstButton);
         mask <= static_cast<uint>(QMessageBox::LastButton); mask <<= 1) {
        const auto which = static_cast<QMessageBox::StandardButton>(mask);
        if (!buttons.testFlag(which))
            continue;
        QPushButton* button = box.addButton(which);
        if (box.defaultButton())
            continue;
        if (defaultButton == QMessageBox::NoButton
                ? box.buttonRole(button) == QMessageBox::AcceptRole
                : which == defaultButton)
            box.setDefaultButton(button);
    }
    if (box.exec() == -1)
        return QMessageBox::Cancel;
    return box.standardButton(box.clickedButton());
}

QString PlainInputDialog::getText(QWidget* parent, const QString& title, const QString& label,
                                  QLineEdit::EchoMode mode, const QString& text, bool* ok)
{
    // On the heap, as QInputDialog::getText() has it: its parent can go
    // while it is up, and takes it along.
    QPointer<QInputDialog> dialog = new QInputDialog(parent);
    dialog->setWindowTitle(title);
    setLabelText(*dialog, label);
    dialog->setTextValue(text);
    dialog->setTextEchoMode(mode);
    const bool accepted = dialog->exec() == QDialog::Accepted && dialog;
    if (ok)
        *ok = accepted;
    const QString value = accepted ? dialog->textValue() : QString();
    delete dialog;
    return value;
}

void PlainInputDialog::setLabelText(QInputDialog& dialog, const QString& text)
{
    // The label is the input field's buddy once the dialog is laid
    // out, and a label with a buddy takes "&" for a mnemonic's mark,
    // plain text or not: "a&b" showed as "ab", the b underlined. A
    // branch name may have one, and so may the user name in the
    // askpass prompt's URL. Doubled, each shows as itself.
    dialog.setLabelText(QString(text).replace(QLatin1Char('&'), QStringLiteral("&&")));
    // The dialog's one label, which setLabelText() makes if need be.
    if (auto* label = dialog.findChild<QLabel*>(QString(), Qt::FindDirectChildrenOnly))
        label->setTextFormat(Qt::PlainText);
}

} // namespace gitbolt::widgets
