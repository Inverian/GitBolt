#include "util/ToolTip.h"

#include <QTextDocument>

namespace gitbolt::util {

QString plainToolTip(const QString& text)
{
    // The very test QToolTip makes: text it doesn't take for markup is
    // shown as plain text, as it is.
    if (!Qt::mightBeRichText(text))
        return text;
    return QStringLiteral("<p style='white-space:pre'>") + text.toHtmlEscaped()
           + QStringLiteral("</p>");
}

} // namespace gitbolt::util
