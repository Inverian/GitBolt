#include "widgets/UpdateNoticeBar.h"
#include "widgets/NoteBarColors.h"

#include <QApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QToolButton>

namespace gitbolt::widgets {

UpdateNoticeBar::UpdateNoticeBar(QWidget* parent)
    : QFrame(parent)
    , message_(new QLabel(this))
    , viewButton_(new QPushButton(tr("View"), this))
    , dismissButton_(new QToolButton(this))
{
    setObjectName(QStringLiteral("updateNotice"));
    message_->setObjectName(QStringLiteral("updateNotice.message"));
    viewButton_->setObjectName(QStringLiteral("updateNotice.view"));
    dismissButton_->setObjectName(QStringLiteral("updateNotice.dismiss"));
    setAutoFillBackground(true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 7);   // + the bottom line
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);
    row->addWidget(message_, 1);
    viewButton_->setAutoDefault(false);
    row->addWidget(viewButton_);
    // A flat ×, as notification bars have it: Dismiss in words is
    // GitNoticeBar's, whose note may matter more.
    dismissButton_->setText(QStringLiteral("×"));
    dismissButton_->setAutoRaise(true);
    dismissButton_->setToolTip(tr("Dismiss"));
    dismissButton_->setAccessibleName(tr("Dismiss"));
    row->addWidget(dismissButton_);

    connect(viewButton_, &QPushButton::clicked, this, [this] {
        hide();
        emit viewRequested();
    });
    connect(dismissButton_, &QToolButton::clicked, this, [this] {
        hide();
        emit dismissed();
    });

    applyColors();
    hide();
}

void UpdateNoticeBar::showVersion(const QString& version)
{
    version_ = version;
    message_->setText(tr("GitBolt %1 is available.").arg(version));
    show();
}

QString UpdateNoticeBar::text() const
{
    return message_->text();
}

void UpdateNoticeBar::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    // As GitNoticeBar: a theme switch arrives as the PaletteChange the
    // window passes down (applyColors() sends one too, let go).
    if (event->type() == QEvent::PaletteChange && !applyingColors_)
        applyColors();
}

void UpdateNoticeBar::applyColors()
{
    const QScopedValueRollback<bool> applying(applyingColors_, true);
    const NoteBarColors colors = noteBarColors(QApplication::palette());
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, colors.background);
    palette.setColor(QPalette::WindowText, colors.text);
    setPalette(palette);
    // The × is a flat tool button, drawn on the bar's amber: its text
    // in the bar's text colour, not the theme's ButtonText (light grey
    // on the dark theme). View keeps its button look, as GitNoticeBar's
    // buttons do.
    QPalette dismiss;
    dismiss.setColor(QPalette::ButtonText, colors.text);
    dismissButton_->setPalette(dismiss);
    border_ = colors.border;
    update();
}

void UpdateNoticeBar::paintEvent(QPaintEvent* event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(QRect(0, height() - 1, width(), 1), border_);
}

} // namespace gitbolt::widgets
