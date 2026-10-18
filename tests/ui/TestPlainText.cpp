//
// TestPlainText — text that a repository, a remote or git wrote, shown
// as the characters it is, wherever GitBolt shows it.
//
// A QLabel, a message box, an input dialog's prompt and a tooltip take
// their text for rich text when Qt::mightBeRichText() finds a tag in its
// first line. A branch named "<b>MARKER</b>" (a valid name), a submodule
// whose name holds "<a href=…>", a server's "remote: <font …>" were
// rendered: a confirmation could name something else than what it acted
// on, and a message box opens a link clicked in its text (QMessageBox
// turns openExternalLinks on), so one click handed the desktop a URL the
// repository chose.
//
//   - PlainMessageBox: plain text, and a click across it opens nothing
//     (one built as QMessageBox::question() builds it does); its icon,
//     buttons and default button are QMessageBox's;
//   - a repository in a folder "<b>MARKER</b>", with a branch of that
//     name checked out: the status bar's branch and path, a push that a
//     server's hook refuses (the toolbar's label and its tooltip, git's
//     lines kept), the sidebar's Rename, New Branch and Set Upstream
//     prompts and Delete question, Manage Submodules' Deinit question
//     and the box of an Init that git turns down;
//   - the Clone dialog's status, the Commit dialog's error (a hook's
//     output) and branch, the askpass prompt
//     (PlainInputDialog::setLabelText, an "&" in it included), the
//     labels naming a file (Blame, the diff, the .gitignore editor) or a
//     commit (Cherry-Pick);
//   - the tooltips of the file lists, of Manage Submodules and Manage
//     Worktrees, of a reworded commit in the Rebase dialog and of the
//     home screen's recent list; and tooltips keep the size and lines
//     they had (util::plainToolTip leaves what Qt shows as plain text
//     alone);
//   - the Commit tab's author and committer emails, links to that email
//     that look like any other, in a panel no click navigates.
//
// gitbolt::test::shownText() is what a label or a tooltip puts on the
// screen. The x-test: links are caught by a URL handler of the test's,
// so nothing reaches the desktop. Settings live in a folder of the
// test's own (QSettings::setPath), and git's global configuration is a
// file there with an identity and nothing else.
//

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>

#include <functional>

#include "../ShownText.h"
#include "../TestRepoHelper.h"
#include "conf/SettingsService.h"
#include "dialogs/CherryPickDialog.h"
#include "dialogs/CloneDialog.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/TextEditorDialog.h"
#include "git/GitProcess.h"
#include "models/CommitLogModel.h"
#include "models/FileStatusModel.h"
#include "services/GitService.h"
#include "ui/DashboardView.h"
#include "ui/InlineOpIndicator.h"
#include "ui/MainWindow.h"
#include "ui/RepositoryView.h"
#include "util/ToolTip.h"
#include "widgets/BlameWidget.h"
#include "widgets/BranchTreeWidget.h"
#include "widgets/DiffViewerWidget.h"
#include "widgets/InteractiveRebaseWidget.h"
#include "widgets/PlainTextDialogs.h"
#include "widgets/SubmoduleWidget.h"
#include "widgets/WorktreeWidget.h"

using gitbolt::services::GitService;
using gitbolt::test::shownText;
using gitbolt::test::shownToolTip;
using gitbolt::ui::MainWindow;
using gitbolt::widgets::PlainInputDialog;
using gitbolt::widgets::PlainMessageBox;

