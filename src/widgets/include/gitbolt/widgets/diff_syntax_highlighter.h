#pragma once
#include <QSyntaxHighlighter>

namespace gitbolt::widgets {

class DiffSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit DiffSyntaxHighlighter(QTextDocument* parent = nullptr);
protected:
    void highlightBlock(const QString& text) override;

};

} // namespace gitbolt::widgets
