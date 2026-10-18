//
// TestOpenExternally — the File Tree's Open Externally (RepositoryView).
//
// It handed the desktop the work tree's file as it was, and the
// desktop opens a file with its default app. On macOS that is
// Terminal for an executable file with no extension, a CHANGELOG at
// mode 755 say, and for a .command file, and Terminal runs it; a
// tracked symbolic link led wherever it pointed, out of the work tree
// too. Nothing in the tree tells such a file from a text file. Now:
//
//   - a file that may run when opened (marked executable, or of a type
//     the platform's opener runs) is opened only once the user says
//     so, in a plain-text question that names it and defaults to
//     Cancel; Open Containing Folder shows its folder instead, and
//     never a bundle (macOS launches a folder named Tool.app);
//   - so is a file of a type whose default app runs it or follows it
//     though git checked it out at mode 644 (a Terminal session file,
//     a jar, a location file, on macOS);
//   - a link is judged by the file it leads to, and one that leads out
//     of the work tree is not opened at all, only the folder it leads
//     into shown if the user asks; a link to a folder shows the folder,
//     and never launches a bundle;
//   - a text file opens as before, with no question.
//
// The desktop never sees a URL: QDesktopServices::setUrlHandler takes
// every file URL. Settings live in a folder of the test's own.
//

#include <QAbstractButton>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <memory>

#include "../TestRepoHelper.h"
#include "services/GitService.h"
#include "ui/RepositoryView.h"
#include "widgets/FileTreeWidget.h"

using gitbolt::services::GitService;
using gitbolt::ui::RepositoryView;

namespace {

// Takes the file URLs QDesktopServices would hand the desktop.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    UrlCatcher() { QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "open"); }
    ~UrlCatcher() override { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
    QList<QUrl> urls;

public slots:
    void open(const QUrl& url) { urls << url; }
};

// Answers the next message box with its button labelled `button`,
// noting how it asked.
class BoxAnswerer {
public:
    explicit BoxAnswerer(const QString& button) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, [this, button] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box)
                return;
            timer_.stop();
            ++asked;
            name = box->objectName();
            text = box->text();
            plainText = box->textFormat() == Qt::PlainText;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
            qtBox = box->testOption(QMessageBox::Option::DontUseNativeDialog);
#else
            qtBox = true;
#endif
            cancelIsDefault = box->defaultButton()
                              && static_cast<QAbstractButton*>(box->defaultButton())
                                     == box->button(QMessageBox::Cancel);
            for (QAbstractButton* b : box->buttons()) {
                if (b->text() == button) {
                    b->click();
                    return;
                }
            }
            missing = true;
            box->reject();
        });
        timer_.start();
    }
    int asked = 0;
    QString name;
    QString text;
    bool plainText = false;
    bool qtBox = false;  // not the platform's, which may not keep the default
    bool cancelIsDefault = false;
    bool missing = false;

private:
    QTimer timer_;
};

void writeFile(const QString& path, const QByteArray& data, bool executable) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("cannot write %s", qPrintable(path));
    file.write(data);
    file.close();
    QFile::Permissions mode = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup
                              | QFile::ReadOther;
    if (executable)
        mode |= QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
    if (!file.setPermissions(mode))
        qFatal("cannot set the mode of %s", qPrintable(path));
}

// A harmless script: it would only print a marker, were it run.
const QByteArray kScript = "#!/bin/sh\necho marker >/dev/null\n";

// A name the platform's opener runs, or follows, by its type alone.
QString runnableName() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("run.command");
#elif defined(Q_OS_WIN)
    return QStringLiteral("run.bat");
#else
    return QStringLiteral("run.desktop");
#endif
}

QString canonical(const QString& path) {
    return QFileInfo(path).canonicalFilePath();
}

QString canonical(const QUrl& url) {
    return QFileInfo(url.toLocalFile()).canonicalFilePath();
}

} // namespace

