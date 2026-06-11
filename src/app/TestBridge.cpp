#include "TestBridge.h"

#include "services/GitService.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QToolBar>
#include <QWindow>

#include <functional>
#include <mutex>

namespace gitbolt::app {

namespace {

QByteArray jsonLine(const QJsonObject& obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
}

QByteArray okLine()
{
    return jsonLine({{QStringLiteral("ok"), true}});
}

QByteArray errLine(const QString& message)
{
    return jsonLine({{QStringLiteral("ok"), false},
                     {QStringLiteral("error"), message}});
}

// Strip mnemonic ampersands and trailing ellipses from user-visible
// text so "Resolve co&nflicts..." and "Resolve conflicts…" both
// normalize to "resolve conflicts".
QString cleanText(QString t)
{
    t.remove(QLatin1Char('&'));
    t.remove(QStringLiteral("..."));
    t.remove(QStringLiteral("…"));
    return t.trimmed();
}

// Walk the menu bar (recursing into submenus) and every toolbar,
// calling `fn(slugPath, action)` for each action. Slugs look like
// "commands/resolve-conflicts" or "toolbar/commit".
void forEachAction(
    ui::MainWindow* window,
    const std::function<void(const QString&, QAction*)>& fn)
{
    std::function<void(QMenu*, const QString&)> walkMenu =
        [&](QMenu* menu, const QString& prefix) {
            for (QAction* a : menu->actions()) {
                if (a->isSeparator())
                    continue;
                if (QMenu* sub = a->menu()) {
                    walkMenu(sub,
                             prefix + QLatin1Char('/')
                                 + TestBridge::slugify(sub->title()));
                } else {
                    fn(prefix + QLatin1Char('/')
                           + TestBridge::slugify(a->text()),
                       a);
                }
            }
        };

    if (auto* bar = window->menuBar()) {
        for (QAction* top : bar->actions()) {
            if (QMenu* menu = top->menu())
                walkMenu(menu, TestBridge::slugify(menu->title()));
        }
    }
    const auto toolbars = window->findChildren<QToolBar*>();
    for (QToolBar* tb : toolbars) {
        for (QAction* a : tb->actions()) {
            // Skip separators, submenu openers, and widget-holder
            // actions (addWidget wraps a QLabel/combo in an action
            // with empty text — not something to "trigger").
            if (a->isSeparator() || a->menu() || a->text().isEmpty())
                continue;
            fn(QStringLiteral("toolbar/") + TestBridge::slugify(a->text()),
               a);
        }
    }
}

// Visible item views / buttons / editors of every visible top-level
// window, active window's children first. `Class[:index]` addressing
// in select-row/type resolves against this ordering, so it is stable
// for the duration of a dialog being frontmost.
template <typename T>
QList<T*> visibleWidgets()
{
    QList<T*> out;
    QWidgetList tops = QApplication::topLevelWidgets();
    // Active window's widgets first so ":0" usually means "the one
    // in the dialog the agent just opened".
    std::stable_sort(tops.begin(), tops.end(),
                     [](QWidget* a, QWidget* b) {
                         const bool aa = a->isActiveWindow();
                         const bool bb = b->isActiveWindow();
                         return aa && !bb;
                     });
    for (QWidget* top : tops) {
        if (!top->isVisible())
            continue;
        const auto found = top->findChildren<T*>();
        for (T* w : found) {
            if (w->isVisible())
                out.append(w);
        }
    }
    return out;
}

// Resolve "objectName" or "ClassName[:index]" against the visible
// widgets of type T.
template <typename T>
T* resolveWidget(const QString& spec)
{
    QString className = spec;
    int index = 0;
    const int colon = spec.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool okNum = false;
        const int n = spec.mid(colon + 1).toInt(&okNum);
        if (okNum) {
            className = spec.left(colon);
            index = n;
        }
    }

    int seen = 0;
    const auto widgets = visibleWidgets<T>();
    for (T* w : widgets) {
        if (!w->objectName().isEmpty() && w->objectName() == spec)
            return w;
        // Match against the whole superclass chain, not just the leaf
        // class, so "QPlainTextEdit:0" resolves a CommitMessageEdit /
        // DiffTextEdit (both QPlainTextEdit subclasses) — agents
        // address by the well-known base class, not internal names.
        bool classMatch = false;
        for (const QMetaObject* mo = w->metaObject(); mo;
             mo = mo->superClass()) {
            if (QString::fromLatin1(mo->className())
                    .section(QStringLiteral("::"), -1)
                == className) {
                classMatch = true;
                break;
            }
        }
        if (classMatch) {
            if (seen == index)
                return w;
            ++seen;
        }
    }
    return nullptr;
}

} // namespace

