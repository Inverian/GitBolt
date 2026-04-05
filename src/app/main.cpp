#include "ui/MainWindow.h"
#include <QApplication>
#include <QStyleHints>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("GitBolt");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("GitBolt");
    app.setOrganizationDomain("gitbolt.dev");

    // Respect system dark mode
    app.styleHints()->setColorScheme(Qt::ColorScheme::Unknown);

    gitbolt::ui::MainWindow window;
    window.show();

    return app.exec();
}