class TestOpenExternally : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void init() {
        repo_ = std::make_unique<gitbolt::test::TestRepo>();
        QVERIFY(repo_->writeAndCommit(QStringLiteral("notes.txt"), "notes\n",
                                      QStringLiteral("base")).ok());
        svc_ = std::make_unique<GitService>();
        QVERIFY(svc_->openRepository(repo_->path()));
        window_ = std::make_unique<QMainWindow>();
        view_ = new RepositoryView(window_.get());
        window_->setCentralWidget(view_);
        view_->setGitService(svc_.get());
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    }

    void cleanup() {
        window_.reset();
        view_ = nullptr;
        svc_.reset();
        repo_.reset();
    }

    // A file the desktop may run is opened only if the user says so:
    // Cancel (the default) opens nothing, Open Containing Folder shows
    // the work tree's folder it is in, Open Anyway opens the file.
    void runnableFileAsksFirst_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("executable");
        QTest::addColumn<QString>("answer");
        QTest::addColumn<QString>("opened");  // "", "folder" or "file"

        const QStringList answers{QStringLiteral("Cancel"),
                                  QStringLiteral("Open Containing Folder"),
                                  QStringLiteral("Open Anyway")};
        const QStringList opened{QString(), QStringLiteral("folder"), QStringLiteral("file")};
        for (int i = 0; i < answers.size(); ++i) {
#ifndef Q_OS_WIN
            // Windows has no executable bit: there the type says it.
            QTest::addRow("CHANGELOG at 755, %s", qPrintable(answers[i]))
                << QStringLiteral("CHANGELOG") << true << answers[i] << opened[i];
            QTest::addRow("tools/build at 755, %s", qPrintable(answers[i]))
                << QStringLiteral("tools/build") << true << answers[i] << opened[i];
#endif
            QTest::addRow("%s, %s", qPrintable(runnableName()), qPrintable(answers[i]))
                << runnableName() << false << answers[i] << opened[i];
        }
    }

    void runnableFileAsksFirst() {
        QFETCH(QString, path);
        QFETCH(bool, executable);
        QFETCH(QString, answer);
        QFETCH(QString, opened);
        const QString full = repo_->path() + QLatin1Char('/') + path;
        writeFile(full, kScript, executable);

        UrlCatcher desktop;
        BoxAnswerer box(answer);
        emit fileTree()->openExternallyRequested(path);

        QCOMPARE(box.asked, 1);
        QVERIFY(!box.missing);
        QCOMPARE(box.name, QStringLiteral("dlg.openExternallyRuns"));
        QVERIFY2(box.text.contains(QDir::toNativeSeparators(path)), qPrintable(box.text));
        QVERIFY2(box.text.contains(QStringLiteral("may run as a program")), qPrintable(box.text));
        QVERIFY(box.plainText);
        QVERIFY(box.qtBox);
        QVERIFY(box.cancelIsDefault);
        if (opened.isEmpty()) {
            QCOMPARE(desktop.urls.size(), 0);
            return;
        }
        QCOMPARE(desktop.urls.size(), 1);
        QCOMPARE(canonical(desktop.urls.first()),
                 canonical(opened == QStringLiteral("file") ? full
                                                           : QFileInfo(full).absolutePath()));
    }

    // Types whose default app runs them, or opens what they point to,
    // ask too: git checks them out at mode 644, so their name alone
    // tells them.
    void typesThatRunAskFirst_data() {
        QTest::addColumn<QString>("path");
#if defined(Q_OS_MACOS)
        for (const char* name : {"session.term", "tool.jar", "Main.class", "app.jnlp",
                                 "share.afploc", "site.ftploc", "link.url", "a.vncloc"})
            QTest::newRow(name) << QString::fromLatin1(name);
#elif defined(Q_OS_WIN)
        for (const char* name : {"x.cpl", "x.chm", "x.application", "x.jar"})
            QTest::newRow(name) << QString::fromLatin1(name);
#else
        for (const char* name : {"setup.exe", "tool.jar"})
            QTest::newRow(name) << QString::fromLatin1(name);
#endif
    }

    void typesThatRunAskFirst() {
        QFETCH(QString, path);
        writeFile(repo_->path() + QLatin1Char('/') + path, kScript, false);

        UrlCatcher desktop;
        BoxAnswerer box(QStringLiteral("Cancel"));
        emit fileTree()->openExternallyRequested(path);
        QCOMPARE(box.asked, 1);
        QVERIFY(!box.missing);
        QCOMPARE(box.name, QStringLiteral("dlg.openExternallyRuns"));
        QCOMPARE(desktop.urls.size(), 0);
    }

    // A text file opens as it did: no question, the file itself.
    void textFileOpensAsBefore() {
        UrlCatcher desktop;
        BoxAnswerer box(QStringLiteral("Cancel"));
        emit fileTree()->openExternallyRequested(QStringLiteral("notes.txt"));
        QCOMPARE(box.asked, 0);
        QCOMPARE(desktop.urls.size(), 1);
        QVERIFY(desktop.urls.first().isLocalFile());
        QCOMPARE(canonical(desktop.urls.first()),
                 canonical(repo_->path() + QStringLiteral("/notes.txt")));
    }

