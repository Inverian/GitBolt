//
// TestEditConfigFile — Repository > Edit .gitignore, .gitattributes and
// .mailmap, in a real MainWindow (off screen).
//
// The three are the repository's own content, and a repository can
// commit any of them as a symbolic link: git's fsck notes it and lets
// it be. Editing read the file the link pointed to, and saving wrote
// the text there, outside the work tree too. Now:
//
//   - a link is refused with a message, before the editor opens, and
//     what it points to stays as it was (or missing);
//   - a link put there while the editor is open (a checkout in a
//     terminal) is refused when the editor is saved;
//   - a file is edited and saved as it always was.
//

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScopedValueRollback>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>

#include "../TestRepoHelper.h"
#include "git/GitProcess.h"
#include "services/GitService.h"
#include "ui/MainWindow.h"

using gitbolt::services::GitService;
using gitbolt::ui::MainWindow;

namespace {

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

// A file's bytes, or "<missing>".
QByteArray fileBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArrayLiteral("<missing>");
}

// The bytes the editor saves for `text`. It writes in text mode, as it
// always has, and so on Windows each line ends in CRLF, as Git for
// Windows checks text files out by default (core.autocrlf true).
QByteArray savedText(QByteArray text) {
#ifdef Q_OS_WIN
    text.replace("\n", "\r\n");
#endif
    return text;
}

// Answers the window's modal dialogs while it lives: a message box with
// OK, noting its text; the text editor by calling `edit` with it (which
// sets the text it saves), then saving.
class ModalAnswerer {
public:
    explicit ModalAnswerer(std::function<void(QDialog*)> edit) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, [this, edit] {
            if (inside_)
                return;
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal || !modal->isVisible())
                return;
            const QScopedValueRollback<bool> busy(inside_, true);
            if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                boxes << box->text();
                if (QAbstractButton* ok = box->button(QMessageBox::Ok))
                    ok->click();
                else
                    box->reject();
            } else {
                editors << modal->windowTitle();
                edit(modal);
                modal->accept();
            }
        });
        timer_.start();
    }
    QStringList boxes;
    QStringList editors;

private:
    QTimer timer_;
    bool inside_ = false;
};

// Sets the text editor's text.
void setText(QDialog* editor, const QString& text) {
    if (auto* edit = editor->findChild<QPlainTextEdit*>())
        edit->setPlainText(text);
}

} // namespace

