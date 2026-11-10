#pragma once
//
// The items a MainWindow puts in the macOS application menu, caught as
// they join it, with their role then. That menu bar outlives each
// window, so searching it would also find earlier windows' items.
//

#include <QAction>
#include <QActionEvent>
#include <QApplication>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QString>

namespace gitbolt::test {

class AppMenuWatcher : public QObject {
public:
    struct Joined {
        QPointer<QAction> action;
        QString text;
        QAction::MenuRole role;
    };
    AppMenuWatcher() { qApp->installEventFilter(this); }
    ~AppMenuWatcher() override { qApp->removeEventFilter(this); }
    QAction* named(const QString& name) const {
        for (const Joined& item : joined) {
            if (item.action && item.action->objectName() == name)
                return item.action;
        }
        return nullptr;
    }
    QList<Joined> joined;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::ActionAdded) {
            auto* menu = qobject_cast<QMenu*>(watched);
            auto* bar = menu ? qobject_cast<QMenuBar*>(menu->parentWidget()) : nullptr;
            if (bar && !bar->parentWidget()) {
                QAction* action = static_cast<QActionEvent*>(event)->action();
                joined.append({action, action->text(), action->menuRole()});
            }
        }
        return false;
    }
};

} // namespace gitbolt::test