#ifndef Q_OS_WIN
    // A tracked symbolic link (or a folder on the way that is one) that
    // leads out of the work tree is not opened, whatever is there; the
    // folder it leads into is shown if the user asks, to open it from
    // there. One that stays in it is judged by the file it leads to.
    void linkOutOfTheWorkTreeIsNotOpened_data() {
        QTest::addColumn<QString>("link");   // the link, in the work tree
        QTest::addColumn<QString>("leadsTo"); // in a folder outside it
        QTest::addColumn<QString>("path");   // what is opened
        QTest::addColumn<QString>("answer");
        for (const QString& answer : {QStringLiteral("OK"),
                                      QStringLiteral("Open Containing Folder")}) {
            QTest::addRow("CHANGELOG to a script, %s", qPrintable(answer))
                << QStringLiteral("CHANGELOG") << QStringLiteral("evil.command")
                << QStringLiteral("CHANGELOG") << answer;
            QTest::addRow("notes to a text file, %s", qPrintable(answer))
                << QStringLiteral("README") << QStringLiteral("notes.txt")
                << QStringLiteral("README") << answer;
            QTest::addRow("a folder on the way, %s", qPrintable(answer))
                << QStringLiteral("docs") << QStringLiteral(".")
                << QStringLiteral("docs/evil.command") << answer;
        }
    }

    void linkOutOfTheWorkTreeIsNotOpened() {
        QFETCH(QString, link);
        QFETCH(QString, leadsTo);
        QFETCH(QString, path);
        QFETCH(QString, answer);
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        writeFile(outside.filePath(QStringLiteral("evil.command")), kScript, true);
        writeFile(outside.filePath(QStringLiteral("notes.txt")), "notes\n", false);
        QVERIFY(QFile::link(QDir::cleanPath(outside.filePath(leadsTo)),
                            repo_->path() + QLatin1Char('/') + link));

        UrlCatcher desktop;
        BoxAnswerer box(answer);
        emit fileTree()->openExternallyRequested(path);
        QCOMPARE(box.asked, 1);
        QVERIFY(!box.missing);
        QCOMPARE(box.name, QStringLiteral("dlg.openExternallyOutside"));
        QVERIFY2(box.text.contains(path), qPrintable(box.text));
        QVERIFY2(box.text.contains(QStringLiteral("outside the working tree")),
                 qPrintable(box.text));
        QVERIFY(box.plainText);
        QVERIFY(box.qtBox);
        if (answer == QStringLiteral("OK")) {
            QCOMPARE(desktop.urls.size(), 0);
            return;
        }
        // The folder, never the file.
        QCOMPARE(desktop.urls.size(), 1);
        QCOMPARE(canonical(desktop.urls.first()), canonical(outside.path()));
    }

    // A link to a folder shows the folder, with no question: a bundle
    // (a link to Tool.app) isn't launched, the folder it is in is shown.
    void linkToAFolderShowsIt_data() {
        QTest::addColumn<QString>("link");
        QTest::addColumn<QString>("leadsTo");
        QTest::addColumn<QString>("shown");
        QTest::newRow("docs") << QStringLiteral("linkdir") << QStringLiteral("docs")
                              << QStringLiteral("docs");
#ifdef Q_OS_MACOS
        QTest::newRow("Tool.app") << QStringLiteral("applink") << QStringLiteral("x/Tool.app")
                                  << QStringLiteral("x");
#else
        QTest::newRow("Tool.app") << QStringLiteral("applink") << QStringLiteral("x/Tool.app")
                                  << QStringLiteral("x/Tool.app");
#endif
    }

    void linkToAFolderShowsIt() {
        QFETCH(QString, link);
        QFETCH(QString, leadsTo);
        QFETCH(QString, shown);
        const QString root = repo_->path() + QLatin1Char('/');
        writeFile(root + QStringLiteral("docs/guide.md"), "guide\n", false);
        writeFile(root + QStringLiteral("x/Tool.app/Contents/MacOS/Tool"), kScript, true);
        QVERIFY(QFile::link(root + leadsTo, root + link));

        UrlCatcher desktop;
        BoxAnswerer box(QStringLiteral("Open Anyway"));
        emit fileTree()->openExternallyRequested(link);
        QTest::qWait(100);  // a question would be up and answered by now
        QCOMPARE(box.asked, 0);
        QCOMPARE(desktop.urls.size(), 1);
        QCOMPARE(desktop.urls.first().toLocalFile(), canonical(root + shown));
    }

    void linkInTheWorkTreeIsJudgedByItsFile() {
        writeFile(repo_->path() + QStringLiteral("/tools/CHANGELOG"), kScript, true);
        QVERIFY(QFile::link(repo_->path() + QStringLiteral("/tools/CHANGELOG"),
                            repo_->path() + QStringLiteral("/notes.md")));

        UrlCatcher desktop;
        BoxAnswerer box(QStringLiteral("Cancel"));
        emit fileTree()->openExternallyRequested(QStringLiteral("notes.md"));
        QCOMPARE(box.asked, 1);
        QCOMPARE(box.name, QStringLiteral("dlg.openExternallyRuns"));
        QCOMPARE(desktop.urls.size(), 0);

        // Opened anyway, it is the file the link leads to that opens,
        // the one that was looked at.
        BoxAnswerer anyway(QStringLiteral("Open Anyway"));
        emit fileTree()->openExternallyRequested(QStringLiteral("notes.md"));
        QCOMPARE(anyway.asked, 1);
        QCOMPARE(desktop.urls.size(), 1);
        QCOMPARE(desktop.urls.first().toLocalFile(),
                 canonical(repo_->path() + QStringLiteral("/tools/CHANGELOG")));
    }
