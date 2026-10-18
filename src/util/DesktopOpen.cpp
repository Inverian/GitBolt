#include "util/DesktopOpen.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace gitbolt::util {

namespace {

// Extensions, lower case and without the dot, of the files the
// platform's opener runs, or follows to what they point to, instead
// of showing them.
QStringList runnableSuffixes()
{
#if defined(Q_OS_MACOS)
    return {QStringLiteral("command"), QStringLiteral("tool"),
            QStringLiteral("terminal"), QStringLiteral("fileloc"),
            QStringLiteral("inetloc"), QStringLiteral("webloc")};
#elif defined(Q_OS_WIN)
    // PATHEXT is what Windows runs by name alone; the default list when
    // it isn't set. The others aren't in it but run, or open what they
    // point to, all the same.
    QString pathExt = qEnvironmentVariable("PATHEXT");
    if (pathExt.trimmed().isEmpty())
        pathExt = QStringLiteral(".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    QStringList suffixes;
    for (const QString& entry : pathExt.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        QString suffix = entry.trimmed().toLower();
        if (suffix.startsWith(QLatin1Char('.')))
            suffix.remove(0, 1);
        if (!suffix.isEmpty())
            suffixes << suffix;
    }
    suffixes << QStringLiteral("lnk") << QStringLiteral("url") << QStringLiteral("scr")
             << QStringLiteral("pif") << QStringLiteral("msi") << QStringLiteral("js")
             << QStringLiteral("jse") << QStringLiteral("vbs") << QStringLiteral("vbe")
             << QStringLiteral("wsf") << QStringLiteral("hta");
    return suffixes;
#else
    return {QStringLiteral("desktop")};
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

QString folderToShow(const QString& folder)
{
    QString at = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    // isBundle() is only ever true on macOS.
    while (QFileInfo(at).isBundle()) {
        const QString up = QFileInfo(at).absolutePath();
        if (up == at)
            break;
        at = up;
    }
    return at;
}

} // namespace gitbolt::util
