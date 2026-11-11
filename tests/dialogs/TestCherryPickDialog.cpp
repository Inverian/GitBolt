//
// TestCherryPickDialog — Commands › Cherry pick and Revert commit's
// dialog, off screen, its host's lookups played by the test:
//
//   - each mode's title, OK button and field names;
//   - a commit filled in (the log's selection, a row's context menu)
//     asks the host for its preview, and OK is on only while there is
//     one: off for nothing, too short a text, or a revision that didn't
//     resolve;
//   - a revert of a merge asks which parent (the first by default),
//     the parents' subjects shown as written; a commit that isn't a
//     merge, and any cherry-pick, has no such row and no mainline.
//
// Settings live in a folder of the test's own (QSettings::setPath).
//

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "dialogs/CherryPickDialog.h"

using gitbolt::dialogs::CherryPickDialog;
using Mode = CherryPickDialog::Mode;

namespace {

gitbolt::git::ObjectId idOf(char digit) {
    return gitbolt::git::ObjectId::fromHex(std::string(40, digit));
}

// A commit with `parents` parents (ids 1…, 2…), "Fix login" by Ann.
gitbolt::git::CommitData commitWith(size_t parents) {
    gitbolt::git::CommitData commit;
    commit.id = idOf('a');
    for (size_t i = 0; i < parents; ++i)
        commit.parentIds.push_back(idOf(static_cast<char>('1' + i)));
    commit.summary = "Fix login";
    commit.message = "Fix login\n";
    commit.author.name = "Ann";
    commit.author.email = "ann@example.com";
    return commit;
}

QPushButton* okButton(CherryPickDialog& dialog) {
    auto* box = dialog.findChild<QDialogButtonBox*>();
    return box ? box->button(QDialogButtonBox::Ok) : nullptr;
}

} // namespace

class TestCherryPickDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
    }

    void modesSayWhatTheyDo_data() {
        QTest::addColumn<bool>("revert");
        QTest::newRow("cherry-pick") << false;
        QTest::newRow("revert") << true;
    }

    void modesSayWhatTheyDo() {
        QFETCH(bool, revert);
        CherryPickDialog dialog(nullptr, revert ? Mode::Revert : Mode::CherryPick);
        QCOMPARE(dialog.mode(), revert ? Mode::Revert : Mode::CherryPick);
        QCOMPARE(dialog.windowTitle(),
                 revert ? QStringLiteral("Revert Commit") : QStringLiteral("Cherry-Pick Commit"));
        QVERIFY(okButton(dialog));
        QCOMPARE(okButton(dialog)->text(),
                 revert ? QStringLiteral("Revert") : QStringLiteral("Cherry-Pick"));
        QVERIFY(dialog.findChild<QLineEdit*>(revert ? QStringLiteral("revert.commit")
                                                    : QStringLiteral("cherryPick.commit")));
        QCOMPARE(dialog.findChild<QComboBox*>(QStringLiteral("revert.parent")) != nullptr, revert);
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("revert.note")) != nullptr, revert);
    }

    // The host's part: commitHashChanged, answered with a preview or
    // none. OK follows it.
    void filledInCommitAsksForItsPreview() {
        CherryPickDialog dialog(nullptr, Mode::Revert);
        QSignalSpy asked(&dialog, &CherryPickDialog::commitHashChanged);
        QVERIFY(!okButton(dialog)->isEnabled());

        dialog.setCommitHash(QStringLiteral("  0123abcd  "));
        QCOMPARE(asked.count(), 1);
        QCOMPARE(asked.at(0).at(0).toString(), QStringLiteral("0123abcd"));
        QCOMPARE(dialog.commitHash(), QStringLiteral("0123abcd"));
        QVERIFY(!okButton(dialog)->isEnabled());   // no preview yet
        dialog.setCommitDetails(commitWith(1));
        QVERIFY(okButton(dialog)->isEnabled());
        const auto labels = dialog.findChildren<QLabel*>();
        QVERIFY(std::any_of(labels.begin(), labels.end(), [](QLabel* l) {
            return l->text() == QStringLiteral("Fix login");
        }));

        // A revision that didn't resolve: the host clears the preview.
        dialog.setCommitHash(QStringLiteral("nosuchref"));
        dialog.clearCommitDetails();
        QVERIFY(!okButton(dialog)->isEnabled());

        // Too short to look up: nothing asked, no preview.
        asked.clear();
        dialog.setCommitDetails(commitWith(1));
        dialog.setCommitHash(QStringLiteral("ab"));
        QCOMPARE(asked.count(), 0);
        QVERIFY(!okButton(dialog)->isEnabled());
    }

    // git revert -m: a merge's revert names the parent it is reverted
    // against, the first unless the user picks another.
    void mergesRevertAsksForAParent() {
        CherryPickDialog dialog(nullptr, Mode::Revert);
        auto* parent = dialog.findChild<QComboBox*>(QStringLiteral("revert.parent"));
        QVERIFY(parent);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QVERIFY(!parent->isVisible());
        QCOMPARE(dialog.mainline(), 0u);

        dialog.setCommitDetails(commitWith(1));
        QVERIFY(!parent->isVisible());
        QCOMPARE(dialog.mainline(), 0u);

        dialog.setCommitDetails(commitWith(2), {QStringLiteral("1111111 main work"),
                                                QStringLiteral("2222222 <b>side</b> work")});
        QVERIFY(parent->isVisible());
        QCOMPARE(parent->count(), 2);
        QCOMPARE(parent->itemText(0), QStringLiteral("1: 1111111 main work"));
        QCOMPARE(parent->itemText(1), QStringLiteral("2: 2222222 <b>side</b> work"));
        QCOMPARE(dialog.mainline(), 1u);
        parent->setCurrentIndex(1);
        QCOMPARE(dialog.mainline(), 2u);

        // Without the host's subjects, the parents' ids.
        dialog.setCommitDetails(commitWith(3));
        QCOMPARE(parent->count(), 3);
        QCOMPARE(parent->itemText(2), QStringLiteral("3: 3333333"));

        // Another commit, or none: the row goes.
        dialog.clearCommitDetails();
        QVERIFY(!parent->isVisible());
        QCOMPARE(dialog.mainline(), 0u);
        dialog.setCommitDetails(commitWith(0));
        QVERIFY(!parent->isVisible());
        QCOMPARE(dialog.mainline(), 0u);
    }

    void cherryPickHasNoParentChooser() {
        CherryPickDialog dialog(nullptr, Mode::CherryPick);
        dialog.setCommitDetails(commitWith(2));
        QVERIFY(!dialog.findChild<QComboBox*>());
        QCOMPARE(dialog.mainline(), 0u);
        QVERIFY(okButton(dialog)->isEnabled());
    }

private:
    QTemporaryDir settingsDir_;
};

QTEST_MAIN(TestCherryPickDialog)
#include "TestCherryPickDialog.moc"
