//
// TestUpdateSettings — Settings › GitBolt › Updates, and the
// SettingsService keys behind in-app updates:
//
//   - updates/autoCheck defaults to on, persists, and an unchanged
//     value emits no settingsChanged;
//   - updates/lastCheck round-trips as UTC ISO 8601, and anything else
//     in GitBolt.ini (garbage, a time without its zone) reads as never,
//     which makes the next check due;
//   - updates/skippedVersion round-trips; neither of the last two
//     emits settingsChanged;
//   - the page's row (GitBolt's last) is hidden unless MainWindow says
//     the session has the in-app check, and a hidden row shows
//     nothing; shown, the switch reflects the setting, Apply writes it
//     once, Cancel writes nothing, and GITBOLT_UPDATE_CHECK's lock
//     disables it and says why; the note says what the check sends,
//     and a link goes to PRIVACY.md's Update checks section.
//
// Settings live in a folder of the test's own (QSettings::setPath), and
// Apply's git config writes go to a temporary global config.
//

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTreeWidget>

#include "conf/SettingsService.h"
#include "dialogs/SettingsDialog.h"

#include <algorithm>

using gitbolt::conf::SettingsService;
using gitbolt::dialogs::SettingsDialog;

namespace {

template <typename T>
T* child(const QWidget& parent, const char* name) {
    return parent.findChild<T*>(QString::fromLatin1(name));
}

void writeRaw(const QString& key, const QVariant& value) {
    QSettings raw(SettingsService().fileName(), QSettings::IniFormat);
    raw.setValue(key, value);
    raw.sync();
}

} // namespace

