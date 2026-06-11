#pragma once

#include <QObject>
#include <QString>

class QLocalServer;
class QLocalSocket;
class QAction;

namespace gitbolt::ui {
class MainWindow;
}

namespace gitbolt::app {

/// Environment-gated control channel for driving GitBolt from test
/// harnesses and AI agents — see docs/AGENT_TESTING.md for the
/// friction log that motivated it. Only constructed when
/// GITBOLT_TEST_BRIDGE is set; never active in a normal launch.
///
/// Listens on a QLocalServer (unix domain socket under the user's
/// temp dir, name "gitbolt-test-bridge" or the env value when it
/// isn't "1"). Protocol: newline-delimited commands in, one compact
/// JSON object per line out.
///
///   list-actions                     menu/toolbar actions + slugs
///   trigger <slug-or-objectName>     queue a QAction::trigger
///   list-widgets                     top-level windows + the active
///                                    window's buttons/views/fields
///   click <objectName-or-text>       queue a QAbstractButton::click
///   select-row <viewSpec> <row>      drive a view's selection model
///                                    (viewSpec = objectName or
///                                    ClassName[:index] among visible)
///   type <widgetSpec> <text…>        focus + set a line/text edit
///   dump-state                       repo path/state/branch,
///                                    conflict count, windows
///   screenshot <path>                grab() active window to PNG
///
/// trigger/click respond {"ok":true,"dispatched":true} BEFORE the
/// action runs (queued invocation): a triggered action may open a
/// modal dialog whose exec() would otherwise hold the reply hostage.
/// Assert outcomes with dump-state / on-disk git, not the reply.
class TestBridge : public QObject {
    Q_OBJECT
public:
    explicit TestBridge(ui::MainWindow* window, QObject* parent = nullptr);

    /// Menu-path slug for an action ("commands/resolve-conflicts").
    /// Lets agents address the ~50 inline QActions that have no
    /// objectName without hand-naming every call site. Public so
    /// the file-local action walker can use it.
    static QString slugify(const QString& text);

private:
    void onNewConnection();
    QByteArray handleLine(const QString& line);

    QByteArray cmdListActions();
    QByteArray cmdTrigger(const QString& spec);
    QByteArray cmdListWidgets();
    QByteArray cmdClick(const QString& spec);
    QByteArray cmdSelectRow(const QString& viewSpec, int row);
    QByteArray cmdType(const QString& widgetSpec, const QString& text);
    QByteArray cmdDumpState();
    QByteArray cmdScreenshot(const QString& path);

    ui::MainWindow* window_ = nullptr;
    QLocalServer* server_ = nullptr;
};

} // namespace gitbolt::app
