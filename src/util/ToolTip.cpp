#include "util/ToolTip.h"

namespace gitbolt::util {

QString plainToolTip(const QString& text)
{
    if (text.isEmpty())
        return {};
    return QStringLiteral("<p style='white-space:pre-wrap'>") + text.toHtmlEscaped()
           + QStringLiteral("</p>");
}

} // namespace gitbolt::util
