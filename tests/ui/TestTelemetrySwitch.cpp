//
// TestTelemetrySwitch — the Settings switch reaches usage statistics
// through the real MainWindow wiring, against a loopback HTTP stub
// (../telemetry/HttpStub.h), never a real endpoint.
//
// "Off stops sending at once and deletes the queue" only holds because
// MainWindow listens to its OWN SettingsService, the one the Settings
// dialog writes through: settingsChanged is emitted per instance, so a
// service wired to main()'s instance would never hear the switch. The
// telemetry unit tests call setUserEnabled() directly and e2e sessions
// are inert, so only this test would catch that regression:
//
//   - off, applied the way the dialog applies it (a batch on the
//     window's SettingsService): the queued record's file is gone, and
//     nothing reaches the stub, not even a counted action;
//   - on again: menu actions are counted by objectName and sent.
//

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "../telemetry/HttpStub.h"
#include "conf/SettingsService.h"
#include "telemetry/EventQueue.h"
#include "telemetry/TelemetryConfig.h"
#include "telemetry/TelemetryGate.h"
#include "telemetry/TelemetryService.h"
#include "ui/MainWindow.h"

using namespace gitbolt::telemetry;
using gitbolt::conf::SettingsService;
using gitbolt::test::HttpStub;
using gitbolt::test::StubRequest;

namespace {

const QString kInstallId = QStringLiteral("1b4e28ba-2fa1-4d2c-883f-0016d3cca427");

// How many triggers of `command` the stub's command_used events add
// up to.
int commandCount(const HttpStub& stub, const QString& command) {
    int n = 0;
    for (const StubRequest& request : stub.requests()) {
        const QJsonObject payload = request.json().value(QStringLiteral("payload")).toObject();
        const QJsonObject properties = payload.value(QStringLiteral("properties")).toObject();
        if (payload.value(QStringLiteral("name")).toString() == QLatin1String("command_used")
            && properties.value(QStringLiteral("command")).toString() == command)
            n += properties.value(QStringLiteral("count")).toInt();
    }
    return n;
}

} // namespace

class TestTelemetrySwitch : public QObject {
    Q_OBJECT

private slots:
    // GitBolt.ini and the telemetry queue in folders of the test's own.
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(dataDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void switchOffDiscardsAndStopsThenOnResumes() {
        HttpStub stub;
        QVERIFY(stub.isListening());
        Config config = Config::fromEndpoints(
            stub.origin(), QStringLiteral("test-client"), QStringLiteral("test-key-value"),
            QStringLiteral("0123456789abcdef0123456789abcdef"), QStringLiteral("42"),
            QStringLiteral("0.1.0"), dataDir_.filePath(QStringLiteral("telemetry")));
        config.paceMs = 50;
        config.transferTimeoutMs = 300;
        config.firstFlushDelayMs = 0;
        config.countWindowMs = 200;
        QVERIFY(config.isComplete());

        // As in main(): the service is declared first and outlives the
        // window, which keeps a raw pointer to it.
        TelemetryService service(config, [] { return kInstallId; }, true);
        gitbolt::ui::MainWindow window;
        window.setTelemetry(&service, Gate::Active);
        QCOMPARE(window.telemetryGate(), Gate::Active);
        auto* home = window.findChild<QAction*>(QStringLiteral("file.home"));
        QVERIFY(home);

        // One record queued and on disk, not sent: start() hasn't run.
        service.recordLaunch();
        QCOMPARE(service.queuedCount(), 1);
        const QString queueFile = EventQueue::filePath(config.queueDir);
        QVERIFY(QFile::exists(queueFile));

        // Off, as the Settings dialog's Apply does it: one batch on the
        // window's own SettingsService (the one findChild finds, as the
        // test bridge does).
        auto* settings = window.findChild<SettingsService*>();
        QVERIFY(settings);
        settings->beginUpdate();
        settings->setUsageStatisticsEnabled(false);
        QVERIFY(QFile::exists(queueFile));   // nothing until the batch closes
        settings->endUpdate();
        QVERIFY(!service.isActive());
        QVERIFY(!QFile::exists(queueFile));

        // Nothing reaches the stub: not the discarded record, not an
        // action triggered while off.
        service.start();
        home->trigger();
        QTest::qWait(500);
        QCOMPARE(stub.requests().size(), 0);

        // On again: sending resumes, and the window counts its actions
        // by objectName.
        settings->beginUpdate();
        settings->setUsageStatisticsEnabled(true);
        settings->endUpdate();
        QVERIFY(service.isActive());
        home->trigger();
        home->trigger();
        QTRY_COMPARE(commandCount(stub, QStringLiteral("file.home")), 2);
        for (const StubRequest& request : stub.requests())
            QCOMPARE(request.path, QByteArray("/track"));
        QVERIFY2(stub.headerViolations().isEmpty(),
                 qPrintable(stub.headerViolations().join(QLatin1Char('\n'))));
    }

private:
    QTemporaryDir settingsDir_;
    QTemporaryDir dataDir_;
};

QTEST_MAIN(TestTelemetrySwitch)
#include "TestTelemetrySwitch.moc"