namespace {

const QString kBold = QStringLiteral("<b>MARKER</b>");
const QString kLink = QStringLiteral("<a href=\"x-test:1\">L</a>");

bool succeeded(const gitbolt::git::Result<gitbolt::git::ProcessOutput>& r) {
    return r.ok() && r.value().success();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

bool writeHook(const QString& gitDir, const char* name, const QByteArray& body) {
    const QString path = gitDir + QStringLiteral("/hooks/") + QLatin1String(name);
    return writeFile(path, "#!/bin/sh\n" + body)
        && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

// Takes the x-test: URLs QDesktopServices would hand the desktop.
class UrlCatcher : public QObject {
    Q_OBJECT
public:
    // mailto: too: the Commit tab sends an email's link there.
    UrlCatcher() {
        QDesktopServices::setUrlHandler(QStringLiteral("x-test"), this, "open");
        QDesktopServices::setUrlHandler(QStringLiteral("mailto"), this, "open");
    }
    ~UrlCatcher() override {
        QDesktopServices::unsetUrlHandler(QStringLiteral("x-test"));
        QDesktopServices::unsetUrlHandler(QStringLiteral("mailto"));
    }
    QList<QUrl> urls;
    QString seen() const { return QUrl::toStringList(urls).join(QLatin1Char(' ')); }

public slots:
    void open(const QUrl& url) { urls << url; }
};

// Clicks all over `label`, as a reader clicking its text would: on a
// link, if it shows one, and the label opens links, the URL goes to
// QDesktopServices.
void clickAcross(QLabel* label) {
    for (int y = 2; y < label->height(); y += 4) {
        for (int x = 2; x < label->width(); x += 6)
            QTest::mouseClick(label, Qt::LeftButton, {}, QPoint(x, y));
    }
}

// The label of `widget`'s whose text is `text`.
QLabel* labelSaying(QWidget& widget, const QString& text) {
    for (QLabel* label : widget.findChildren<QLabel*>()) {
        if (label->text() == text)
            return label;
    }
    return nullptr;
}

// The label holding a dialog's text: a message box's, or an input
// dialog's prompt.
QLabel* textLabelOf(QDialog* dialog) {
    if (qobject_cast<QMessageBox*>(dialog))
        return dialog->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
    return dialog->findChild<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
}

// The size of the window QToolTip shows `tip` in: its label, as it lays
// the text out.
QSize toolTipSize(const QString& tip) {
    QToolTip::showText(QPoint(20, 20), tip);
    QSize size;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        if (top->inherits("QTipLabel") && top->isVisible())
            size = top->size();
    }
    QToolTip::hideText();
    return size;
}

// A link in a rich text document: its text, where it goes, how it looks
// and where it starts.
struct Link {
    QString text;
    QString href;
    QTextCharFormat format;
    int position = 0;
};

QList<Link> linksIn(const QTextDocument* document) {
    QList<Link> links;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor())
                links.append({fragment.text(), fragment.charFormat().anchorHref(),
                              fragment.charFormat(), fragment.position()});
        }
    }
    return links;
}

// What the next modal dialog said, and how: its text label's format and
// what it showed, clicked all over first. Then it is answered: `button`
// on a message box, Cancel on any other dialog.
class ModalProbe {
public:
    explicit ModalProbe(QMessageBox::StandardButton button = QMessageBox::Cancel) {
        timer_.setInterval(20);
        QObject::connect(&timer_, &QTimer::timeout, [this, button] {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!modal || !modal->isVisible())
                return;
            timer_.stop();
            up = true;
            if (auto* box = qobject_cast<QMessageBox*>(modal)) {
                icon = box->icon();
                buttons = box->standardButtons();
                defaultButton = box->defaultButton()
                    ? box->standardButton(box->defaultButton()) : QMessageBox::NoButton;
            }
            if (QLabel* label = textLabelOf(modal)) {
                format = label->textFormat();
                shown = shownText(label);
                clickAcross(label);
            }
            auto* box = qobject_cast<QMessageBox*>(modal);
            if (QAbstractButton* answer = box ? box->button(button) : nullptr)
                answer->click();
            else
                modal->reject();
        });
        timer_.start();
    }

    bool up = false;
    Qt::TextFormat format = Qt::AutoText;
    QString shown;
    QMessageBox::Icon icon = QMessageBox::NoIcon;
    QMessageBox::StandardButtons buttons;
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton;

private:
    QTimer timer_;
};

} // namespace

