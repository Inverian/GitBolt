//
// TestRecentRepositories — File › Recent and the home screen's list
// (conf::SettingsService): one entry per repository, whatever spelling
// of its folder it was opened by.
//

#include "conf/SettingsService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

using gitbolt::conf::SettingsService;

class TestRecentRepositories : public QObject {
    Q_OBJECT

    QTemporaryDir settingsDir_;

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QCoreApplication::setOrganizationName(QStringLiteral("GitBolt"));
        QCoreApplication::setApplicationName(QStringLiteral("GitBolt"));
    }

    void init() {
        SettingsService settings;
        settings.clearRecentRepositories();
    }

    // The same folder through a symbolic link: one entry, with the
    // spelling it was opened by last.
    void oneEntryPerFolderWhateverItsSpelling() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString real = base.filePath(QStringLiteral("real/repo"));
        QVERIFY(QDir().mkpath(real));
        QVERIFY(QFile::link(base.filePath(QStringLiteral("real")),
                            base.filePath(QStringLiteral("link"))));
        const QString viaLink = base.filePath(QStringLiteral("link/repo"));

        SettingsService settings;
        settings.addRecentRepository(real);
        settings.addRecentRepository(viaLink);
        QCOMPARE(settings.recentRepositories(), QStringList{QDir::toNativeSeparators(viaLink)});
        settings.addRecentRepository(real);
        QCOMPARE(settings.recentRepositories(), QStringList{QDir::toNativeSeparators(real)});
#endif
    }

    // The same folder by its name in another case, where the file
    // system takes either (Windows, and macOS's own volumes): one
    // entry, with the spelling it was opened by last. Explorer gives a
    // folder's case on disk, a typed path or a second launch's argument
    // the case it was typed in, and on Windows both stayed. Where case
    // counts, they are two folders, and two entries.
    void oneEntryPerFolderWhateverItsCase() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString onDisk = base.filePath(QStringLiteral("Repo"));
        const QString typed = base.filePath(QStringLiteral("rEPO"));
        QVERIFY(QDir().mkpath(onDisk));
        const bool eitherCase = QFileInfo::exists(typed);
        if (!eitherCase)
            QVERIFY(QDir().mkpath(typed));

        SettingsService settings;
        settings.addRecentRepository(onDisk);
        settings.addRecentRepository(typed);
        if (eitherCase)
            QCOMPARE(settings.recentRepositories(), QStringList{QDir::toNativeSeparators(typed)});
        else
            QCOMPARE(settings.recentRepositories(),
                     (QStringList{QDir::toNativeSeparators(typed),
                                  QDir::toNativeSeparators(onDisk)}));
    }

    // Two different folders stay two entries, and an entry whose
    // folder is gone is not looked at (nor dropped) when another is
    // added.
    void differentFoldersStaySeparate() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString a = base.filePath(QStringLiteral("a"));
        const QString b = base.filePath(QStringLiteral("b"));
        const QString gone = base.filePath(QStringLiteral("gone"));
        QVERIFY(QDir().mkpath(a));
        QVERIFY(QDir().mkpath(b));
        QVERIFY(QDir().mkpath(gone));

        SettingsService settings;
        settings.addRecentRepository(gone);
        QVERIFY(QDir(gone).removeRecursively());
        settings.addRecentRepository(a);
        settings.addRecentRepository(b);
        QCOMPARE(settings.recentRepositories(),
                 (QStringList{QDir::toNativeSeparators(b), QDir::toNativeSeparators(a),
                              QDir::toNativeSeparators(gone)}));
    }

    // Removing an entry, and clearing the list, take its resolved path
    // too: a later spelling of a removed folder is a new entry.
    void removeForgetsTheResolvedPath() {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString a = base.filePath(QStringLiteral("a"));
        QVERIFY(QDir().mkpath(a));
        SettingsService settings;
        settings.addRecentRepository(a);
        settings.removeRecentRepository(a);
        QVERIFY(settings.recentRepositories().isEmpty());
        settings.addRecentRepository(a + QStringLiteral("/"));
        QCOMPARE(settings.recentRepositories(), QStringList{QDir::toNativeSeparators(a)});
        settings.clearRecentRepositories();
        QVERIFY(settings.recentRepositories().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestRecentRepositories)
#include "TestRecentRepositories.moc"
