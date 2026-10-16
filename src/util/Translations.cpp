#include "util/Translations.h"

#include <QCoreApplication>
#include <QPointer>
#include <QString>
#include <QTranslator>

namespace gitbolt::util {

bool installEnglishPlurals()
{
    auto* app = QCoreApplication::instance();
    if (!app)
        return false;
    // The application owns it and deletes it with itself (a translator
    // uninstalls itself then); the guard goes null with it.
    static QPointer<QTranslator> installed;
    if (installed)
        return true;
    // The path, not QLocale: an English UI needs English plurals in a
    // German locale too. Built by qt_add_translations() into
    // gitbolt_util's resources (root CMakeLists.txt).
    auto* translator = new QTranslator(app);
    if (!translator->load(QStringLiteral(":/i18n/gitbolt_en.qm"))) {
        delete translator;
        return false;
    }
    if (!QCoreApplication::installTranslator(translator)) {
        delete translator;
        return false;
    }
    installed = translator;
    return true;
}

} // namespace gitbolt::util
