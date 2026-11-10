#pragma once

#include <QDialog>

class QLabel;

namespace gitbolt::dialogs {

/// "About GitBolt" dialog — shown from Help → About. Displays the
/// application name, version, tagline, a detailed build-info block
/// (Qt version, libgit2 version and features, compiler, build date, the
/// git GitBolt runs and what is known against it),
/// license notice, the notices of the software it ships that the LGPL
/// asks for (Qt's, LibXDiff's), with links to their licence texts and
/// to About Qt, and clickable links to the website, the releases, the
/// project repo, trademark policy, license text, and privacy policy
/// (what usage statistics collect).
///
/// The dialog takes no constructor arguments — everything it displays
/// is pulled from QApplication metadata, libgit2 runtime APIs, and
/// preprocessor macros baked in at compile time.
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);

    /// The licence texts of the software GitBolt ships (THIRD-PARTY-
    /// NOTICES.txt, cmake/Notices.cmake), where its package installs
    /// them relative to `appDir`, the folder of the running binary
    /// (QCoreApplication::applicationDirPath()): Contents/Resources/
    /// THIRD-PARTY-NOTICES.txt in the macOS bundle, THIRD-PARTY-
    /// NOTICES.txt above Windows's bin folder, and share/doc/gitbolt/
    /// copyright on Linux, from /usr/lib/gitbolt (the .deb). Empty when
    /// none is there, as in a build tree on Linux or Windows.
    static QString thirdPartyNoticesPath(const QString& appDir);

private:
    void setupUi();
    // The licence label's links: the notices file and About Qt here,
    // the web ones in the browser.
    void openLink(const QString& link);

    QString noticesPath_;  // thirdPartyNoticesPath(), or empty
    QLabel* iconLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* taglineLabel_ = nullptr;
    QLabel* buildInfoLabel_ = nullptr;
    QLabel* licenseLabel_ = nullptr;
    QLabel* linksLabel_ = nullptr;
};

} // namespace gitbolt::dialogs