QString TestBridge::slugify(const QString& text)
{
    QString s = cleanText(text).toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
              QStringLiteral("-"));
    while (s.startsWith(QLatin1Char('-')))
        s.remove(0, 1);
    while (s.endsWith(QLatin1Char('-')))
        s.chop(1);
    return s;
}

TestBridge::TestBridge(ui::MainWindow* window, QObject* parent)
    : QObject(parent), window_(window)
{
    QString name = qEnvironmentVariable("GITBOLT_TEST_BRIDGE");
    if (name.isEmpty() || name == QStringLiteral("1"))
        name = QStringLiteral("gitbolt-test-bridge");

    server_ = new QLocalServer(this);
    QLocalServer::removeServer(name);   // clear a stale socket file
    if (!server_->listen(name)) {
        qWarning("TestBridge: listen(%s) failed: %s",
                 qPrintable(name),
                 qPrintable(server_->errorString()));
        return;
    }
    qInfo("TestBridge: listening on %s",
          qPrintable(server_->fullServerName()));
    connect(server_, &QLocalServer::newConnection,
            this, &TestBridge::onNewConnection);
}

void TestBridge::onNewConnection()
{
    while (QLocalSocket* sock = server_->nextPendingConnection()) {
        connect(sock, &QLocalSocket::disconnected,
                sock, &QLocalSocket::deleteLater);
        connect(sock, &QLocalSocket::readyRead, this, [this, sock]() {
            while (sock->canReadLine()) {
                const QString line =
                    QString::fromUtf8(sock->readLine()).trimmed();
                if (line.isEmpty())
                    continue;
                sock->write(handleLine(line));
                sock->flush();
            }
        });
    }
}

QByteArray TestBridge::handleLine(const QString& line)
{
    const QStringList parts = line.split(QLatin1Char(' '),
                                         Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return errLine(QStringLiteral("empty command"));
    const QString verb = parts.first();

    if (verb == QStringLiteral("list-actions"))
        return cmdListActions();
    if (verb == QStringLiteral("trigger") && parts.size() >= 2)
        return cmdTrigger(parts.at(1));
    if (verb == QStringLiteral("list-widgets"))
        return cmdListWidgets();
    if (verb == QStringLiteral("click") && parts.size() >= 2)
        return cmdClick(line.section(QLatin1Char(' '), 1));
    if (verb == QStringLiteral("select-row") && parts.size() >= 3)
        return cmdSelectRow(parts.at(1), parts.at(2).toInt());
    if (verb == QStringLiteral("type") && parts.size() >= 3)
        return cmdType(parts.at(1), line.section(QLatin1Char(' '), 2));
    if (verb == QStringLiteral("dump-state"))
        return cmdDumpState();
    if (verb == QStringLiteral("screenshot") && parts.size() >= 2)
        return cmdScreenshot(parts.at(1));

    return errLine(QStringLiteral("unknown command: ") + verb);
}

QByteArray TestBridge::cmdListActions()
{
    QJsonArray actions;
    forEachAction(window_, [&actions](const QString& slug, QAction* a) {
        QJsonObject o;
        o[QStringLiteral("slug")] = slug;
        o[QStringLiteral("text")] = cleanText(a->text());
        if (!a->objectName().isEmpty())
            o[QStringLiteral("objectName")] = a->objectName();
        o[QStringLiteral("enabled")] = a->isEnabled();
        if (a->isCheckable())
            o[QStringLiteral("checked")] = a->isChecked();
        actions.append(o);
    });
    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("actions"), actions}});
}

