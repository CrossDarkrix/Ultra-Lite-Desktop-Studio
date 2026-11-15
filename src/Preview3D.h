#pragma once
#include <QJsonObject>
#include <QMap>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QTimer>
#include <QVector3D>
#include <QWidget>
#include <functional>

class WidgetWindow;
class EffectsWidget;

// 3D preview (v0.11.0, part 1) — a port of the core of Python's
// litedesktopstudio/preview3d.py. The desktop is a fixed 720x420 plane
// projected with a yaw/pitch camera using Python's exact math; widgets
// are drawn as textured quads from live grabs, and the effects
// overlay's Moon / Water Surface / Ice markers can be dragged on the
// plane. Changes apply live (this build's editor convention), unlike
// Python's pending + apply model. Part 2 candidates: widget resize /
// rotate handles, puddle & cloud markers, per-type data overlays,
// the preset picker dialog.
class Preview3DCanvas : public QWidget {
    Q_OBJECT
public:
    explicit Preview3DCanvas(const QList<WidgetWindow *> *widgets, QWidget *parent = nullptr);

    void setSnapEnabled(bool on) { snap_ = on; }
    // "Desktop Lock" （画面固定）, Python's model (v0.12.0): ON = the
    // camera is fixed and widgets/markers can be edited; OFF = orbit
    // and zoom are free but widget editing is disabled (a notice
    // explains this when a widget is pressed).
    void setDesktopLocked(bool on) { locked_ = on; update(); }
    bool desktopLocked() const { return locked_; }
    void resetView();
    void refreshPixmaps();

signals:
    void requestSave();
    void requestAddWidget(const QString &type);
    void requestRemoveWidget(WidgetWindow *w);

protected:
    void paintEvent(QPaintEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    struct Proj { QPointF p; double scale; double z2; };
    Proj project(double x, double y, double z) const;
    static QPointF unitToPlane(double u, double v) {
        return QPointF((u - 0.5) * 720.0, (v - 0.5) * 420.0);
    }
    // Newton inverse of project() for points on the y=14 plane,
    // ported from Python's _screen_to_unit_on_desktop.
    QPointF screenToUnit(const QPointF &pos, double startU, double startV) const;
    QRect desktopRect() const;
    EffectsWidget *effectsWidget() const;
    // Renders a widget offscreen at its real size (QPainter over
    // paintEvent). QWidget::grab() returns empty pixmaps for some
    // top-level windows on Windows (bgAlpha-driven per-pixel alpha),
    // which is why whole widgets went missing from the preview.
    QPixmap renderWidget(WidgetWindow *w) const;
    std::function<void()> burst_; // installed by Preview3DWindow (screenshot burst)
    QPolygonF widgetQuad(const QRect &geom, double yLift) const;
    QPolygonF bandQuad(double v0, double v1, double y) const;
    double bandDepth(double v0, double v1) const;
    void drawLine3D(QPainter &p, const QVector3D &a, const QVector3D &b, QColor color, double width) const;
    void drawLabel(QPainter &p, const QPointF &at, const QString &text, bool selected) const;
    QString widgetLabel(WidgetWindow *w) const;
    WidgetWindow *hitWidget(const QPointF &pos) const; // markers excluded; fx included

    const QList<WidgetWindow *> *widgets_;
    QMap<WidgetWindow *, QPixmap> pixmaps_;
    QJsonObject fxObj_; // cached effects_json of the first overlay
    QTimer refreshTimer_;

    double yaw_ = -28.0;
    double pitch_ = 58.0;
    double zoom_ = 1.0;
    bool snap_ = false;
    bool locked_ = false;
    bool dragging_ = false; // fast pixmap mode + burst repaint coalescing
    QTimer frameTimer_;

    enum class Drag { None, Orbit, Widget, WidgetResize, Moon, Water, WaterDepth, Ice };
    Drag drag_ = Drag::None;
    QPoint lastPos_;
    QPointF dragStartUnit_;
    WidgetWindow *dragWidget_ = nullptr;
    QPointF dragWidgetOffset_; // pointer unit minus widget top-left unit
    double dragStartValue_ = 0.0;
    WidgetWindow *selected_ = nullptr;
    Drag selectedMarker_ = Drag::None; // Moon / Water / Ice when a marker is selected
    QString notice_;                   // transient in-canvas notice (Python-style)
    qint64 noticeUntilMs_ = 0;
    void showNotice(const QString &text);
    QPointF resizeHandlePos(WidgetWindow *w) const;
};

class Preview3DWindow : public QWidget {
    Q_OBJECT
public:
    explicit Preview3DWindow(const QList<WidgetWindow *> *widgets, QWidget *parent = nullptr);

signals:
    void requestSave();
    void requestAddWidget(const QString &type);
    void requestRemoveWidget(WidgetWindow *w);
};
