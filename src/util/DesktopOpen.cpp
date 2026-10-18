#include "util/DesktopOpen.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace gitbolt::util {

namespace {

// Extensions, lower case and without the dot, of the files the
// platform's opener runs, or follows to what they point to, instead
// of showing them. git checks a file out at mode 644 unless it was
// committed executable, so these are told by their name alone.
QStringList runnableSuffixes()
{
#if defined(Q_OS_MACOS)
    return {
        // Terminal runs a shell script (.command, .tool) and the
        // command a session file (.term) or a settings file (.terminal)
        // holds.
        QStringLiteral("command"), QStringLiteral("tool"), QStringLiteral("term"),
        QStringLiteral("terminal"),
        // JavaLauncher, every Mac's default app for them, runs them
        // with whatever Java is installed.
        QStringLiteral("jar"), QStringLiteral("class"), QStringLiteral("jnlp"),
        // Location files: Finder, Safari, Mail, Screen Sharing and the
        // like open what they point to, a file:// URL included.
        QStringLiteral("fileloc"), QStringLiteral("inetloc"), QStringLiteral("webloc"),
        QStringLiteral("url"), QStringLiteral("afploc"), QStringLiteral("ftploc"),
        QStringLiteral("smbloc"), QStringLiteral("vncloc"), QStringLiteral("mailloc"),
        QStringLiteral("newsloc"), QStringLiteral("telnetloc")};
#elif defined(Q_OS_WIN)
    // PATHEXT is what Windows runs by name alone, and Windows' own list
    // runs when opened whatever PATHEXT says, so both. The others aren't
    // in it, but their default verb runs them all the same (a .cpl is a
    // DLL control.exe loads, a .chm runs its script, a .application
    // installs and starts an app), or opens what they point to, a share
    // elsewhere included; a file git checked out has no Mark of the
    // Web, so Windows asks nothing.
    const QString pathExt = qEnvironmentVariable("PATHEXT")
                            + QStringLiteral(";.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;"
                                             ".WSH;.MSC");
    QStringList suffixes;
    for (const QString& entry : pathExt.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        QString suffix = entry.trimmed().toLower();
        if (suffix.startsWith(QLatin1Char('.')))
            suffix.remove(0, 1);
        if (!suffix.isEmpty())
            suffixes << suffix;
    }
    suffixes << QStringLiteral("lnk") << QStringLiteral("url") << QStringLiteral("scr")
             << QStringLiteral("pif") << QStringLiteral("msi") << QStringLiteral("msp")
             << QStringLiteral("js") << QStringLiteral("jse") << QStringLiteral("vbs")
             << QStringLiteral("vbe") << QStringLiteral("wsf") << QStringLiteral("wsh")
             << QStringLiteral("hta") << QStringLiteral("cpl") << QStringLiteral("chm")
             << QStringLiteral("jar") << QStringLiteral("reg") << QStringLiteral("appref-ms")
             << QStringLiteral("application") << QStringLiteral("settingcontent-ms")
             << QStringLiteral("library-ms") << QStringLiteral("search-ms")
             << QStringLiteral("scf") << QStringLiteral("theme") << QStringLiteral("themepack")
             << QStringLiteral("diagcab");
    return suffixes;
#else
    // A launcher, a jar (java -jar), and the Windows programs and
    // shortcuts Wine runs where it is installed: its desktop file
    // takes them, executable bit or not.
    return {QStringLiteral("desktop"), QStringLiteral("jar"), QStringLiteral("exe"),
            QStringLiteral("com"), QStringLiteral("msi"), QStringLiteral("bat"),
            QStringLiteral("cmd"), QStringLiteral("lnk"), QStringLiteral("url")};
#endif
}

} // namespace

bool mayRunWhenOpened(const QFileInfo& file)
{
    // A folder's executable bit only lets it be searched.
    if (file.isDir())
        return file.isBundle();
    if (file.isExecutable())
        return true;
    return runnableSuffixes().contains(file.suffix().toLower());
}

QString folderToShow(const QString& path)
{
    QString at = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        // The desktop opens what a symbolic link leads to, so that is
        // what is looked at: a link to Tool.app is the bundle, and one
        // to a script is the script. isBundle() is only ever true on
        // macOS.
        const QString real = QFileInfo(at).canonicalFilePath();
        if (!real.isEmpty()) {
            const QFileInfo info(real);
            if (info.isDir() && !info.isBundle())
                return real;
            at = real;
        }
        const QString up = QFileInfo(at).absolutePath();
        if (up == at)
            return at;
        at = up;
    }
}

bool isWithinFolder(const QString& path, const QString& folder)
{
    const QString real = QFileInfo(path).canonicalFilePath();
    const QString root = QFileInfo(folder).canonicalFilePath();
    if (real.isEmpty() || root.isEmpty())
        return false;
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
    const QString rootSlash = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return real.compare(root, pathCase) == 0 || real.startsWith(rootSlash, pathCase);
}

} // namespace gitbolt::util
