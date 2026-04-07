#pragma once

#include <QDialog>

class QLabel;

namespace gitbolt::dialogs {

/// "About GitBolt" dialog — shown from Help → About. Displays the
/// application name, version, tagline, a detailed build-info block
/// (Qt version, libgit2 version and features, compiler, build date),
/// license notice, and clickable links to the project repo,
/// trademark policy, and license text.
///
/// The dialog takes no constructor arguments — everything it displays
/// is pulled from QApplication metadata, libgit2 runtime APIs, and
/// preprocessor macros baked in at compile time.
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);

private:
    void setupUi();

    QLabel* iconLabel_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* taglineLabel_ = nullptr;
    QLabel* buildInfoLabel_ = nullptr;
    QLabel* licenseLabel_ = nullptr;
    QLabel* linksLabel_ = nullptr;
};

} // namespace gitbolt::dialogs
