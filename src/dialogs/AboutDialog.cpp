#include "dialogs/AboutDialog.h"

#include "git/Repository.h"  // libgit2Version(), libgit2Features()

#include <QApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSysInfo>
#include <QVBoxLayout>
#include <QtGlobal>

namespace gitbolt::dialogs {

// ---------------------------------------------------------------------------
// Compile-time info helpers
// ---------------------------------------------------------------------------
//
// These return short strings describing the environment GitBolt was
// BUILT with, which is distinct from the environment it runs in.
// For example: Qt version reports QT_VERSION_STR (the header we
// compiled against), while Qt's runtime version could theoretically
// differ if a user LD_PRELOADs a newer Qt. In practice they match,
// but we're explicit about "built with" vs "runtime" in the dialog.

namespace {

QString compilerString() {
#if defined(__clang__)
    // Apple Clang reports itself as Apple LLVM; vanilla clang reports
    // regular clang version. Check __apple_build_version__ to
    // distinguish.
    #if defined(__apple_build_version__)
        return QStringLiteral("Apple Clang %1.%2.%3")
            .arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
    #else
        return QStringLiteral("Clang %1.%2.%3")
            .arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
    #endif
#elif defined(__GNUC__)
    return QStringLiteral("GCC %1.%2.%3")
        .arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return QStringLiteral("MSVC %1").arg(_MSC_VER);
#else
    return QStringLiteral("Unknown compiler");
#endif
}

QString buildTypeString() {
#if defined(QT_DEBUG)
    return QStringLiteral("Debug");
#else
    return QStringLiteral("Release");
#endif
}

QString buildDateString() {
    // __DATE__ and __TIME__ are set at compile time. They produce a
    // string like "Apr  6 2026 23:12:05" — good enough for "when was
    // this binary built" at a glance.
    return QStringLiteral(__DATE__ " " __TIME__);
}

} // namespace


// ---------------------------------------------------------------------------
// AboutDialog
// ---------------------------------------------------------------------------

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUi();
}

