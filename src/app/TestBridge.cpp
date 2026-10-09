#include "TestBridge.h"

#include "conf/SettingsService.h"
#include "models/CommitLogModel.h"
#include "services/GitService.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTimer>
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

// The protocol is one command per line, so a `type` argument can never
// contain a raw newline. Decoding \n \t \\ here is the only way a
// multi-line commit message fits through the channel.
QString unescapeTypeText(const QString& in)
{
    QString out;
    out.reserve(in.size());
    for (qsizetype i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == QLatin1Char('\\') && i + 1 < in.size()) {
            const QChar next = in.at(i + 1);
            if (next == QLatin1Char('n')) {
                out += QLatin1Char('\n');
                ++i;
                continue;
            }
            if (next == QLatin1Char('t')) {
                out += QLatin1Char('\t');
                ++i;
                continue;
            }
            if (next == QLatin1Char('\\')) {
                out += QLatin1Char('\\');
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
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
// window: the modal dialog's first, then the active window's, then
// any other dialog's, then the rest. `Class[:index]` addressing in
// select-row/select-item/type resolves against this ordering, so ":0"
// means "the one in the dialog the agent just opened".
template <typename T>
QList<T*> visibleWidgets()
{
    QList<T*> out;
    QWidgetList tops = QApplication::topLevelWidgets();
    // Not just isActiveWindow(): while GitBolt isn't the frontmost app
    // (an agent driving it from a terminal) no window is active, and
    // topLevelWidgets() has no stable order. "QComboBox:0" then
    // sometimes meant the toolbar's branch switcher, where select-item
    // checks the branch out, and the dialog's OK confirmed its default
    // item instead of the one the agent asked for.
    QWidget* const modal = QApplication::activeModalWidget();
    const auto rank = [modal](QWidget* w) {
        if (w == modal)
            return 0;
        if (w->isActiveWindow())
            return 1;
        return qobject_cast<QDialog*>(w) ? 2 : 3;
    };
    std::stable_sort(tops.begin(), tops.end(),
                     [&rank](QWidget* a, QWidget* b) {
                         return rank(a) < rank(b);
                     });
    for (QWidget* top : tops) {
        if (!top->isVisible())
            continue;
        const auto found = top->findChildren<T*>();
        for (T* w : found) {
            // A dialog parented to the main window is both a top-level
            // and the main window's descendant, so without the window()
            // check its widgets were listed twice and `Class:N` indices
            // skipped over the duplicates.
            if (w->isVisible() && w->window() == top)
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
    const qsizetype colon = spec.lastIndexOf(QLatin1Char(':'));
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
    // macOS puts the socket in the per-user $TMPDIR (mode 700), but
    // on Linux it lands in world-traversable /tmp with umask-derived
    // permissions — under a group-writable umask any same-group user
    // could drive the GUI. Restrict to the owning user everywhere.
    server_->setSocketOptions(QLocalServer::UserAccessOption);
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
    if (verb == QStringLiteral("select-row") && parts.size() >= 3) {
        // toInt() without the ok-flag yields 0 on garbage — which
        // silently selected row 0 instead of erroring. An agent-
        // facing surface must fail loudly.
        QList<int> rows;
        const QStringList rowParts =
            parts.at(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& p : rowParts) {
            bool okRow = false;
            const int row = p.toInt(&okRow);
            if (!okRow)
                return errLine(
                    QStringLiteral("select-row: not an integer: ") + p);
            rows.append(row);
        }
        if (rows.isEmpty())
            return errLine(QStringLiteral("select-row: no rows given"));
        return cmdSelectRow(parts.at(1), rows);
    }
    if (verb == QStringLiteral("select-item") && parts.size() >= 3)
        // section() keeps the remainder, so item texts with spaces work.
        return cmdSelectItem(parts.at(1), line.section(QLatin1Char(' '), 2));
    if (verb == QStringLiteral("type") && parts.size() >= 3)
        return cmdType(parts.at(1),
                       unescapeTypeText(line.section(QLatin1Char(' '), 2)));
    if (verb == QStringLiteral("fire-timer") && parts.size() >= 2)
        return cmdFireTimer(parts.at(1));
    if (verb == QStringLiteral("dump-state"))
        return cmdDumpState();
    if (verb == QStringLiteral("quit"))
        return cmdQuit();
    if (verb == QStringLiteral("screenshot") && parts.size() >= 2)
        // section() keeps the remainder intact so paths containing
        // spaces aren't silently truncated at the first one.
        return cmdScreenshot(line.section(QLatin1Char(' '), 1));

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

    // Combo boxes with their entries, so an agent can see what
    // `select-item` can pick without guessing at item texts.
    QJsonArray combos;
    int comboIndex = 0;
    for (QComboBox* c : visibleWidgets<QComboBox>()) {
        QJsonObject o;
        o[QStringLiteral("class")] = QString::fromLatin1(
                                         c->metaObject()->className())
                                         .section(QStringLiteral("::"), -1);
        o[QStringLiteral("index")] = comboIndex++;
        if (!c->objectName().isEmpty())
            o[QStringLiteral("objectName")] = c->objectName();
        o[QStringLiteral("current")] = c->currentText();
        o[QStringLiteral("enabled")] = c->isEnabled();
        QJsonArray items;
        for (int i = 0; i < c->count(); ++i)
            items.append(c->itemText(i));
        o[QStringLiteral("items")] = items;
        combos.append(o);
    }

    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("windows"), windows},
                     {QStringLiteral("buttons"), buttons},
                     {QStringLiteral("views"), views},
                     {QStringLiteral("editors"), editors},
                     {QStringLiteral("combos"), combos}});
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

QByteArray TestBridge::cmdSelectRow(const QString& viewSpec,
                                    const QList<int>& rows)
{
    auto* view = resolveWidget<QAbstractItemView>(viewSpec);
    if (!view)
        return errLine(QStringLiteral("no visible view matches: ")
                       + viewSpec);
    auto* model = view->model();
    if (!model)
        return errLine(QStringLiteral("view has no model"));
    const int rowCount = model->rowCount(view->rootIndex());
    for (int row : rows) {
        if (row < 0 || row >= rowCount)
            return errLine(QStringLiteral("row %1 out of range (%2 rows)")
                               .arg(row)
                               .arg(rowCount));
    }

    // In-process selection — this is the operation that no amount
    // of synthetic mouse/AX input could reach (friction log #4).
    // Multiple rows ("0,2,5") cover the ExtendedSelection staging
    // lists, where Stage/Unstage act on everything selected.
    QItemSelection selection;
    for (int row : rows) {
        const QModelIndex idx = model->index(row, 0, view->rootIndex());
        selection.select(idx, idx);
    }
    view->setCurrentIndex(model->index(rows.first(), 0, view->rootIndex()));
    view->selectionModel()->select(
        selection, QItemSelectionModel::ClearAndSelect
                       | QItemSelectionModel::Rows);

    return jsonLine(
        {{QStringLiteral("ok"), true},
         {QStringLiteral("selectedRows"),
          view->selectionModel()->selectedRows().count()}});
}

QByteArray TestBridge::cmdSelectItem(const QString& comboSpec,
                                     const QString& item)
{
    auto* combo = resolveWidget<QComboBox>(comboSpec);
    if (!combo)
        return errLine(QStringLiteral("no visible combo box matches: ")
                       + comboSpec);

    // "#N" picks by index; anything else must match an entry's text
    // exactly. A fuzzy match could quietly check out the wrong branch.
    int index = -1;
    if (item.startsWith(QLatin1Char('#'))) {
        bool okNum = false;
        index = item.mid(1).toInt(&okNum);
        if (!okNum || index < 0 || index >= combo->count())
            return errLine(QStringLiteral("select-item: no index ") + item);
    } else {
        index = combo->findText(item, Qt::MatchExactly);
        if (index < 0) {
            QStringList have;
            for (int i = 0; i < combo->count(); ++i)
                have.append(combo->itemText(i));
            return errLine(QStringLiteral("select-item: no item \"%1\"; have: %2")
                               .arg(item, have.join(QStringLiteral(", "))));
        }
    }

    combo->setCurrentIndex(index);
    // A user's pick also emits activated/textActivated, and code that
    // must react only to real choices listens for those, not for
    // setCurrentIndex (the toolbar branch switcher checks out on
    // `activated`). Queued like trigger/click so a modal the handler
    // opens can't hold the reply hostage.
    QPointer<QComboBox> guard(combo);
    QMetaObject::invokeMethod(
        combo,
        [guard, index]() {
            if (!guard)
                return;
            emit guard->activated(index);
            emit guard->textActivated(guard->itemText(index));
        },
        Qt::QueuedConnection);

    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("dispatched"), true},
                     {QStringLiteral("index"), index},
                     {QStringLiteral("text"), combo->itemText(index)}});
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

QByteArray TestBridge::cmdFireTimer(const QString& name)
{
    // Periodic work (Plugins → Periodic background fetch) fires every
    // few minutes at best; a test can't wait that out, nor shorten an
    // interval the UI only takes in whole minutes.
    auto* timer = window_->findChild<QTimer*>(name);
    if (!timer)
        return errLine(QStringLiteral("no timer named: ") + name);
    // A stopped timer never fires: firing it anyway would test a state
    // the app can't be in (the feature switched off).
    if (!timer->isActive())
        return errLine(QStringLiteral("timer not running: ") + name);

    // timeout() is a private signal, emitted here through the meta-
    // object system. Queued like trigger: "ok" means dispatched.
    QMetaObject::invokeMethod(timer, "timeout", Qt::QueuedConnection);
    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("dispatched"), true}});
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
        // headOid lets a harness assert "a commit happened" / "HEAD
        // moved" without shelling out to git.
        if (auto head = repo->head())
            state[QStringLiteral("headOid")] =
                QString::fromStdString(head.value().toHex());
    }

    if (auto* settings = window_->findChild<conf::SettingsService*>())
        state[QStringLiteral("theme")] = settings->theme();
    // The *effective* palette, not the stored preference: a startup-
    // ordering bug once left the INI saying Dark while the window
    // rendered light, and only this distinction can catch that.
    state[QStringLiteral("paletteWindow")] =
        QApplication::palette().color(QPalette::Window).name();
    if (auto* logModel = window_->findChild<models::CommitLogModel*>())
        state[QStringLiteral("logRows")] = logModel->rowCount();
    // Where op outcomes land ("Push complete.", "fetch failed: …").
    state[QStringLiteral("statusMessage")] =
        window_->statusBar()->currentMessage();

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

QByteArray TestBridge::cmdQuit()
{
    // close(), not qApp->quit(): closeEvent must run so geometry and
    // session state persist — relaunch-and-verify tests depend on the
    // exit being indistinguishable from a user closing the window.
    QMetaObject::invokeMethod(
        window_, [w = window_]() { w->close(); }, Qt::QueuedConnection);
    return jsonLine({{QStringLiteral("ok"), true},
                     {QStringLiteral("dispatched"), true}});
}

} // namespace gitbolt::app
