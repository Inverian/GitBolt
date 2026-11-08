#include "update/Version.h"

namespace gitbolt::update {

std::optional<Version> Version::parse(QStringView text) {
    if (text.startsWith(u'v'))
        text = text.mid(1);
    int parts[3] = {0, 0, 0};
    int part = 0;
    qsizetype digits = 0;
    for (qsizetype i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == u'.') {
            if (digits == 0)
                return std::nullopt;
            if (i < text.size() && ++part > 2)
                return std::nullopt;
            digits = 0;
            continue;
        }
        const QChar c = text[i];
        if (c < u'0' || c > u'9')
            return std::nullopt;
        if (digits == 1 && parts[part] == 0)
            return std::nullopt;   // "01"
        if (++digits > 6)
            return std::nullopt;
        parts[part] = parts[part] * 10 + (c.unicode() - u'0');
    }
    if (part != 2)
        return std::nullopt;
    return Version{parts[0], parts[1], parts[2]};
}

QString Version::toString() const {
    return QStringLiteral("%1.%2.%3").arg(majorPart).arg(minorPart).arg(patchPart);
}

} // namespace gitbolt::update
