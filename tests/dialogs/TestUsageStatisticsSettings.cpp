//
// TestUsageStatisticsSettings — Settings › General › Usage statistics,
// and the SettingsService keys behind it:
//
//   - the switch defaults to on, persists, and an unchanged value
//     emits no settingsChanged; a reload sees another process turn it
//     off;
//   - the install ID is empty until telemetry asks for one, is then a
//     UUID v4 the telemetry library accepts, and stays the same;
//   - GitBolt.ini's path, which the telemetry queue sits next to,
//     follows the settings location (so test isolation covers it);
//   - the group is hidden unless MainWindow says the build has
//     telemetry; shown, it reflects the setting, Apply writes it once,
//     Cancel writes nothing, an environment lock disables the switch
//     and says why, and opening it never creates an install ID;
//   - Copy puts the install ID on the clipboard.
//
// Settings live in a folder of the test's own (QSettings::setPath),
// and Apply's git config writes go to a temporary global config: never
// the user's files.
//

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "conf/SettingsService.h"
#include "dialogs/SettingsDialog.h"
#include "telemetry/EventSchema.h"

using gitbolt::conf::SettingsService;
using gitbolt::dialogs::SettingsDialog;

namespace {

template <typename T>
T* child(const QWidget& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}

} // namespace

class TestUsageStatisticsSettings : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(homeDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        // The names main() sets, which SettingsService builds the
        // INI's path from.
        QCoreApplication::setOrganizationName(QStringLiteral("GitBolt"));
        QCoreApplication::setApplicationName(QStringLiteral("GitBolt"));
        // Apply runs `git config --global user.name …` with whatever
        // the Git Config page loaded: point it at a file of our own.
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(homeDir_.filePath(QStringLiteral("gitconfig"))));
        qputenv("HOME", QFile::encodeName(homeDir_.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(homeDir_.path()));
    }

    // Every test starts from an empty GitBolt.ini.
    void init() {
        SettingsService settings;
        QSettings(settings.fileName(), QSettings::IniFormat).clear();
    }

    // ---- SettingsService ------------------------------------------------

    void switchDefaultsOnAndPersists() {
        {
            SettingsService settings;
            QVERIFY(settings.usageStatisticsEnabled());
            QSignalSpy changed(&settings, &SettingsService::settingsChanged);
            settings.setUsageStatisticsEnabled(false);
            QCOMPARE(changed.count(), 1);
            // The same value again is no change.
            settings.setUsageStatisticsEnabled(false);
            QCOMPARE(changed.count(), 1);
        }
        SettingsService again;
        QVERIFY(!again.usageStatisticsEnabled());
    }

    // Another process turns the switch off: QSettings keeps serving
    // what it read until it syncs, so the telemetry service re-reads
    // through reloadUsageStatisticsEnabled(). Every SettingsService of
    // this process sees the change then (MainWindow's own passes the
    // switch on at each settingsChanged, so it must not stay on).
    void reloadSeesAnotherProcessesSwitch() {
        const QString ini = SettingsService().fileName();
        auto writeIni = [&ini](const QByteArray& content, int ageSecs) {
            QFile file(ini);
            // Its folder too: run on its own, no test before this one
            // has had QSettings write the INI (and make the folder).
            return QDir().mkpath(QFileInfo(ini).absolutePath())
                   && file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                   && file.write(content) == content.size() && file.flush()
                   && file.setFileTime(QDateTime::currentDateTime().addSecs(ageSecs),
                                       QFileDevice::FileModificationTime);
        };
        QVERIFY(writeIni("[telemetry]\nenabled=true\n", -10));
        SettingsService holder;
        SettingsService window;
        QVERIFY(holder.usageStatisticsEnabled());
        QVERIFY(window.usageStatisticsEnabled());

        // The other process's write: a new size and a later time.
        QVERIFY(writeIni("[telemetry]\nenabled=false\n", 0));
        QVERIFY(holder.usageStatisticsEnabled());   // what it read before
        QVERIFY(!holder.reloadUsageStatisticsEnabled());
        QVERIFY(!holder.usageStatisticsEnabled());
        QVERIFY(!window.usageStatisticsEnabled());
    }

    void installIdIsCreatedOnceAndOnlyOnRequest() {
        SettingsService settings;
        QSignalSpy changed(&settings, &SettingsService::settingsChanged);
        QVERIFY(settings.telemetryInstallId().isEmpty());

        const QString id = settings.ensureTelemetryInstallId();
        QVERIFY2(gitbolt::telemetry::isValidInstallId(id), qPrintable(id));
        QCOMPARE(settings.telemetryInstallId(), id);
        QCOMPARE(settings.ensureTelemetryInstallId(), id);
        QCOMPARE(changed.count(), 0);

        // Another instance (main()'s, MainWindow's) sees the same ID.
        SettingsService other;
        QCOMPARE(other.telemetryInstallId(), id);

        // Turning statistics off keeps it: it can still be quoted in a
        // deletion request.
        other.setUsageStatisticsEnabled(false);
        QCOMPARE(SettingsService().telemetryInstallId(), id);
    }

    void fileNameFollowsTheSettingsLocation() {
        SettingsService settings;
        const QFileInfo ini(settings.fileName());
        QCOMPARE(ini.absoluteFilePath(),
                 QFileInfo(settingsDir_.filePath(QStringLiteral("GitBolt/GitBolt.ini")))
                     .absoluteFilePath());
    }

    // ---- SettingsDialog -------------------------------------------------

    // Source builds and forks have no telemetry: nothing to show, and a
    // dialog nobody told about it looks as it always did.
    void groupHiddenUnlessAvailable() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        auto* group = child<QGroupBox>(dialog, "settings.usage-group");
        QVERIFY(group);
        QVERIFY(group->isHidden());

        dialog.setUsageStatistics(false, QString());
        QVERIFY(group->isHidden());

        dialog.setUsageStatistics(true, QString());
        QVERIFY(!group->isHidden());
    }

    void showsSettingAndIdWithoutCreatingOne() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUsageStatistics(true, QString());

        auto* check = child<QCheckBox>(dialog, "settings.usage-statistics");
        auto* id = child<QLineEdit>(dialog, "settings.install-id");
        auto* copy = child<QPushButton>(dialog, "settings.install-id-copy");
        auto* link = child<QLabel>(dialog, "settings.usage-privacy-link");
        auto* note = child<QLabel>(dialog, "settings.usage-locked-note");
        QVERIFY(check && id && copy && link && note);

        QVERIFY(check->isChecked());
        QVERIFY(check->isEnabled());
        QVERIFY(note->isHidden());
        QVERIFY(id->isReadOnly());
        QVERIFY(id->text().isEmpty());
        QVERIFY(!copy->isEnabled());
        QVERIFY(link->openExternalLinks());
        QVERIFY(link->text().contains(
            QStringLiteral("https://github.com/Inverian/GitBolt/blob/main/PRIVACY.md")));
        // Shown for deletion requests, never created by looking.
        QVERIFY(settings.telemetryInstallId().isEmpty());

        const QString created = settings.ensureTelemetryInstallId();
        SettingsDialog reopened(&settings, nullptr);
        reopened.setUsageStatistics(true, QString());
        QCOMPARE(child<QLineEdit>(reopened, "settings.install-id")->text(), created);
        QVERIFY(child<QPushButton>(reopened, "settings.install-id-copy")->isEnabled());
    }

    void applyWritesTheSwitchOnce() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUsageStatistics(true, QString());
        QSignalSpy changed(&settings, &SettingsService::settingsChanged);

        child<QCheckBox>(dialog, "settings.usage-statistics")->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "apply"));
        QVERIFY(!settings.usageStatisticsEnabled());
        // One batch, one signal: MainWindow hands it to telemetry.
        QCOMPARE(changed.count(), 1);
    }

    void cancelWritesNothing() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUsageStatistics(true, QString());
        child<QCheckBox>(dialog, "settings.usage-statistics")->setChecked(false);
        dialog.reject();
        QVERIFY(settings.usageStatisticsEnabled());
    }

    // GITBOLT_TELEMETRY turned statistics off for this session: the
    // switch can't say otherwise, and Apply leaves the stored
    // preference alone.
    void environmentLockDisablesTheSwitch() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        const QString reason =
            QStringLiteral("Turned off by the GITBOLT_TELEMETRY environment variable.");
        dialog.setUsageStatistics(true, reason);

        auto* check = child<QCheckBox>(dialog, "settings.usage-statistics");
        auto* note = child<QLabel>(dialog, "settings.usage-locked-note");
        QVERIFY(!check->isEnabled());
        QVERIFY(!check->isChecked());
        QVERIFY(!note->isHidden());
        QCOMPARE(note->text(), reason);

        QVERIFY(QMetaObject::invokeMethod(&dialog, "apply"));
        QVERIFY(settings.usageStatisticsEnabled());
    }

    // The Git Config page's identity comes from `git config --global`,
    // which refused to run with GIT_CONFIG in GitBolt's environment ("only
    // one config file at a time"): the fields came up empty, and Apply
    // saved nothing. git's repository variables are left out of it, as
    // for every git GitBolt runs.
    void gitIdentityIgnoresAnExportedGitConfig() {
        QFile global(homeDir_.filePath(QStringLiteral("gitconfig")));
        QVERIFY(global.open(QIODevice::WriteOnly | QIODevice::Truncate));
        global.write("[user]\n\tname = Global Name\n");
        global.close();
        qputenv("GIT_CONFIG", QFile::encodeName(homeDir_.filePath(QStringLiteral("elsewhere"))));
        const auto restore = qScopeGuard([] { qunsetenv("GIT_CONFIG"); });

        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("settings.git-user-name"));
        QVERIFY(name);
        QTRY_COMPARE_WITH_TIMEOUT(name->text(), QStringLiteral("Global Name"), 10000);
    }

    void copyPutsTheIdOnTheClipboard() {
        // The offscreen platform keeps its clipboard in the process;
        // anywhere else this would overwrite the user's.
        const QString platform = QGuiApplication::platformName();
        if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal"))
            QSKIP("needs the offscreen platform (ctest sets it): the clipboard is the user's");

        SettingsService settings;
        const QString id = settings.ensureTelemetryInstallId();
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUsageStatistics(true, QString());
        QApplication::clipboard()->clear();
        child<QPushButton>(dialog, "settings.install-id-copy")->click();
        QCOMPARE(QApplication::clipboard()->text(), id);
    }

private:
    QTemporaryDir settingsDir_;
    QTemporaryDir homeDir_;
};

QTEST_MAIN(TestUsageStatisticsSettings)
#include "TestUsageStatisticsSettings.moc"