class TestEditConfigFile : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QVERIFY(writeFile(global, "[user]\n\tname = T\n\temail = t@example.com\n"
                                  "[commit]\n\tgpgSign = false\n"
                                  "[init]\n\tdefaultBranch = main\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    // .gitignore committed as a link to a file outside the repository,
    // and .mailmap as one to a file that doesn't exist: neither opens in
    // the editor, and nothing outside the work tree changes.
    void aLinkIsNotEdited() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString marker = outside.filePath(QStringLiteral("MARKER"));
        const QString missing = outside.filePath(QStringLiteral("MISSING"));
        QVERIFY(writeFile(marker, "original marker\n"));
        gitbolt::test::TestRepo repo;
        QVERIFY(QFile::link(marker, QDir(repo.path()).filePath(QStringLiteral(".gitignore"))));
        QVERIFY(QFile::link(missing, QDir(repo.path()).filePath(QStringLiteral(".mailmap"))));
        gitbolt::git::GitProcess git(repo.path().toStdString());
        QVERIFY(succeeded(git.run({"add", ".gitignore", ".mailmap"})));
        QVERIFY(succeeded(git.run({"commit", "-q", "-m", "links"})));

        MainWindow window;
        QVERIFY(openIn(window, repo.path()));
        ModalAnswerer answerer([](QDialog* editor) { setText(editor, QStringLiteral("edited\n")); });
        action(window, "repository.edit-gitignore")->trigger();
        action(window, "repository.edit-mailmap")->trigger();

        QCOMPARE(answerer.editors, QStringList());
        QCOMPARE(answerer.boxes,
                 (QStringList{
                     QStringLiteral(".gitignore is a symbolic link: edit the file it points "
                                    "to instead."),
                     QStringLiteral(".mailmap is a symbolic link: edit the file it points "
                                    "to instead.")}));
        QCOMPARE(fileBytes(marker), QByteArrayLiteral("original marker\n"));
        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(QFileInfo(QDir(repo.path()).filePath(QStringLiteral(".gitignore"))).isSymLink());
#endif
    }

    // .gitattributes is a file when the editor opens, and a link to a
    // file outside the repository by the time it is saved: the save is
    // refused, and the file it points to stays as it was.
    void aLinkMadeWhileEditingIsNotWritten() {
#ifdef Q_OS_WIN
        QSKIP("QFile::link makes a shortcut on Windows, not a link");
#else
        QTemporaryDir outside;
        QVERIFY(outside.isValid());
        const QString marker = outside.filePath(QStringLiteral("MARKER"));
        QVERIFY(writeFile(marker, "original marker\n"));
        gitbolt::test::TestRepo repo;
        const QString attributes = QDir(repo.path()).filePath(QStringLiteral(".gitattributes"));
        QVERIFY(writeFile(attributes, "*.txt text\n"));

        MainWindow window;
        QVERIFY(openIn(window, repo.path()));
        bool swapped = false;
        ModalAnswerer answerer([&](QDialog* editor) {
            setText(editor, QStringLiteral("*.txt -text\n"));
            swapped = QFile::remove(attributes) && QFile::link(marker, attributes);
        });
        action(window, "repository.edit-gitattributes")->trigger();

        QVERIFY(swapped);
        QCOMPARE(answerer.editors, QStringList{QStringLiteral("Edit .gitattributes")});
        QCOMPARE(answerer.boxes,
                 QStringList{QStringLiteral(".gitattributes is a symbolic link: edit the file "
                                            "it points to instead.")});
        QCOMPARE(fileBytes(marker), QByteArrayLiteral("original marker\n"));
#endif
    }

    // A file, and one not there yet, edit and save as they always have.
    void aFileIsEditedAndSaved() {
        gitbolt::test::TestRepo repo;
        const QString ignore = QDir(repo.path()).filePath(QStringLiteral(".gitignore"));
        const QString mailmap = QDir(repo.path()).filePath(QStringLiteral(".mailmap"));
        QVERIFY(writeFile(ignore, "build/\n"));

        MainWindow window;
        QVERIFY(openIn(window, repo.path()));
        QString shown;
        ModalAnswerer answerer([&](QDialog* editor) {
            if (auto* edit = editor->findChild<QPlainTextEdit*>())
                shown = edit->toPlainText();
            setText(editor, shown + QStringLiteral("*.o\n"));
        });
        action(window, "repository.edit-gitignore")->trigger();
        QCOMPARE(shown, QStringLiteral("build/\n"));
        QCOMPARE(fileBytes(ignore), savedText("build/\n*.o\n"));
        action(window, "repository.edit-mailmap")->trigger();
        QCOMPARE(fileBytes(mailmap), savedText("*.o\n"));
        QCOMPARE(answerer.boxes, QStringList());
        QCOMPARE(answerer.editors.size(), 2);
    }

private:
    static bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
        return r.ok() && r.value().success();
    }

    static GitService* openIn(MainWindow& window, const QString& path) {
        window.show();
        if (!QTest::qWaitForWindowExposed(&window))
            return nullptr;
        window.openRepositoryAtPath(path);
        auto* svc = window.findChild<GitService*>();
        auto* commit = window.findChild<QAction*>(QStringLiteral("act.commit"));
        if (!svc || !commit)
            return nullptr;
        const bool open = QTest::qWaitFor([&] {
            return svc->isOpen() && !svc->openPending() && commit->isEnabled();
        }, 15000);
        return open ? svc : nullptr;
    }

    static QAction* action(MainWindow& window, const char* name) {
        auto* a = window.findChild<QAction*>(QString::fromLatin1(name));
        if (!a)
            qFatal("no action %s", name);
        return a;
    }

    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestEditConfigFile)
#include "TestEditConfigFile.moc"
