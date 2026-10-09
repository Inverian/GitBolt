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
        // cmake/Libgit2.cmake: no SSH, HTTPS on the OS's TLS stack.
        QVERIFY((git_libgit2_features() & GIT_FEATURE_SSH) == 0);
#if defined(Q_OS_MACOS)
        QCOMPARE(backend(GIT_FEATURE_HTTPS), "securetransport");
#elif defined(Q_OS_WIN)
        QCOMPARE(backend(GIT_FEATURE_HTTPS), "winhttp");
#endif
#endif
    }
};

QTEST_MAIN(TestLibgit2Features)
#include "TestLibgit2Features.moc"
