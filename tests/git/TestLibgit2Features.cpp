#include <QStringList>
#include <QTest>
#include <git2.h>
#include "git/Repository.h"

// What GitBolt needs from the libgit2 it links. Only local repository
// access goes through libgit2 (clone, fetch, pull and push run the git
// CLI), so its network transports don't matter, but thread safety does:
// git operations run on QtConcurrent worker threads.
//
// The bundled build (GITBOLT_BUNDLED_LIBGIT2) is ours, so it is also
// held to its pinned version and backends (cmake/Libgit2.cmake).
class TestLibgit2Features : public QObject {
    Q_OBJECT

    static const char* backend(git_feature_t feature) {
#if LIBGIT2_VER_MAJOR > 1 || (LIBGIT2_VER_MAJOR == 1 && LIBGIT2_VER_MINOR >= 9)
        const char* name = git_libgit2_feature_backend(feature);
        return name ? name : "";
#else
        Q_UNUSED(feature);
        return "";   // no backend query before libgit2 1.9
#endif
    }

private slots:
    void isThreadSafe() {
        // For the CI logs: which libgit2 each platform's build got.
        qInfo("libgit2 %s: %s (threads '%s', https '%s', ssh '%s')",
              gitbolt::git::libgit2Version().c_str(),
              gitbolt::git::libgit2Features().c_str(),
              backend(GIT_FEATURE_THREADS), backend(GIT_FEATURE_HTTPS),
              backend(GIT_FEATURE_SSH));
        QVERIFY2((git_libgit2_features() & GIT_FEATURE_THREADS) != 0,
                 "libgit2 was built without thread support (USE_THREADS=OFF), "
                 "but GitBolt calls it from worker threads");
    }

    void bundledMatchesPin() {
#ifndef GITBOLT_BUNDLED_LIBGIT2_VERSION
        QSKIP("linked against a system libgit2");
#else
        // The pinned static build, and its headers, not some other
        // libgit2 on the library or include path (Homebrew's, say).
        QCOMPARE(QString::fromStdString(gitbolt::git::libgit2Version()),
                 QStringLiteral(GITBOLT_BUNDLED_LIBGIT2_VERSION));
        QCOMPARE(QStringLiteral(LIBGIT2_VERSION),
                 QStringLiteral(GITBOLT_BUNDLED_LIBGIT2_VERSION));
        // cmake/Libgit2.cmake: no SSH, HTTPS on the OS's TLS stack, and
        // none where that would be OpenSSL (Linux): the DEB would then
        // depend on it and the AppImage bundle it.
        QVERIFY((git_libgit2_features() & GIT_FEATURE_SSH) == 0);
#if defined(Q_OS_MACOS)
        QCOMPARE(backend(GIT_FEATURE_HTTPS), "securetransport");
#elif defined(Q_OS_WIN)
        QCOMPARE(backend(GIT_FEATURE_HTTPS), "winhttp");
#else
        QVERIFY2((git_libgit2_features() & GIT_FEATURE_HTTPS) == 0,
                 backend(GIT_FEATURE_HTTPS));
#endif
#endif
    }

    // The bundled build is held to its patches as well: the tests of a
    // patch skip without it (their #ifdefs), so a patch whose detection
    // in cmake/Libgit2.cmake stopped matching would go untested, and an
    // installer could ship without it, with only a configure warning.
    void bundledIsPatched() {
#ifndef GITBOLT_LIBGIT2_BUNDLED
        QSKIP("linked against a system libgit2");
#else
        QStringList missing;
#ifndef GITBOLT_LIBGIT2_CONFIG_PATCHED
        missing << QStringLiteral("config fixes");
#endif
#ifndef GITBOLT_LIBGIT2_NULL_VALUES_PATCHED
        missing << QStringLiteral("fixes for config lines with no value");
#endif
#ifndef GITBOLT_LIBGIT2_LIMITS_PATCHED
        missing << QStringLiteral("limits on hostile content");
#endif
#ifndef GITBOLT_LIBGIT2_TRUNCATION_PATCHED
        missing << QStringLiteral("fixes for objects cut short");
#endif
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("cmake/PatchLibgit2.cmake's ")
                            + missing.join(QStringLiteral(", "))
                            + QStringLiteral(" not found (see configure's warnings)")));
        // Help › About then shows the plain version.
        QCOMPARE(QString::fromStdString(gitbolt::git::libgit2VersionDescription()),
                 QString::fromStdString(gitbolt::git::libgit2Version()));
#endif
    }

    // Help › About says when the libgit2 is a system one, without
    // GitBolt's patches, so a bug report copied from it says so too.
    void systemVersionSaysUnpatched() {
#ifdef GITBOLT_LIBGIT2_BUNDLED
        QSKIP("linked against the bundled libgit2");
#else
        QCOMPARE(QString::fromStdString(gitbolt::git::libgit2VersionDescription()),
                 QString::fromStdString(gitbolt::git::libgit2Version())
                     + QStringLiteral(" (system, without GitBolt's patches)"));
#endif
    }
};

QTEST_MAIN(TestLibgit2Features)
#include "TestLibgit2Features.moc"
