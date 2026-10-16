//
// TestFileHistory — the File Tree's Show History… (RepositoryView).
//
// `git log --follow` runs on a worker, which takes a while on a big
// repository. Meanwhile nothing said it was coming, and a second
// Show History… on the file loaded it again and opened a second
// dialog. Now one load per file runs at a time, with a busy cursor
// and a status-bar note until its dialog comes up.
//

#include <QApplication>
#include <QDialog>
#include <QMainWindow>
#include <QStatusBar>
#include <QTest>

#include "../TestRepoHelper.h"
#include "services/GitService.h"
#include "ui/RepositoryView.h"

using gitbolt::services::GitService;
using gitbolt::ui::RepositoryView;

namespace {

int historyDialogs() {
    int open = 0;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (top->objectName() == QStringLiteral("dlg.fileHistory") && top->isVisible())
            ++open;
    }
    return open;
}

} // namespace

class TestFileHistory : public QObject {
    Q_OBJECT

private slots:
    void oneLoadPerFileWithFeedback() {
        gitbolt::test::TestRepo repo;
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "one\n", QStringLiteral("one")).ok());
        QVERIFY(repo.writeAndCommit(QStringLiteral("a.txt"), "two\n", QStringLiteral("two")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        QMainWindow window;
        auto* view = new RepositoryView(&window);
        window.setCentralWidget(view);
        view->setGitService(&svc);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        for (int click = 0; click < 3; ++click)
            QVERIFY(QMetaObject::invokeMethod(view, "showFileHistory",
                                              Q_ARG(QString, QStringLiteral("a.txt"))));
        QVERIFY(QGuiApplication::overrideCursor());
        QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::BusyCursor);
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Loading the history of a.txt…"));

        QTRY_COMPARE_WITH_TIMEOUT(historyDialogs(), 1, 15000);
        QTest::qWait(500);
        QCOMPARE(historyDialogs(), 1);
        QVERIFY(!QGuiApplication::overrideCursor());
        QCOMPARE(window.statusBar()->currentMessage(), QString());

        // Done, it can be asked for again.
        QVERIFY(QMetaObject::invokeMethod(view, "showFileHistory",
                                          Q_ARG(QString, QStringLiteral("a.txt"))));
        QTRY_COMPARE_WITH_TIMEOUT(historyDialogs(), 2, 15000);
    }
};

QTEST_MAIN(TestFileHistory)
#include "TestFileHistory.moc"