void AboutDialog::setupUi()
{
    setWindowTitle(tr("About GitBolt"));
    setModal(true);

    // ----- Icon column ----------------------------------------------------
    iconLabel_ = new QLabel(this);
    iconLabel_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    // The GitBolt icon lives in resources/icons/ and is shipped as
    // a family of .png sizes. We load the 128px version which is
    // big enough to look crisp on Retina without dominating the dialog.
    // If the resource isn't available (e.g. running an unbundled
    // debug binary that didn't embed the .qrc), we fall back to a
    // text placeholder so the dialog still renders.
    const QPixmap icon(QStringLiteral(":/icons/gitbolt-128.png"));
    if (!icon.isNull()) {
        iconLabel_->setPixmap(icon);
    } else {
        iconLabel_->setText(QStringLiteral("⚡"));
        QFont bigFont = iconLabel_->font();
        bigFont.setPointSize(64);
        iconLabel_->setFont(bigFont);
    }
    iconLabel_->setFixedWidth(140);

    // ----- Text column ----------------------------------------------------
    titleLabel_ = new QLabel(this);
    titleLabel_->setText(
        QStringLiteral("<div style='font-size:22pt; font-weight:bold;'>GitBolt</div>"
                       "<div style='font-size:10pt; color:gray;'>Version %1</div>")
            .arg(QApplication::applicationVersion()));
    titleLabel_->setTextFormat(Qt::RichText);

    taglineLabel_ = new QLabel(this);
    taglineLabel_->setText(tr("Fast cross-platform Git GUI client built with Qt 6 and libgit2."));
    taglineLabel_->setWordWrap(true);
    QFont tagFont = taglineLabel_->font();
    tagFont.setItalic(true);
    taglineLabel_->setFont(tagFont);

    // Horizontal separator between the title block and the detail block.
    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);

    // Detailed build info table. Lay it out as a rich-text HTML table
    // rather than a QFormLayout because (a) QFormLayout's alignment
    // quirks on macOS produce inconsistent spacing, and (b) rich text
    // gives us a single selectable block the user can copy-paste into
    // a bug report.
    buildInfoLabel_ = new QLabel(this);
    buildInfoLabel_->setTextFormat(Qt::RichText);
    buildInfoLabel_->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    buildInfoLabel_->setText(QStringLiteral(
        "<table cellpadding='2' cellspacing='0' style='font-size:10pt;'>"
        "<tr><td><b>Qt:</b></td><td>%1 (runtime %2)</td></tr>"
        "<tr><td><b>libgit2:</b></td><td>%3</td></tr>"
        "<tr><td><b>libgit2 features:</b></td><td>%4</td></tr>"
        "<tr><td><b>Compiler:</b></td><td>%5</td></tr>"
        "<tr><td><b>Build type:</b></td><td>%6</td></tr>"
        "<tr><td><b>Build date:</b></td><td>%7</td></tr>"
        "<tr><td><b>OS:</b></td><td>%8 (%9)</td></tr>"
        "<tr><td><b>Architecture:</b></td><td>%10</td></tr>"
        "</table>")
        .arg(QStringLiteral(QT_VERSION_STR))
        .arg(qVersion())
        .arg(QString::fromStdString(gitbolt::git::libgit2Version()))
        .arg(QString::fromStdString(gitbolt::git::libgit2Features()))
        .arg(compilerString())
        .arg(buildTypeString())
        .arg(buildDateString())
        .arg(QSysInfo::prettyProductName())
        .arg(QSysInfo::kernelVersion())
        .arg(QSysInfo::currentCpuArchitecture()));

    // License notice + clickable links. Using a single QLabel with
    // openExternalLinks=true lets the user click straight through to
    // GitHub / the license text in their browser.
    licenseLabel_ = new QLabel(this);
    licenseLabel_->setTextFormat(Qt::RichText);
    licenseLabel_->setOpenExternalLinks(true);
    licenseLabel_->setWordWrap(true);
    licenseLabel_->setText(tr(
        "<p style='font-size:9pt;'>Copyright © 2026 GitBolt. "
        "Licensed under the <a href='https://www.gnu.org/licenses/gpl-3.0.html'>"
        "GNU General Public License v3.0</a>. "
        "See the <a href='https://github.com/ThePieMonster/GitBolt/blob/main/LICENSE'>"
        "LICENSE</a> file for the full terms.</p>"
        "<p style='font-size:9pt;'>The name \"GitBolt\" and the GitBolt logo are "
        "trademarks, protected separately from the source code license. "
        "See the <a href='https://github.com/ThePieMonster/GitBolt/blob/main/TRADEMARK.md'>"
        "trademark policy</a> for details.</p>"));

    linksLabel_ = new QLabel(this);
    linksLabel_->setTextFormat(Qt::RichText);
    linksLabel_->setOpenExternalLinks(true);
    linksLabel_->setText(tr(
        "<p style='font-size:9pt;'>"
        "<a href='https://www.gitbolt.com'>Website</a>"
        " &nbsp;·&nbsp; "
        "<a href='https://github.com/ThePieMonster/GitBolt'>Project repository</a>"
        " &nbsp;·&nbsp; "
        "<a href='https://github.com/ThePieMonster/GitBolt/issues'>Report a bug</a>"
        " &nbsp;·&nbsp; "
        "<a href='https://github.com/ThePieMonster/GitBolt/blob/main/CONTRIBUTING.md'>Contribute</a>"
        "</p>"));

    // ----- Assemble the layout -------------------------------------------
    auto* textColumn = new QVBoxLayout;
    textColumn->setSpacing(8);
    textColumn->addWidget(titleLabel_);
    textColumn->addWidget(taglineLabel_);
    textColumn->addWidget(separator);
    textColumn->addWidget(buildInfoLabel_);
    textColumn->addSpacing(4);
    textColumn->addWidget(licenseLabel_);
    textColumn->addWidget(linksLabel_);
    textColumn->addStretch();

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(16);
    topRow->addWidget(iconLabel_, 0, Qt::AlignTop);
    topRow->addLayout(textColumn, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);

    // Root layout owns the dialog's outer padding. Previously this
    // was (0,0,0,0) with the top row handling its own margins — but
    // that left the QDialogButtonBox flush against the right and
    // bottom edges of the window, so the Close button crowded the
    // corner. Moving the padding here gives every child (top row
    // AND button box) uniform breathing room from the window frame.
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(20, 20, 20, 16);
    rootLayout->setSpacing(12);
    rootLayout->addLayout(topRow);
    rootLayout->addWidget(buttons);

    // At least 520, and wider when the text column needs it (a long
    // OS name in the build info, the links row in a wide font at 96
    // DPI): an explicit minimum overrides the layout's own, so a
    // fixed 520 let dragging the dialog narrower cut lines short.
    setMinimumWidth(qMax(520, minimumSizeHint().width()));
}

} // namespace gitbolt::dialogs