class TestUpdateSettings : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QVERIFY(homeDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QCoreApplication::setOrganizationName(QStringLiteral("GitBolt"));
        QCoreApplication::setApplicationName(QStringLiteral("GitBolt"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(homeDir_.filePath(QStringLiteral("gitconfig"))));
        qputenv("HOME", QFile::encodeName(homeDir_.path()));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(homeDir_.path()));
    }

    void init() {
        SettingsService settings;
        QSettings(settings.fileName(), QSettings::IniFormat).clear();
    }

    // ---- SettingsService ------------------------------------------------

    void autoCheckDefaultsOnAndPersists() {
        {
            SettingsService settings;
            QVERIFY(settings.updateAutoCheck());
            QSignalSpy changed(&settings, &SettingsService::settingsChanged);
            settings.setUpdateAutoCheck(false);
            QCOMPARE(changed.count(), 1);
            settings.setUpdateAutoCheck(false);
            QCOMPARE(changed.count(), 1);
        }
        QVERIFY(!SettingsService().updateAutoCheck());
    }

    void lastCheckIsUtcAndQuiet() {
        SettingsService settings;
        QSignalSpy changed(&settings, &SettingsService::settingsChanged);
        QVERIFY(!settings.updateLastCheck().isValid());

        // Written in another zone, read back as the same moment in UTC.
        const QDateTime when(QDate(2026, 11, 1), QTime(12, 30, 15, 250), QTimeZone::fromSecondsAheadOfUtc(3600));
        settings.setUpdateLastCheck(when);
        QCOMPARE(changed.count(), 0);
        const QDateTime read = SettingsService().updateLastCheck();
        QCOMPARE(read, when);
        QCOMPARE(read.timeSpec(), Qt::UTC);
        QCOMPARE(QSettings(settings.fileName(), QSettings::IniFormat).value(QStringLiteral("updates/lastCheck")).toString(),
                 QStringLiteral("2026-11-01T11:30:15.250Z"));

        settings.setUpdateLastCheck(QDateTime());
        QVERIFY(!settings.updateLastCheck().isValid());
        QVERIFY(!QSettings(settings.fileName(), QSettings::IniFormat).contains(QStringLiteral("updates/lastCheck")));
    }

    // Only what GitBolt writes is a time: the rest makes the check due.
    void lastCheckReadsAnythingElseAsNever_data() {
        QTest::addColumn<QString>("stored");
        QTest::newRow("garbage") << QStringLiteral("yesterday");
        QTest::newRow("no zone") << QStringLiteral("2026-11-01T11:30:15");
        QTest::newRow("empty") << QString();
    }
    void lastCheckReadsAnythingElseAsNever() {
        QFETCH(QString, stored);
        writeRaw(QStringLiteral("updates/lastCheck"), stored);
        QVERIFY(!SettingsService().updateLastCheck().isValid());
    }

    void skippedVersionRoundTrips() {
        SettingsService settings;
        QSignalSpy changed(&settings, &SettingsService::settingsChanged);
        QVERIFY(settings.skippedUpdateVersion().isEmpty());
        settings.setSkippedUpdateVersion(QStringLiteral("0.1.1"));
        QCOMPARE(SettingsService().skippedUpdateVersion(), QStringLiteral("0.1.1"));
        settings.setSkippedUpdateVersion(QString());
        QVERIFY(SettingsService().skippedUpdateVersion().isEmpty());
        QCOMPARE(changed.count(), 0);
    }

    // ---- SettingsDialog -------------------------------------------------

    // Source builds, forks and test sessions without a test feed have no
    // update check: nothing to show. The page is GitBolt's last, after
    // Usage Statistics (whose row the telemetry tests count on).
    void pageHiddenUnlessAvailable() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        auto* nav = child<QTreeWidget>(dialog, "settings.nav");
        auto* pages = dialog.findChild<QStackedWidget*>();
        auto* group = child<QGroupBox>(dialog, "settings.updates-group");
        QVERIFY(nav && pages && group);
        QTreeWidgetItem* gitbolt = nav->topLevelItem(0);
        QTreeWidgetItem* item = gitbolt->child(gitbolt->childCount() - 1);
        QCOMPARE(item->text(0), QStringLiteral("Updates"));
        QCOMPARE(gitbolt->indexOfChild(item), 6);
        QCOMPARE(gitbolt->child(5)->text(0), QStringLiteral("Usage Statistics"));
        QVERIFY(item->isHidden());

        dialog.setUpdates(false, QString());
        QVERIFY(item->isHidden());
        QWidget* general = pages->currentWidget();
        nav->setCurrentItem(item);
        QCOMPARE(pages->currentWidget(), general);

        dialog.setUpdates(true, QString());
        QVERIFY(!item->isHidden());
        nav->setCurrentItem(gitbolt->child(0));
        nav->setCurrentItem(item);
        QVERIFY(pages->currentWidget()->isAncestorOf(group));
    }

    void showsTheSettingAndWhatItSends() {
        SettingsService settings;
        settings.setUpdateAutoCheck(false);
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUpdates(true, QString());

        auto* check = child<QCheckBox>(dialog, "settings.check-for-updates");
        auto* locked = child<QLabel>(dialog, "settings.check-for-updates-locked-note");
        auto* note = child<QLabel>(dialog, "settings.check-for-updates-note");
        QVERIFY(check && locked && note);
        QCOMPARE(check->text(), QStringLiteral("Check for updates automatically"));
        QVERIFY(!check->isChecked());
        QVERIFY(check->isEnabled());
        QVERIFY(locked->isHidden());
        QCOMPARE(note->text(),
                 QStringLiteral("Once a day GitBolt asks GitHub whether a newer release is out. GitHub sees your "
                                "IP address. Nothing is downloaded or installed until you choose Install and "
                                "Restart."));

        settings.setUpdateAutoCheck(true);
        SettingsDialog reopened(&settings, nullptr);
        reopened.setUpdates(true, QString());
        QVERIFY(child<QCheckBox>(reopened, "settings.check-for-updates")->isChecked());
    }

    // As the Usage Statistics page links its section: PRIVACY.md's
    // "## Update checks", by the anchor GitHub gives that heading.
    void linksThePrivacySection() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUpdates(true, QString());
        auto* group = child<QGroupBox>(dialog, "settings.updates-group");
        auto* link = child<QLabel>(dialog, "settings.updates-privacy-link");
        QVERIFY(group && link);
        QVERIFY(group->isAncestorOf(link));
        QCOMPARE(link->textFormat(), Qt::RichText);
        QVERIFY(link->openExternalLinks());
        QVERIFY2(link->text().contains(
                     QStringLiteral("href=\"https://github.com/Inverian/GitBolt/blob/main/PRIVACY.md#update-checks\"")),
                 qPrintable(link->text()));
        QVERIFY2(link->text().contains(QStringLiteral(">What's sent, and what's checked before installing</a>")),
                 qPrintable(link->text()));

        // The heading is there to link to, and is the only one GitHub
        // would give that anchor (a second one would be update-checks-1).
        QFile privacy(QFileInfo(QStringLiteral(__FILE__)).dir().filePath(QStringLiteral("../../PRIVACY.md")));
        // Text mode: a Windows checkout may have CRLF line ends.
        QVERIFY2(privacy.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(privacy.fileName()));
        const QList<QByteArray> lines = privacy.readAll().split('\n');
        QCOMPARE(std::count_if(lines.cbegin(), lines.cend(),
                               [](const QByteArray& line) {
                                   return line.startsWith('#') && line.contains(" Update checks")
                                          && line.trimmed().endsWith("Update checks");
                               }),
                 1);
        QVERIFY(lines.contains("## Update checks"));
    }

    void applyWritesTheSwitchOnce() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUpdates(true, QString());
        QSignalSpy changed(&settings, &SettingsService::settingsChanged);

        child<QCheckBox>(dialog, "settings.check-for-updates")->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "apply"));
        QVERIFY(!settings.updateAutoCheck());
        QCOMPARE(changed.count(), 1);
    }

    void cancelWritesNothing() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUpdates(true, QString());
        child<QCheckBox>(dialog, "settings.check-for-updates")->setChecked(false);
        dialog.reject();
        QVERIFY(settings.updateAutoCheck());
    }

    // Hidden, Apply leaves the key alone: a build without the check
    // writes no [updates] section.
    void hiddenPageWritesNothing() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        dialog.setUpdates(false, QString());
        child<QCheckBox>(dialog, "settings.check-for-updates")->setChecked(false);
        QVERIFY(QMetaObject::invokeMethod(&dialog, "apply"));
        QVERIFY(!QSettings(settings.fileName(), QSettings::IniFormat).contains(QStringLiteral("updates/autoCheck")));
    }

    void environmentLockDisablesTheSwitch() {
        SettingsService settings;
        SettingsDialog dialog(&settings, nullptr);
        const QString reason = QStringLiteral("Turned off by the GITBOLT_UPDATE_CHECK environment variable.");
        dialog.setUpdates(true, reason);

        auto* check = child<QCheckBox>(dialog, "settings.check-for-updates");
        auto* locked = child<QLabel>(dialog, "settings.check-for-updates-locked-note");
        QVERIFY(!check->isEnabled());
        QVERIFY(!check->isChecked());   // what this session does
        QVERIFY(!locked->isHidden());
        QCOMPARE(locked->text(), reason);

        QVERIFY(QMetaObject::invokeMethod(&dialog, "apply"));
        QVERIFY(settings.updateAutoCheck());   // the stored preference stays
    }

private:
    QTemporaryDir settingsDir_;
    QTemporaryDir homeDir_;
};

QTEST_MAIN(TestUpdateSettings)
#include "TestUpdateSettings.moc"