#endif

#ifdef Q_OS_MACOS
    // Open Containing Folder of a file in a folder named Tool.app shows
    // the folder that one is in: macOS launches a bundle it is asked to
    // open, as Finder does an app.
    void containingFolderIsNeverABundle() {
        writeFile(repo_->path() + QStringLiteral("/Tool.app/CHANGELOG"), kScript, true);

        UrlCatcher desktop;
        BoxAnswerer box(QStringLiteral("Open Containing Folder"));
        emit fileTree()->openExternallyRequested(QStringLiteral("Tool.app/CHANGELOG"));
        QCOMPARE(box.asked, 1);
        QVERIFY(!box.missing);
        QCOMPARE(desktop.urls.size(), 1);
        QCOMPARE(canonical(desktop.urls.first()), canonical(repo_->path()));
    }
#endif

private:
    gitbolt::widgets::FileTreeWidget* fileTree() const {
        auto* tree = view_->findChild<gitbolt::widgets::FileTreeWidget*>();
        if (!tree)
            qFatal("no file tree");
        return tree;
    }

    QTemporaryDir settingsDir_;
    std::unique_ptr<gitbolt::test::TestRepo> repo_;
    std::unique_ptr<GitService> svc_;
    std::unique_ptr<QMainWindow> window_;
    RepositoryView* view_ = nullptr;
};

QTEST_MAIN(TestOpenExternally)
#include "TestOpenExternally.moc"
