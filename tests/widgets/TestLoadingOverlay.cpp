// Tests for LoadingOverlayWidget, the repository view's loading veil.

#include <QTest>
#include <QWidget>

#include "widgets/LoadingOverlayWidget.h"

using gitbolt::widgets::LoadingOverlayWidget;

class TestLoadingOverlay : public QObject {
    Q_OBJECT

private slots:
    // Made under a parent that was shown, then hidden: the constructor's
    // hide() sent a hide event before the spinner's timer existed.
    void madeUnderAHiddenParent() {
        QWidget parent;
        parent.show();
        QVERIFY(QTest::qWaitForWindowExposed(&parent));
        parent.hide();
        LoadingOverlayWidget overlay(&parent);
        QVERIFY(overlay.isHidden());
    }

    void madeUnderANativeParentNeverShown() {
        QWidget parent;
        parent.winId();
        LoadingOverlayWidget overlay(&parent);
        QVERIFY(overlay.isHidden());
    }

    void showAndHide() {
        QWidget parent;
        parent.resize(200, 100);
        parent.show();
        QVERIFY(QTest::qWaitForWindowExposed(&parent));
        LoadingOverlayWidget overlay(&parent);
        overlay.showOverlay(QStringLiteral("Loading…"));
        QVERIFY(overlay.isVisible());
        QCOMPARE(overlay.geometry(), parent.rect());
        overlay.hideOverlay();
        QVERIFY(overlay.isHidden());
    }
};

QTEST_MAIN(TestLoadingOverlay)
#include "TestLoadingOverlay.moc"
