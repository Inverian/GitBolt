#include "util/LockFiles.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLockFile>
#include <QString>

namespace gitbolt::util {

bool removeUnprovableStaleLock(QLockFile& lock)
{
    const auto settled = [&lock]() {
        const QDateTime written = QFileInfo(lock.fileName()).lastModified();
        return written.isValid()
               && qAbs(written.secsTo(QDateTime::currentDateTime())) >= 2;
    };
    if (!settled())
        return false;
    QLockFile removal(lock.fileName() + QStringLiteral(".rmlock"));
    // Re-checked under the removal lock: another process may have
    // just removed the file and re-created it as a live lock.
    return removal.tryLock(0) && settled() && lock.removeStaleLockFile();
}

} // namespace gitbolt::util