class TestPlainText : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // As main() has it: the style decides how a message box's text
        // takes clicks.
        QApplication::setStyle(QStringLiteral("fusion"));
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        const QString global = settingsDir_.filePath(QStringLiteral("gitconfig"));
        QVERIFY(writeFile(global, "[user]\n\tname = T\n\temail = t@example.com\n"
                                  "[commit]\n\tgpgSign = false\n"
                                  "[init]\n\tdefaultBranch = main\n"));
        qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(global));
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }

    // A box's text with a link and a bold tag in it, as a submodule's
    // name can have them. Built as QMessageBox::question() builds it,
    // the box draws the link, and a click on it opens the URL. Through
    // PlainMessageBox, it shows the text as written, and nothing opens;
    // its icon, buttons and default button are the ones QMessageBox
    // gives the same call.
    void messageBoxShowsItsTextAsWritten() {
        const QString text = QStringLiteral("Deinit '%1 %2'?").arg(kLink, kBold);
        UrlCatcher desktop;

        ModalProbe rich(QMessageBox::No);
        QCOMPARE(QMessageBox::question(nullptr, QStringLiteral("Deinit Submodule"), text,
                                       QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                                       QMessageBox::No),
                 QMessageBox::No);
        QVERIFY(rich.up);
        QCOMPARE(rich.shown, QStringLiteral("Deinit 'L MARKER'?"));
        QVERIFY2(!desktop.urls.isEmpty(), "the clicks never reached the link");
        desktop.urls.clear();

        ModalProbe plain(QMessageBox::No);
        QCOMPARE(PlainMessageBox::question(nullptr, QStringLiteral("Deinit Submodule"), text,
                                           QMessageBox::Yes | QMessageBox::No
                                               | QMessageBox::Cancel,
                                           QMessageBox::No),
                 QMessageBox::No);
        QVERIFY(plain.up);
        QCOMPARE(plain.format, Qt::PlainText);
        QCOMPARE(plain.shown, text);
        QVERIFY2(desktop.urls.isEmpty(), qPrintable(desktop.seen()));
        QCOMPARE(plain.icon, rich.icon);
        QCOMPARE(plain.buttons, rich.buttons);
        QCOMPARE(plain.defaultButton, rich.defaultButton);

        // warning() and information(), with their default buttons.
        for (const bool warning : {true, false}) {
            ModalProbe probe(QMessageBox::Ok);
            const auto answer = warning
                ? PlainMessageBox::warning(nullptr, QStringLiteral("Push Failed"), text)
                : PlainMessageBox::information(nullptr, QStringLiteral("Push Failed"), text);
            QCOMPARE(answer, QMessageBox::Ok);
            QCOMPARE(probe.format, Qt::PlainText);
            QCOMPARE(probe.shown, text);
            QCOMPARE(probe.icon, warning ? QMessageBox::Warning : QMessageBox::Information);
            QCOMPARE(probe.buttons, QMessageBox::StandardButtons(QMessageBox::Ok));
            QCOMPARE(probe.defaultButton, QMessageBox::Ok);
        }
        QVERIFY(desktop.urls.isEmpty());
    }

    // The askpass prompt names the URL git asks about, and the user name
    // in it can be the server's (a redirect to http://<it>@host/…); it
    // was rendered, where the user decides whom to give a password. An
    // "&" in it, or in a branch's name, is shown too: the label is the
    // field's buddy, which took one for a mnemonic's mark and hid it.
    void inputDialogPromptShowsItsTextAsWritten() {
        const QString prompt =
            QStringLiteral("Password for 'http://%1&b@127.0.0.1:1': ").arg(kBold);
        QInputDialog dialog;
        PlainInputDialog::setLabelText(dialog, prompt);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* label = textLabelOf(&dialog);
        QVERIFY(label);
        QVERIFY(label->buddy());
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QCOMPARE(shownText(label), prompt);
        // Set again, the dialog's one label keeps its format.
        const QString again = kBold + QStringLiteral(" &&&");
        PlainInputDialog::setLabelText(dialog, again);
        QCOMPARE(dialog.findChildren<QLabel*>().size(), 1);
        QCOMPARE(shownText(label), again);
        dialog.close();

        // getText(), as the sidebar asks for a branch's new name.
        ModalProbe rename;
        bool ok = true;
        PlainInputDialog::getText(nullptr, QStringLiteral("Rename Branch"),
                                  QStringLiteral("New name for 'a&b':"), QLineEdit::Normal,
                                  QStringLiteral("a&b"), &ok);
        QVERIFY(rename.up);
        QVERIFY(!ok);
        QCOMPARE(rename.format, Qt::PlainText);
        QCOMPARE(rename.shown, QStringLiteral("New name for 'a&b':"));
    }

    // The toolbar's narration of an op: a failure's first line and its
    // tooltip are git's, a server's lines among them. Shown as written,
    // line breaks and indentation kept in the tooltip; one elided keeps
    // its whole text there.
    void opIndicatorShowsGitsTextAsWritten() {
        QLabel label;
        gitbolt::ui::InlineOpIndicator indicator(&label);
        const QString full =
            QStringLiteral("remote: %1 %2\nremote:     indented\nerror: failed")
                .arg(kBold, kLink);
        indicator.fail(QStringLiteral("remote: %1").arg(kBold), full, 0);
        QCOMPARE(label.textFormat(), Qt::PlainText);
        QCOMPARE(shownText(&label), QStringLiteral("✗ remote: %1").arg(kBold));
        QCOMPARE(shownToolTip(label.toolTip()), full);

        indicator.succeed(kBold);
        QCOMPARE(shownText(&label), QStringLiteral("✓ %1").arg(kBold));
        QCOMPARE(label.toolTip(), QString());

        const QString longOne = kBold + QString(200, QLatin1Char('x'));
        indicator.start(longOne);
        QVERIFY(shownText(&label).startsWith(kBold));
        QCOMPARE(shownToolTip(label.toolTip()), longOne);
    }

    // A window with a branch "<b>MARKER</b>" checked out, whose push a
    // server's hook turns down with markup of its own, says all of it as
    // written: the status bar's branch, the toolbar's failure and its
    // tooltip, the sidebar's Rename prompt and Delete question, and
    // Manage Submodules' Deinit question; and no click on them opens
    // anything.
    void windowShowsTheRepositorysTextAsWritten() {
        // In a folder "<b>MARKER</b>" (two, as the name has a slash).
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString repoPath = scratch.filePath(kBold);
        QVERIFY(QDir().mkpath(repoPath));
        const QString other = QStringLiteral("<i>other</i>");
        gitbolt::git::GitProcess git(repoPath.toStdString());
        QVERIFY(succeeded(git.run({"init", "-q", "-b", kBold.toStdString()})));
        QVERIFY(succeeded(git.run({"commit", "-q", "--allow-empty", "-m", "base"})));
        QVERIFY(succeeded(git.run({"branch", other.toStdString()})));
        const QString originPath = scratch.filePath(QStringLiteral("origin.git"));
        QVERIFY(succeeded(git.run({"init", "-q", "--bare", originPath.toStdString()})));
        QVERIFY(succeeded(git.run({"remote", "add", "origin", originPath.toStdString()})));
        QVERIFY(writeHook(originPath, "pre-receive",
                          "echo '<b>MARKER</b> <a href=\"x-test:1\">L</a>' >&2\n"
                          "echo '    indented' >&2\n"
                          "exit 1\n"));
        UrlCatcher desktop;

        MainWindow window;
        auto* svc = openIn(window, repoPath);
        QVERIFY(svc);
        QLabel* branchLabel = nullptr;
        QLabel* pathLabel = nullptr;
        for (QLabel* label : window.statusBar()->findChildren<QLabel*>()) {
            if (label->text().startsWith(QStringLiteral("Branch: ")))
                branchLabel = label;
            else if (label->text().contains(kBold))
                pathLabel = label;
        }
        QVERIFY(branchLabel && pathLabel);
        QCOMPARE(shownText(branchLabel), QStringLiteral("Branch: ") + kBold);
        QCOMPARE(shownText(pathLabel), pathLabel->text());

        auto* opLabel = window.findChild<QLabel*>(QStringLiteral("toolbar.opIndicator"));
        QVERIFY(opLabel);
        action(window, "act.push")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(opLabel->text().startsWith(QStringLiteral("✗")), 20000);
        QCOMPARE(opLabel->textFormat(), Qt::PlainText);
        QVERIFY2(shownText(opLabel).contains(QStringLiteral("remote: ") + kBold),
                 qPrintable(shownText(opLabel)));
        // git pads a server's lines with spaces; its line breaks and
        // the server's indentation stay.
        const QString tip = shownToolTip(opLabel->toolTip());
        QVERIFY2(tip.contains(QStringLiteral("remote: %1 %2").arg(kBold, kLink)),
                 qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("\nremote:     indented")), qPrintable(tip));
        clickAcross(opLabel);

        auto* tree = window.findChild<gitbolt::widgets::BranchTreeWidget*>();
        QVERIFY(tree);
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "act.commit")->isEnabled(), 15000);
        {
            ModalProbe rename;
            emit tree->renameBranchRequested(other, tree->repositoryKey());
            QVERIFY(rename.up);
            QCOMPARE(rename.format, Qt::PlainText);
            QCOMPARE(rename.shown, QStringLiteral("New name for '%1':").arg(other));
        }
        {
            ModalProbe newBranch;
            emit tree->createBranchRequested(other, tree->repositoryKey());
            QVERIFY(newBranch.up);
            QCOMPARE(newBranch.format, Qt::PlainText);
            QCOMPARE(newBranch.shown,
                     QStringLiteral("Name of the new branch from '%1':").arg(other));
        }
        {
            ModalProbe upstream;
            emit tree->setUpstreamRequested(other, tree->repositoryKey());
            QVERIFY(upstream.up);
            QCOMPARE(upstream.format, Qt::PlainText);
            QCOMPARE(upstream.shown,
                     QStringLiteral("Upstream for '%1' (e.g., origin/%1):").arg(other));
        }
        {
            ModalProbe question(QMessageBox::No);
            emit tree->deleteBranchRequested(other, tree->repositoryKey());
            QVERIFY(question.up);
            QCOMPARE(question.format, Qt::PlainText);
            QCOMPARE(question.shown,
                     QStringLiteral("Are you sure you want to delete branch '%1'?").arg(other));
        }
        // Both answered No: the branch is there under its own name.
        QVERIFY(succeeded(git.run({"rev-parse", "--verify", "-q",
                                   "refs/heads/" + other.toStdString()})));

        action(window, "repository.manage-submodules")->trigger();
        gitbolt::widgets::SubmoduleWidget* submodules = nullptr;
        QTRY_VERIFY((submodules = window.findChild<gitbolt::widgets::SubmoduleWidget*>())
                    != nullptr);
        {
            const QString name = kLink + QLatin1Char(' ') + kBold;
            ModalProbe question(QMessageBox::No);
            emit submodules->deinitRequested(name);
            QVERIFY(question.up);
            QCOMPARE(question.format, Qt::PlainText);
            QVERIFY2(question.shown.startsWith(QStringLiteral("Deinit '%1'?\n\n").arg(name)),
                     qPrintable(question.shown));
        }
        {
            // git turns the name down, quoting it first thing: the
            // failure's box shows git's words as written.
            const QString name = kLink + QLatin1Char(' ') + kBold;
            ModalProbe failure(QMessageBox::Ok);
            emit submodules->initRequested(name);
            QTRY_VERIFY_WITH_TIMEOUT(failure.up, 15000);
            QCOMPARE(failure.format, Qt::PlainText);
            QVERIFY2(failure.shown.contains(QStringLiteral("'%1'").arg(name)),
                     qPrintable(failure.shown));
        }
        if (auto* dialog = qobject_cast<QDialog*>(submodules->window()))
            dialog->close();
        QVERIFY2(desktop.urls.isEmpty(), qPrintable(desktop.seen()));
    }

    // The Clone dialog's status names the destination, whose folder
    // names can be anything; and it shows git's error, which starts
    // with a server's ("fatal: remote error: <font …>").
    void cloneStatusShowsItsTextAsWritten() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString target = scratch.filePath(kBold);
        QVERIFY(writeFile(target + QStringLiteral("/marker.txt"), "x\n"));

        gitbolt::dialogs::CloneDialog dialog;
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("clone.url"));
        auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("clone.path"));
        auto* status = dialog.findChild<QLabel*>(QStringLiteral("clone.status"));
        QVERIFY(url && path && status);
        url->setText(scratch.filePath(QStringLiteral("nowhere")));
        path->setText(target);
        QPushButton* clone = nullptr;
        for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Clone"))
                clone = button;
        }
        QVERIFY(clone);
        clone->click();
        QVERIFY(status->isVisible());
        QCOMPARE(status->textFormat(), Qt::PlainText);
        QCOMPARE(shownText(status),
                 QStringLiteral("Destination already exists and is not empty: %1").arg(target));
    }

    // A commit's failure is shown in git's words, a hook's output among
    // them; the footer names the branch.
    void commitErrorShowsGitsTextAsWritten() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit(QStringLiteral("a.txt"), "a\n", QStringLiteral("a"));
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createBranch(kBold.toStdString(), *head).ok());
        QVERIFY(repo.repo().checkout(kBold.toStdString()).ok());
        QVERIFY(writeHook(repo.path() + QStringLiteral("/.git"), "pre-commit",
                          "echo '<b>MARKER</b> <a href=\"x-test:1\">L</a>' >&2\nexit 1\n"));
        repo.writeFile(QStringLiteral("b.txt"), "b\n");
        QVERIFY(repo.stageFile(QStringLiteral("b.txt")).ok());

        GitService svc;
        QVERIFY(svc.openRepository(repo.path()));
        gitbolt::dialogs::CommitDialog dialog(&svc, nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* message = dialog.findChild<QPlainTextEdit*>(QStringLiteral("commit.message"));
        auto* commit = dialog.findChild<QPushButton*>(QStringLiteral("commit.commitBtn"));
        auto* error = dialog.findChild<QLabel*>(QStringLiteral("commit.error"));
        QVERIFY(message && commit && error);
        QLabel* branch = nullptr;
        QTRY_VERIFY((branch = labelSaying(dialog, kBold)) != nullptr);
        QCOMPARE(shownText(branch), kBold);
        message->setPlainText(QStringLiteral("refused"));
        commit->click();
        QTRY_VERIFY_WITH_TIMEOUT(error->isVisible(), 15000);
        QCOMPARE(error->textFormat(), Qt::PlainText);
        QVERIFY2(shownText(error).contains(QStringLiteral("%1 %2").arg(kBold, kLink)),
                 qPrintable(shownText(error)));
    }

    // The labels that name a file, or show a commit's message and
    // author.
    void labelsShowTheRepositorysTextAsWritten() {
        gitbolt::dialogs::TextEditorDialog editor;
        const QString ignore = QStringLiteral("/tmp/%1/.gitignore").arg(kBold);
        editor.setLabel(ignore);
        QLabel* label = labelSaying(editor, ignore);
        QVERIFY(label);
        QCOMPARE(shownText(label), ignore);

        gitbolt::dialogs::CherryPickDialog pick;
        gitbolt::git::CommitData commit;
        commit.summary = kLink.toStdString();
        commit.author.name = kBold.toStdString();
        commit.author.email = "a@b";
        pick.setCommitDetails(commit);
        QVERIFY((label = labelSaying(pick, kLink)) != nullptr);
        QCOMPARE(shownText(label), kLink);
        const QString author = kBold + QStringLiteral(" <a@b>");
        QVERIFY((label = labelSaying(pick, author)) != nullptr);
        QCOMPARE(shownText(label), author);

        const QString file = kBold + QStringLiteral("/a.txt");
        gitbolt::widgets::BlameWidget blame;
        gitbolt::git::BlameResult result;
        result.path = file.toStdString();
        blame.setBlameResult(result);
        QVERIFY((label = labelSaying(blame, file)) != nullptr);
        QCOMPARE(shownText(label), file);

        gitbolt::widgets::DiffViewerWidget diff;
        gitbolt::git::DiffFileEntry entry;
        entry.status = gitbolt::git::DiffStatus::Added;
        entry.newPath = file.toStdString();
        diff.setDiffForFile(entry);
        label = diff.findChild<QLabel*>(QStringLiteral("diff.fileLabel"));
        QVERIFY(label);
        QCOMPARE(shownText(label), file);
    }

    // Tooltips that carry a repository's paths and names, or a commit's
    // message, show them as written, line breaks kept.
    void listToolTipsShowTheRepositorysTextAsWritten() {
        gitbolt::models::FileStatusModel files;
        gitbolt::git::StatusEntry entry;
        entry.path = (kBold + QStringLiteral("/a.txt")).toStdString();
        entry.status = gitbolt::git::FileStatus::WtNew;
        files.setEntries({entry});
        QCOMPARE(files.rowCount(), 1);
        QCOMPARE(shownToolTip(files.index(0, gitbolt::models::FileStatusModel::Path)
                                  .data(Qt::ToolTipRole).toString()),
                 kBold + QStringLiteral("/a.txt"));

        gitbolt::widgets::SubmoduleTableModel submodules;
        gitbolt::git::SubmoduleInfo sub;
        sub.name = kBold.toStdString();
        sub.path = "%2";
        sub.url = kLink.toStdString();
        sub.status = gitbolt::git::SubmoduleStatus::Clean;
        submodules.setSubmodules({sub});
        const QString subTip =
            shownToolTip(submodules.index(0, 0).data(Qt::ToolTipRole).toString());
        QVERIFY2(subTip.startsWith(QStringLiteral("%1\nPath: %2\nURL: %3\nStatus: ")
                                       .arg(kBold, QStringLiteral("%2"), kLink)),
                 qPrintable(subTip));

        gitbolt::widgets::WorktreeTableModel worktrees;
        gitbolt::git::WorktreeInfo wt;
        wt.name = kBold.toStdString();
        wt.path = "/tmp/" + kLink.toStdString();
        wt.branch = kBold.toStdString();
        worktrees.setWorktrees({wt});
        QCOMPARE(shownToolTip(worktrees.index(0, 0).data(Qt::ToolTipRole).toString()),
                 QStringLiteral("%1\nPath: /tmp/%2\nBranch: %1\nUnlocked").arg(kBold, kLink));

        gitbolt::widgets::RebaseListModel rebase;
        gitbolt::git::RebaseOperation op;
        op.type = gitbolt::git::RebaseOperationType::Reword;
        op.message = "subject\n";
        op.newMessage = (kBold + QStringLiteral("\n\n    body ") + kLink + QStringLiteral("\n"))
                            .toStdString();
        rebase.setOperations({op});
        QCOMPARE(shownToolTip(rebase.index(0, 0).data(Qt::ToolTipRole).toString()),
                 QStringLiteral("Keep the changes, with a new message:\n\n%1\n\n    body %2")
                     .arg(kBold, kLink));
    }

    // The home screen's recent list: a row's tooltip is the folder's
    // path, whatever its folders are called.
    void recentListToolTipShowsThePathAsWritten() {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString folder = QDir::toNativeSeparators(scratch.filePath(kBold));
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(succeeded(gitbolt::git::GitProcess(folder.toStdString()).run({"init", "-q"})));
        gitbolt::conf::SettingsService settings;
        settings.clearRecentRepositories();
        settings.addRecentRepository(folder);

        gitbolt::ui::DashboardView view(&settings);
        auto* list = view.findChild<QTreeWidget*>();
        QVERIFY(list);
        QCOMPARE(list->topLevelItemCount(), 1);
        const QString path = list->topLevelItem(0)->data(0, Qt::UserRole).toString();
        QVERIFY(path.endsWith(kBold));
        for (int column = 0; column < list->columnCount(); ++column)
            QCOMPARE(shownToolTip(list->topLevelItem(0)->toolTip(column)), path);
        // Its probe lands before the view goes.
        QTRY_VERIFY_WITH_TIMEOUT(view.findChildren<QFutureWatcherBase*>().isEmpty(), 15000);
        settings.clearRecentRepositories();
    }

    // A tooltip keeps the shape it had. Text Qt shows as plain text is
    // left as it is, and laid out as before; escaped text keeps the
    // lines plain text would give it. A tooltip that wraps rich text
    // breaks it into a narrow block wherever it can: a branch's three
    // lines into five or more, a path into two or four.
    void toolTipsKeepTheirShape() {
        using gitbolt::util::plainToolTip;
        const QStringList ordinary = {
            QStringLiteral("refs/heads/feature/some-long-branch-name\n"
                           "Upstream: origin/feature/some-long-branch-name\n(HEAD)"),
            QStringLiteral("src/widgets/InteractiveRebaseWidget.cpp"),
            QStringLiteral("/Users/someone/Developer/My Projects/some repository"),
            QStringLiteral("fatal: unable to access 'https://example.invalid/repo.git/': "
                           "Could not resolve host: example.invalid"),
        };
        for (const QString& text : ordinary) {
            const QSize plain = toolTipSize(text);
            QVERIFY(plain.isValid());
            QCOMPARE(toolTipSize(plainToolTip(text)), plain);
            QCOMPARE(plainToolTip(text), text);
        }

        // With a tag in its first line, escaped: shown as written, in
        // as many lines as the same text with other brackets, which Qt
        // takes for plain text.
        const QString marked =
            QStringLiteral("remote: %1 a line as long as a server's can be, with words to "
                           "break at\nremote:     indented")
                .arg(kBold);
        const QString unmarked =
            QString(marked).replace(QLatin1Char('<'), QLatin1Char('[')).replace(
                QLatin1Char('>'), QLatin1Char(']'));
        QVERIFY(Qt::mightBeRichText(marked));
        QVERIFY(!Qt::mightBeRichText(unmarked));
        QCOMPARE(shownToolTip(plainToolTip(marked)), marked);
        QCOMPARE(toolTipSize(plainToolTip(marked)).height(), toolTipSize(unmarked).height());
        QCOMPARE(plainToolTip(QString()), QString());
    }

    // The Commit tab's author and committer: an email is shown as
    // written, as a link to it that looks like any other. An email with
    // a ' in it closed the link's single-quoted href and went on with
    // attributes of its own, a style and an href to anywhere. And a
    // link clicked there leaves the panel as it is: QTextBrowser went
    // to the href itself, which blanked the panel. An email's link goes
    // to the mail app, with that email and nothing else.
    void commitDetailsShowTheEmailsAsWritten() {
        using gitbolt::git::ObjectId;
        const QString authorEmail = QStringLiteral(
            "m@example.com' style='font-size:40px;color:#00aa00' href='x-test:author");
        const QString committerEmail =
            QStringLiteral("c@example.com' style='font-size:40px' href='x-test:committer");
        gitbolt::git::CommitData marked;
        marked.id = ObjectId::fromHex(std::string(40, '1'));
        marked.parentIds = {ObjectId::fromHex(std::string(40, '2'))};
        marked.summary = "subject";
        marked.message = "subject\n";
        marked.author.name = "M";
        marked.author.email = authorEmail.toStdString();
        marked.committer.name = "C";
        marked.committer.email = committerEmail.toStdString();
        // The same commit by people with ordinary emails: how a link
        // looks.
        gitbolt::git::CommitData ordinary = marked;
        ordinary.id = ObjectId::fromHex(std::string(40, '3'));
        ordinary.author.email = "m@example.com";
        ordinary.committer.email = "c@example.com";

        gitbolt::models::CommitLogModel model;
        model.setCommits({marked, ordinary});
        gitbolt::ui::RepositoryView view;
        view.setCommitLogModel(&model);
        view.resize(1000, 700);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto* details = view.findChild<QTextBrowser*>(QStringLiteral("commit.detailsBrowser"));
        auto* message = view.findChild<QTextBrowser*>(QStringLiteral("commit.messageBrowser"));
        QVERIFY(details && message);

        view.onCommitSelected(QString::fromStdString(ordinary.id.toHex()));
        const QList<Link> looks = linksIn(details->document());
        view.onCommitSelected(QString::fromStdString(marked.id.toHex()));
        const QList<Link> links = linksIn(details->document());
        // The author's, the committer's and the parent's.
        QCOMPARE(looks.size(), 3);
        QCOMPARE(links.size(), 3);
        for (int i = 0; i < 2; ++i) {
            const QString email = i == 0 ? authorEmail : committerEmail;
            QCOMPARE(links[i].text, email);
            const QUrl target(links[i].href, QUrl::StrictMode);
            QCOMPARE(target.scheme(), QStringLiteral("mailto"));
            QCOMPARE(target.path(QUrl::FullyDecoded), email);
            QTextCharFormat format = links[i].format;
            QTextCharFormat look = looks[i].format;
            format.clearProperty(QTextFormat::AnchorHref);
            look.clearProperty(QTextFormat::AnchorHref);
            QVERIFY2(format == look, qPrintable(QStringLiteral("%1px %2")
                                                    .arg(format.font().pixelSize())
                                                    .arg(format.foreground().color().name())));
        }

        UrlCatcher desktop;
        QVERIFY(details->isVisible());
        const QString shown = details->toPlainText();
        for (const Link& link : links) {
            QTextCursor cursor(details->document());
            cursor.setPosition(link.position + 1);
            const QPoint at = details->cursorRect(cursor).center();
            QCOMPARE(details->anchorAt(at), link.href);
            QTest::mouseClick(details->viewport(), Qt::LeftButton, {}, at);
            QCOMPARE(details->source(), QUrl());
            QCOMPARE(details->toPlainText(), shown);
        }
        QCOMPARE(desktop.urls.size(), 2);
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(desktop.urls[i].scheme(), QStringLiteral("mailto"));
            QCOMPARE(desktop.urls[i].path(QUrl::FullyDecoded), i == 0 ? authorEmail : committerEmail);
        }
        QVERIFY(!message->openLinks());
    }

private:
    // Opens `path` the way a second launch's hand-off does, and returns
    // the window's GitService once the repository is on screen.
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

QTEST_MAIN(TestPlainText)
#include "TestPlainText.moc"