QByteArray TestBridge::cmdTrigger(const QString& spec)
{
    QAction* match = nullptr;
    forEachAction(window_, [&](const QString& slug, QAction* a) {
        if (match)
            return;
        if (slug == spec || a->objectName() == spec)
            match = a;
    });
    // Fall back to suffix matching ("resolve-conflicts" without the
    // menu prefix) so agents don't need the exact menu path.
    if (!match) {
        forEachAction(window_, [&](const QString& slug, QAction* a) {
            if (!match && slug.endsWith(QLatin1Char('/') + spec))
                match = a;
        });
    }
    if (!match)
        return errLine(QStringLiteral("no action matches: ") + spec);
    if (!match->isEnabled())
        return errLine(QStringLiteral("action disabled: ") + spec);

    // Queued: a trigger that opens a modal dialog (exec) must not
    // hold this reply hostage. "ok" means dispatched, not finished.
    QMetaObject::invokeMethod(
        match, [match]() { match->trigger(); }, Qt::QueuedConnection);
    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("dispatched"), true}});
}

QByteArray TestBridge::cmdListWidgets()
{
    QJsonArray windows;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* top : tops) {
        if (!top->isVisible())
            continue;
        QJsonObject w;
        w[QStringLiteral("class")] = QString::fromLatin1(
            top->metaObject()->className());
        w[QStringLiteral("title")] = top->windowTitle();
        if (!top->objectName().isEmpty())
            w[QStringLiteral("objectName")] = top->objectName();
        w[QStringLiteral("active")] = top->isActiveWindow();
        windows.append(w);
    }

    QJsonArray buttons;
    const auto btns = visibleWidgets<QAbstractButton>();
    for (QAbstractButton* b : btns) {
        QJsonObject o;
        o[QStringLiteral("text")] = cleanText(b->text());
        if (!b->objectName().isEmpty())
            o[QStringLiteral("objectName")] = b->objectName();
        o[QStringLiteral("enabled")] = b->isEnabled();
        buttons.append(o);
    }

    QJsonArray views;
    const auto vws = visibleWidgets<QAbstractItemView>();
    for (QAbstractItemView* v : vws) {
        QJsonObject o;
        o[QStringLiteral("class")] = QString::fromLatin1(
                                         v->metaObject()->className())
                                         .section(QStringLiteral("::"), -1);
        if (!v->objectName().isEmpty())
            o[QStringLiteral("objectName")] = v->objectName();
        o[QStringLiteral("rows")] =
            v->model() ? v->model()->rowCount(v->rootIndex()) : 0;
        views.append(o);
    }

    // Text editors agents can drive with `type`. The class name is
    // the LEAF (e.g. CommitMessageEdit), but `type`/select address
    // by any superclass too — see resolveWidget — so an agent can
    // use the reported leaf name or a base like QPlainTextEdit.
    QJsonArray editors;
    auto addEditors = [&editors](const QObjectList& list,
                                 const char* base) {
        int i = 0;
        for (QObject* o : list) {
            auto* w = qobject_cast<QWidget*>(o);
            QJsonObject e;
            e[QStringLiteral("base")] = QString::fromLatin1(base);
            e[QStringLiteral("class")] =
                QString::fromLatin1(o->metaObject()->className())
                    .section(QStringLiteral("::"), -1);
            e[QStringLiteral("index")] = i++;
            if (w && !w->objectName().isEmpty())
                e[QStringLiteral("objectName")] = w->objectName();
            editors.append(e);
        }
    };
    QObjectList lineList;
    for (auto* w : visibleWidgets<QLineEdit>()) lineList.append(w);
    QObjectList plainList;
    for (auto* w : visibleWidgets<QPlainTextEdit>()) plainList.append(w);
    addEditors(lineList, "QLineEdit");
    addEditors(plainList, "QPlainTextEdit");

    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("windows"), windows},
                     {QStringLiteral("buttons"), buttons},
                     {QStringLiteral("views"), views},
                     {QStringLiteral("editors"), editors}});
}

QByteArray TestBridge::cmdClick(const QString& spec)
{
    const QString wanted = cleanText(spec);
    const auto btns = visibleWidgets<QAbstractButton>();
    QAbstractButton* match = nullptr;
    // Exact objectName / text first.
    for (QAbstractButton* b : btns) {
        if ((!b->objectName().isEmpty() && b->objectName() == spec)
            || cleanText(b->text()).compare(wanted, Qt::CaseInsensitive)
                   == 0) {
            match = b;
            break;
        }
    }
    // Prefix fallback so live-count labels like "Commit (1)" match a
    // plain "Commit" query without the agent tracking the count.
    if (!match) {
        for (QAbstractButton* b : btns) {
            if (cleanText(b->text()).startsWith(wanted,
                                                Qt::CaseInsensitive)) {
                match = b;
                break;
            }
        }
    }
    if (!match)
        return errLine(QStringLiteral("no visible button matches: ")
                       + spec);
    if (!match->isEnabled())
        return errLine(QStringLiteral("button disabled: ") + spec);

    QMetaObject::invokeMethod(
        match, [match]() { match->click(); }, Qt::QueuedConnection);
    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("dispatched"), true}});
}

