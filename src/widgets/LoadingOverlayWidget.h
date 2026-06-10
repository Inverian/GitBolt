#pragma once

#include <QWidget>

class QTimer;

namespace gitbolt::widgets {

/// Translucent busy overlay with an animated spinner arc and a
/// message line, used while a long operation populates the view
/// underneath (e.g. opening a large repository).
///
/// Parent it to the widget it should cover; it tracks the parent's
/// size via an event filter and always fills the full rect. Being a
/// plain child widget on top, it also swallows mouse events so the
/// half-loaded view underneath isn't clickable while loading.
///
/// The animation timer only runs while the overlay is visible
/// (started in showEvent, stopped in hideEvent — same pattern as
/// BoltLogoWidget), so a hidden overlay costs nothing.
class LoadingOverlayWidget : public QWidget {
    Q_OBJECT

public:
    explicit LoadingOverlayWidget(QWidget* parent);

    /// Resize to the parent, raise above siblings, and show.
    void showOverlay(const QString& message);
    void hideOverlay();
    void setMessage(const QString& message);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QTimer* timer_ = nullptr;
    int angle_ = 0;        // current spinner rotation, degrees
    QString message_;
};

} // namespace gitbolt::widgets