QByteArray TestBridge::cmdSelectRow(const QString& viewSpec, int row)
{
    auto* view = resolveWidget<QAbstractItemView>(viewSpec);
    if (!view)
        return errLine(QStringLiteral("no visible view matches: ")
                       + viewSpec);
    auto* model = view->model();
    if (!model)
        return errLine(QStringLiteral("view has no model"));
    if (row < 0 || row >= model->rowCount(view->rootIndex()))
        return errLine(QStringLiteral("row out of range (%1 rows)")
                           .arg(model->rowCount(view->rootIndex())));

    // In-process selection — this is the operation that no amount
    // of synthetic mouse/AX input could reach (friction log #4).
    const QModelIndex idx = model->index(row, 0, view->rootIndex());
    view->setCurrentIndex(idx);
    view->selectionModel()->select(
        idx, QItemSelectionModel::ClearAndSelect
                 | QItemSelectionModel::Rows);

    return jsonLine(
        {{QStringLiteral("ok"), true},
         {QStringLiteral("selectedRows"),
          view->selectionModel()->selectedRows().count()}});
}

QByteArray TestBridge::cmdType(const QString& widgetSpec,
                               const QString& text)
{
    if (auto* line = resolveWidget<QLineEdit>(widgetSpec)) {
        line->setFocus();
        line->setText(text);   // fires textChanged, unlike AX setValue
        return okLine();
    }
    if (auto* edit = resolveWidget<QPlainTextEdit>(widgetSpec)) {
        edit->setFocus();
        edit->setPlainText(text);
        return okLine();
    }
    return errLine(QStringLiteral("no visible editor matches: ")
                   + widgetSpec);
}

QByteArray TestBridge::cmdDumpState()
{
    QJsonObject state;

    auto* svc = window_->findChild<services::GitService*>();
    state[QStringLiteral("repoOpen")] = (svc && svc->isOpen());
    if (svc && svc->isOpen()) {
        std::lock_guard<std::mutex> lock(svc->repoMutex());
        auto* repo = svc->repository();
        state[QStringLiteral("repoPath")] =
            QString::fromStdString(repo->workdir());
        switch (repo->state()) {
        case git::RepoState::None:
            state[QStringLiteral("repoState")] = QStringLiteral("none");
            break;
        case git::RepoState::Merge:
            state[QStringLiteral("repoState")] = QStringLiteral("merge");
            break;
        case git::RepoState::CherryPick:
            state[QStringLiteral("repoState")] =
                QStringLiteral("cherry-pick");
            break;
        case git::RepoState::Rebase:
            state[QStringLiteral("repoState")] = QStringLiteral("rebase");
            break;
        case git::RepoState::Revert:
            state[QStringLiteral("repoState")] = QStringLiteral("revert");
            break;
        default:
            state[QStringLiteral("repoState")] = QStringLiteral("other");
            break;
        }
        if (auto branch = repo->headBranchName())
            state[QStringLiteral("branch")] =
                QString::fromStdString(*branch);
        if (auto conflicts = repo->conflictEntries())
            state[QStringLiteral("conflictCount")] =
                static_cast<int>(conflicts->size());
    }

    QJsonArray windows;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* top : tops) {
        if (top->isVisible() && top->isWindow())
            windows.append(top->windowTitle());
    }
    state[QStringLiteral("windows")] = windows;
    state[QStringLiteral("ok")] = true;
    return jsonLine(state);
}

QByteArray TestBridge::cmdScreenshot(const QString& path)
{
    QWidget* target = QApplication::activeWindow();
    if (!target)
        target = window_;
    if (!target->grab().save(path))
        return errLine(QStringLiteral("could not save to ") + path);
    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("path"), path}});
}

} // namespace gitbolt::app
