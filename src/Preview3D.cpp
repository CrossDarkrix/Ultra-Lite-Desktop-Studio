#include "Preview3D.h"
#include "EffectsOverlay.h"
#include "WidgetWindow.h"
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QSet>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

Preview3DCanvas::Preview3DCanvas(const QList<WidgetWindow *> *widgets, QWidget *parent)
    : QWidget(parent), widgets_(widgets) {
    // Dev hooks for offscreen angle sweeps (v0.11.2 diagnosis).
    if (const double v = qEnvironmentVariable("LDS_TEST_YAW").toDouble())
        yaw_ = v;
    if (const double v = qEnvironmentVariable("LDS_TEST_PITCH").toDouble())
        pitch_ = v;
    setMinimumSize(640, 420);
    setMouseTracking(false);
    frameTimer_.setInterval(16); // ~60fps repaint coalescing during drags
    frameTimer_.setSingleShot(true);
    connect(&frameTimer_, &QTimer::timeout, this, [this] {
        update();
        if (burst_) burst_();
    });
    refreshTimer_.setInterval(1000);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        if (dragging_) return; // never steal time from an active drag
        refreshPixmaps();
        update();
    });
}

void Preview3DCanvas::resetView() {
    yaw_ = -28.0;
    pitch_ = 58.0;
    zoom_ = 1.0;
    update();
}

void Preview3DCanvas::showEvent(QShowEvent *) {
    refreshPixmaps();
    refreshTimer_.start();
}

void Preview3DCanvas::hideEvent(QHideEvent *) { refreshTimer_.stop(); }

QRect Preview3DCanvas::desktopRect() const {
    QScreen *s = QGuiApplication::primaryScreen();
    return s ? s->geometry() : QRect(0, 0, 1920, 1080);
}

EffectsWidget *Preview3DCanvas::effectsWidget() const {
    if (!widgets_) return nullptr;
    for (WidgetWindow *w : *widgets_)
        if (w && w->type() == QLatin1String("effects_overlay"))
            return static_cast<EffectsWidget *>(w);
    return nullptr;
}

QPixmap Preview3DCanvas::renderWidget(WidgetWindow *w) const {
    // QWidget::render into our own pixmap drives the widget's
    // paintEvent directly; unlike grab() it does not depend on the
    // native window's backing store or layering, so translucent
    // widgets (bgAlpha < 255) come out with their real content.
    QPixmap pm(w->size());
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    w->render(&p, QPoint(), QRegion(), QWidget::RenderFlags());
    p.end();
    return pm;
}

void Preview3DCanvas::refreshPixmaps() {
    if (!widgets_) return;
    QSet<WidgetWindow *> alive;
    for (WidgetWindow *w : *widgets_) {
        if (!w) continue;
        alive.insert(w);
        QPixmap pm = renderWidget(w);
        if (pm.width() > 640)
            pm = pm.scaledToWidth(640, Qt::SmoothTransformation);
        pixmaps_[w] = pm;
    }
    for (auto it = pixmaps_.begin(); it != pixmaps_.end();) {
        if (!alive.contains(it.key())) it = pixmaps_.erase(it);
        else ++it;
    }
    if (EffectsWidget *fx = effectsWidget()) fxObj_ = fx->effectsJson();
    else fxObj_ = QJsonObject();
}

// Python _project_point, verbatim math.
Preview3DCanvas::Proj Preview3DCanvas::project(double x, double y, double z) const {
    const double yaw = qDegreesToRadians(yaw_);
    const double pitch = qDegreesToRadians(pitch_);
    const double cy = std::cos(yaw), sy = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double x1 = x * cy - z * sy;
    const double z1 = x * sy + z * cy;
    const double y1 = y * cp - z1 * sp;
    const double z2 = y * sp + z1 * cp;
    const double focal = 760.0 * zoom_;
    const double scale = focal / std::max(120.0, 900.0 + z2);
    return {QPointF(width() * 0.5 + x1 * scale, height() * 0.56 - y1 * scale), scale, z2};
}

// Python _screen_to_unit_on_desktop: Newton inverse on the y=14 plane.
QPointF Preview3DCanvas::screenToUnit(const QPointF &pos, double startU, double startV) const {
    double u = std::clamp(startU, 0.0, 1.0);
    double v = std::clamp(startV, 0.0, 1.0);
    const double eps = 0.002;
    for (int i = 0; i < 18; ++i) {
        const QPointF pl = unitToPlane(u, v);
        const QPointF p = project(pl.x(), 14.0, pl.y()).p;
        const double fx = p.x() - pos.x();
        const double fy = p.y() - pos.y();
        if (std::abs(fx) + std::abs(fy) < 0.35) break;
        const QPointF plu = unitToPlane(std::min(1.0, u + eps), v);
        const QPointF pu = project(plu.x(), 14.0, plu.y()).p;
        const QPointF plv = unitToPlane(u, std::min(1.0, v + eps));
        const QPointF pv = project(plv.x(), 14.0, plv.y()).p;
        const double j11 = (pu.x() - p.x()) / eps;
        const double j21 = (pu.y() - p.y()) / eps;
        const double j12 = (pv.x() - p.x()) / eps;
        const double j22 = (pv.y() - p.y()) / eps;
        const double det = j11 * j22 - j12 * j21;
        if (std::abs(det) < 1e-6) break;
        u = std::clamp(u + (-fx * j22 + j12 * fy) / det, 0.0, 1.0);
        v = std::clamp(v + (j21 * fx - j11 * fy) / det, 0.0, 1.0);
    }
    return QPointF(u, v);
}

QPolygonF Preview3DCanvas::widgetQuad(const QRect &geom, double yLift) const {
    const QRect desk = desktopRect();
    const double u0 = (geom.left() - desk.left()) / double(desk.width());
    const double v0 = (geom.top() - desk.top()) / double(desk.height());
    const double u1 = (geom.right() - desk.left()) / double(desk.width());
    const double v1 = (geom.bottom() - desk.top()) / double(desk.height());
    QPolygonF quad;
    const QPointF corners[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    for (const QPointF &c : corners) {
        const QPointF pl = unitToPlane(c.x(), c.y());
        quad << project(pl.x(), yLift, pl.y()).p;
    }
    return quad;
}

QPolygonF Preview3DCanvas::bandQuad(double v0, double v1, double y) const {
    QPolygonF quad;
    const QPointF corners[4] = {{0.0, v0}, {1.0, v0}, {1.0, v1}, {0.0, v1}};
    for (const QPointF &c : corners) {
        const QPointF pl = unitToPlane(c.x(), c.y());
        quad << project(pl.x(), y, pl.y()).p;
    }
    return quad;
}

double Preview3DCanvas::bandDepth(double v0, double v1) const {
    const QPointF pl = unitToPlane(0.5, (v0 + v1) * 0.5);
    return project(pl.x(), 2.0, pl.y()).z2;
}

void Preview3DCanvas::drawLine3D(QPainter &p, const QVector3D &a, const QVector3D &b,
                                 QColor color, double width) const {
    const Proj p1 = project(a.x(), a.y(), a.z());
    const Proj p2 = project(b.x(), b.y(), b.z());
    const double f = std::clamp((p1.scale + p2.scale) * 0.75, 0.35, 1.0);
    color.setAlpha(std::clamp(int(color.alpha() * f), 30, 220));
    p.setPen(QPen(color, std::max(1.0, width)));
    p.drawLine(p1.p, p2.p);
}

void Preview3DCanvas::showNotice(const QString &text) {
    notice_ = text;
    noticeUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 2600; // Python: 2600ms
    update();
    QTimer::singleShot(2700, this, [this] { update(); });
}

QPointF Preview3DCanvas::resizeHandlePos(WidgetWindow *w) const {
    return widgetQuad(w->geometry(), 8.0)[2]; // bottom-right corner
}

QString Preview3DCanvas::widgetLabel(WidgetWindow *w) const {
    const QString t = w->config().title;
    return t.isEmpty() ? w->type() : t;
}

void Preview3DCanvas::drawLabel(QPainter &p, const QPointF &at, const QString &text, bool selected) const {
    QFont f = p.font();
    f.setPointSize(9);
    p.setFont(f);
    const QFontMetrics fm(f);
    const int w = fm.horizontalAdvance(text) + 14;
    const QRectF chip(at.x(), at.y(), w, 20);
    p.setPen(QPen(selected ? QColor(140, 210, 255) : QColor(255, 255, 255, 70), 1));
    p.setBrush(QColor(8, 14, 26, 185));
    p.drawRoundedRect(chip, 5, 5);
    p.setPen(QColor(235, 244, 255));
    p.drawText(chip, Qt::AlignCenter, text);
}

void Preview3DCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    // While dragging, fast (nearest) pixmap transforms keep the camera
    // fluid; the smooth transform returns for the resting frame.
    p.setRenderHint(QPainter::SmoothPixmapTransform, !dragging_);
    const QRectF rect(0, 0, width(), height());

    // Background (Python's "blue" theme gradient).
    QLinearGradient bg(0, 0, 0, rect.height());
    bg.setColorAt(0.0, QColor(8, 13, 28));
    bg.setColorAt(0.45, QColor(18, 31, 58));
    bg.setColorAt(1.0, QColor(7, 10, 18));
    p.fillRect(rect, bg);

    // Desktop plane + grid (Python palette).
    const double halfW = 360.0, halfH = 210.0, step = 60.0;
    QPolygonF planePoly;
    const QPointF pc[4] = {project(-halfW, 0, -halfH).p, project(halfW, 0, -halfH).p,
                           project(halfW, 0, halfH).p, project(-halfW, 0, halfH).p};
    for (const QPointF &c : pc) planePoly << c;
    QPainterPath planePath;
    planePath.addPolygon(planePoly);
    planePath.closeSubpath();
    QLinearGradient planeGrad(pc[0], pc[2]);
    planeGrad.setColorAt(0.0, QColor(255, 255, 255, 34));
    planeGrad.setColorAt(1.0, QColor(120, 170, 255, 18));
    p.setPen(QPen(QColor(255, 246, 220, 155), 2.0));
    p.setBrush(planeGrad);
    p.drawPath(planePath);
    const QColor grid(142, 210, 255, 82), major(255, 210, 210, 120);
    for (double x = -halfW; x <= halfW + 0.1; x += step)
        drawLine3D(p, QVector3D(float(x), 0, float(-halfH)), QVector3D(float(x), 0, float(halfH)),
                   std::abs(x) < 0.1 ? major : grid, 1.0);
    for (double z = -halfH; z <= halfH + 0.1; z += step)
        drawLine3D(p, QVector3D(float(-halfW), 0, float(z)), QVector3D(float(halfW), 0, float(z)),
                   std::abs(z) < 0.1 ? major : grid, 1.0);

    if (!widgets_) return;
    EffectsWidget *fx = effectsWidget();

    // Water / Ice bands lie on the plane, under the widget quads.
    if (fx) {
        if (fxObj_.value(QStringLiteral("water_surface_enabled")).toBool(false)) {
            const double y = fxObj_.value(QStringLiteral("water_surface_y")).toDouble(0.58);
            const double depth = fxObj_.value(QStringLiteral("water_surface_depth")).toDouble(0.42);
            const QPolygonF band = bandQuad(y, std::min(1.0, y + depth), 2.0);
            QPainterPath bp;
            bp.addPolygon(band);
            bp.closeSubpath();
            QColor wc(fxObj_.value(QStringLiteral("water_surface_color")).toString(QStringLiteral("#4FC3FF")));
            wc.setAlpha(selectedMarker_ == Drag::Water || selectedMarker_ == Drag::WaterDepth ? 130 : 92);
            p.setPen(QPen(QColor(216, 250, 255, 160), 1.2));
            p.setBrush(wc);
            p.drawPath(bp);
            drawLabel(p, band[3] + QPointF(6, 4), QStringLiteral("Water Surface"),
                      selectedMarker_ == Drag::Water || selectedMarker_ == Drag::WaterDepth);
            // Depth handle: midpoint of the front (v1) edge.
            const QPointF handle = (band[2] + band[3]) * 0.5;
            p.setPen(QPen(QColor(255, 255, 255), 1.4));
            p.setBrush(QColor(79, 195, 255));
            p.drawEllipse(handle, 6, 6);
        }
        if (fxObj_.value(QStringLiteral("ice_enabled")).toBool(false)) {
            const double y = fxObj_.value(QStringLiteral("ice_y")).toDouble(0.58);
            const QPolygonF band = bandQuad(y, 1.0, 2.5);
            QPainterPath bp;
            bp.addPolygon(band);
            bp.closeSubpath();
            QColor ic(fxObj_.value(QStringLiteral("ice_color")).toString(QStringLiteral("#9BDDF2")));
            ic.setAlpha(selectedMarker_ == Drag::Ice ? 120 : 80);
            p.setPen(QPen(QColor(232, 251, 255, 150), 1.2));
            p.setBrush(ic);
            p.drawPath(bp);
            drawLabel(p, band[3] + QPointF(6, 4), QStringLiteral("Ice"), selectedMarker_ == Drag::Ice);
        }
    }

    // Widget quads, far to near. The effects overlay is the plane's
    // own texture and is ALWAYS drawn first: sorting it among the
    // widgets by center depth let it paint over (and hide) widgets on
    // the far side whenever the camera rotated (v0.11.2, the user's
    // reproduction). One helper draws one textured quad.
    struct Item { WidgetWindow *w; QPolygonF quad; double z2; };
    QList<Item> items;
    auto drawQuad = [this, &p](const QPolygonF &quad, const QPixmap &pm, bool sel) {
        QPainterPath path;
        path.addPolygon(quad);
        path.closeSubpath();
        bool textured = false;
        if (!pm.isNull()) {
            const QPolygonF src = {QPointF(0, 0), QPointF(pm.width(), 0),
                                   QPointF(pm.width(), pm.height()), QPointF(0, pm.height())};
            QTransform t;
            if (QTransform::quadToQuad(src, quad, t)) {
                // No clip: quadToQuad maps the pixmap rect exactly onto
                // the quad. (The old clip-then-setWorldTransform code
                // let the clip be reinterpreted under the new transform
                // on some platforms, flinging textures off-quad.)
                p.save();
                p.setTransform(t, true);
                p.drawPixmap(0, 0, pm);
                p.restore();
                textured = true;
            }
        }
        if (!textured) {
            p.setBrush(QColor(30, 42, 64, 170));
            p.drawPath(path);
        }
        p.setPen(QPen(sel ? QColor(140, 210, 255) : QColor(255, 255, 255, 80), sel ? 2.2 : 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    if (fx) {
        drawQuad(widgetQuad(fx->geometry(), 1.0), pixmaps_.value(fx),
                 selected_ == fx && selectedMarker_ == Drag::None);
    }
    for (WidgetWindow *w : *widgets_) {
        if (!w || w == fx) continue;
        const QRect g = w->geometry();
        const QPointF pl = unitToPlane(
            (g.center().x() - desktopRect().left()) / double(desktopRect().width()),
            (g.center().y() - desktopRect().top()) / double(desktopRect().height()));
        items.append({w, widgetQuad(g, 8.0), project(pl.x(), 8.0, pl.y()).z2});
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.z2 > b.z2; });
    for (const Item &it : items)
        drawQuad(it.quad, pixmaps_.value(it.w), selected_ == it.w && selectedMarker_ == Drag::None);
    // Labels on top of all quads (the overlay's included).
    if (fx)
        drawLabel(p, widgetQuad(fx->geometry(), 1.0)[3] + QPointF(4, 4), widgetLabel(fx),
                  selected_ == fx && selectedMarker_ == Drag::None);
    for (const Item &it : items) {
        const bool sel = selected_ == it.w && selectedMarker_ == Drag::None;
        drawLabel(p, it.quad[3] + QPointF(4, 4), widgetLabel(it.w), sel);
    }

    // Moon marker floats above the plane (y=14, like Python's markers).
    if (fx && fxObj_.value(QStringLiteral("moon_body_enabled")).toBool(false)) {
        const double u = fxObj_.value(QStringLiteral("moon_x")).toDouble(0.78);
        const double v = fxObj_.value(QStringLiteral("moon_y")).toDouble(0.18);
        const QPointF pl = unitToPlane(u, v);
        const Proj c = project(pl.x(), 14.0, pl.y());
        const double r = std::max(7.0, 16.0 * c.scale);
        QRadialGradient glow(c.p, r * 2.6);
        glow.setColorAt(0.0, QColor(207, 232, 255, 90));
        glow.setColorAt(1.0, QColor(207, 232, 255, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(c.p, r * 2.6, r * 2.6);
        p.setPen(QPen(selectedMarker_ == Drag::Moon ? QColor(140, 210, 255) : QColor(255, 255, 255, 160),
                      selectedMarker_ == Drag::Moon ? 2.2 : 1.2));
        p.setBrush(QColor(255, 243, 196));
        p.drawEllipse(c.p, r, r);
        drawLabel(p, c.p + QPointF(r + 6, -10), QStringLiteral("Moon"), selectedMarker_ == Drag::Moon);
    }

    // Resize handle on the selected widget (Desktop Lock ON only,
    // like Python: handles are edit-mode affordances).
    if (locked_ && selected_ && selected_ != fx && selectedMarker_ == Drag::None) {
        const QPointF h = resizeHandlePos(selected_);
        p.setPen(QPen(QColor(10, 16, 26), 1.2));
        p.setBrush(QColor(140, 210, 255));
        p.drawRect(QRectF(h.x() - 6, h.y() - 6, 12, 12));
    }

    // Transient notice (Python's in-canvas notice, 2.6s).
    if (!notice_.isEmpty() && QDateTime::currentMSecsSinceEpoch() < noticeUntilMs_) {
        QFont f = p.font();
        f.setPointSize(10);
        p.setFont(f);
        const QFontMetrics fm(f);
        const int tw = fm.horizontalAdvance(notice_) + 28;
        const QRectF box((width() - tw) / 2.0, 14, tw, 30);
        p.setPen(QPen(QColor(140, 210, 255, 160), 1));
        p.setBrush(QColor(8, 14, 26, 215));
        p.drawRoundedRect(box, 7, 7);
        p.setPen(QColor(240, 247, 255));
        p.drawText(box, Qt::AlignCenter, notice_);
    }
}

WidgetWindow *Preview3DCanvas::hitWidget(const QPointF &pos) const {
    if (!widgets_) return nullptr;
    struct Item { WidgetWindow *w; QPolygonF quad; double z2; };
    QList<Item> items;
    for (WidgetWindow *w : *widgets_) {
        if (!w || w->type() == QLatin1String("effects_overlay")) continue;
        const QRect g = w->geometry();
        const QPointF pl = unitToPlane(
            (g.center().x() - desktopRect().left()) / double(desktopRect().width()),
            (g.center().y() - desktopRect().top()) / double(desktopRect().height()));
        items.append({w, widgetQuad(g, 8.0), project(pl.x(), 8.0, pl.y()).z2});
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.z2 < b.z2; });
    for (const Item &it : items)
        if (it.quad.containsPoint(pos, Qt::OddEvenFill)) return it.w;
    if (EffectsWidget *fx = effectsWidget())
        if (widgetQuad(fx->geometry(), 8.0).containsPoint(pos, Qt::OddEvenFill)) return fx;
    return nullptr;
}

void Preview3DCanvas::contextMenuEvent(QContextMenuEvent *e) {
    // Right-click in the preview (v0.13.0, the user's request): delete
    // and background transparency for the widget under the cursor,
    // plus save — mirroring the normal-display context menu. (The
    // transparency is background-only since v0.13.1.)
    WidgetWindow *hit = hitWidget(e->pos());
    if (hit) {
        selected_ = hit;
        selectedMarker_ = Drag::None;
        update();
    }
    QMenu menu(this);
    if (hit) {
        hit->addOpacityMenu(menu);
        QAction *del = menu.addAction(tr("このウィジェットを削除"));
        connect(del, &QAction::triggered, this, [this, hit] {
            if (selected_ == hit) selected_ = nullptr;
            emit requestRemoveWidget(hit);
        });
        menu.addSeparator();
    }
    QAction *save = menu.addAction(tr("設定を保存"));
    connect(save, &QAction::triggered, this, [this] { emit requestSave(); });
    menu.exec(e->globalPos());
}

void Preview3DCanvas::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const QPointF pos = e->position();
    lastPos_ = e->pos();
    drag_ = Drag::None;
    dragWidget_ = nullptr;
    EffectsWidget *fx = effectsWidget();

    if (locked_ && fx) {
        // Marker editing only under Desktop Lock (Python's model).
        // Moon first (it floats above everything).
        if (fxObj_.value(QStringLiteral("moon_body_enabled")).toBool(false)) {
            const double u = fxObj_.value(QStringLiteral("moon_x")).toDouble(0.78);
            const double v = fxObj_.value(QStringLiteral("moon_y")).toDouble(0.18);
            const QPointF pl = unitToPlane(u, v);
            const Proj c = project(pl.x(), 14.0, pl.y());
            const double r = std::max(7.0, 16.0 * c.scale);
            if (QLineF(pos, c.p).length() <= r + 9) {
                drag_ = Drag::Moon;
                selected_ = fx;
                selectedMarker_ = Drag::Moon;
                update();
                return;
            }
        }
        if (fxObj_.value(QStringLiteral("water_surface_enabled")).toBool(false)) {
            const double y = fxObj_.value(QStringLiteral("water_surface_y")).toDouble(0.58);
            const double depth = fxObj_.value(QStringLiteral("water_surface_depth")).toDouble(0.42);
            const QPolygonF band = bandQuad(y, std::min(1.0, y + depth), 2.0);
            const QPointF handle = (band[2] + band[3]) * 0.5;
            if (QLineF(pos, handle).length() <= 12) {
                drag_ = Drag::WaterDepth;
                dragStartUnit_ = screenToUnit(pos, 0.5, y + depth);
                selected_ = fx;
                selectedMarker_ = Drag::WaterDepth;
                update();
                return;
            }
            if (band.containsPoint(pos, Qt::OddEvenFill)) {
                drag_ = Drag::Water;
                dragStartUnit_ = screenToUnit(pos, 0.5, y);
                dragStartValue_ = y;
                selected_ = fx;
                selectedMarker_ = Drag::Water;
                update();
                return;
            }
        }
        if (fxObj_.value(QStringLiteral("ice_enabled")).toBool(false)) {
            const double y = fxObj_.value(QStringLiteral("ice_y")).toDouble(0.58);
            const QPolygonF band = bandQuad(y, 1.0, 2.5);
            if (band.containsPoint(pos, Qt::OddEvenFill)) {
                drag_ = Drag::Ice;
                dragStartUnit_ = screenToUnit(pos, 0.5, y);
                dragStartValue_ = y;
                selected_ = fx;
                selectedMarker_ = Drag::Ice;
                update();
                return;
            }
        }
    }

    // Widgets: non-overlay first (the overlay covers the whole plane),
    // front-most (smallest z2) first among themselves.
    if (widgets_) {
        struct Item { WidgetWindow *w; QPolygonF quad; double z2; };
        QList<Item> items;
        for (WidgetWindow *w : *widgets_) {
            if (!w || w->type() == QLatin1String("effects_overlay")) continue;
            const QRect g = w->geometry();
            const QPointF pl = unitToPlane(
                (g.center().x() - desktopRect().left()) / double(desktopRect().width()),
                (g.center().y() - desktopRect().top()) / double(desktopRect().height()));
            items.append({w, widgetQuad(g, 8.0), project(pl.x(), 8.0, pl.y()).z2});
        }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.z2 < b.z2; });
        for (const Item &it : items) {
            // Resize handle of the already-selected widget comes first.
            if (locked_ && selected_ == it.w && selectedMarker_ == Drag::None &&
                QLineF(pos, resizeHandlePos(it.w)).length() <= 13) {
                drag_ = Drag::WidgetResize;
                dragWidget_ = it.w;
                update();
                return;
            }
            if (it.quad.containsPoint(pos, Qt::OddEvenFill)) {
                selected_ = it.w;
                selectedMarker_ = Drag::None;
                if (locked_) {
                    drag_ = Drag::Widget;
                    dragWidget_ = it.w;
                    const QRect desk = desktopRect();
                    const QPointF unit = screenToUnit(
                        pos, (it.w->geometry().center().x() - desk.left()) / double(desk.width()),
                        (it.w->geometry().center().y() - desk.top()) / double(desk.height()));
                    dragStartUnit_ = unit;
                    dragWidgetOffset_ = QPointF(
                        unit.x() - (it.w->geometry().left() - desk.left()) / double(desk.width()),
                        unit.y() - (it.w->geometry().top() - desk.top()) / double(desk.height()));
                } else {
                    // Python's lock-required notice (Phase23D4A): shown
                    // for normal widgets only, and the press still
                    // falls through to orbiting afterwards.
                    showNotice(tr("ウィジェットを3Dプレビューで移動・サイズ変更するには、画面固定をONにしてください。"));
                    drag_ = Drag::Orbit;
                }
                update();
                return;
            }
        }
        // The overlay IS the canvas: pressing it selects it AND starts
        // an orbit drag. (Before v0.11.1 the press was swallowed here,
        // which left only the thin margins outside the plane usable
        // for orbiting — the user called the grab area "severe".)
        if (fx) {
            const QPolygonF quad = widgetQuad(fx->geometry(), 8.0);
            if (quad.containsPoint(pos, Qt::OddEvenFill)) {
                selected_ = fx;
                selectedMarker_ = Drag::None;
                drag_ = Drag::Orbit;
                update();
                return;
            }
        }
    }

    selected_ = nullptr;
    selectedMarker_ = Drag::None;
    drag_ = Drag::Orbit;
    update();
}

void Preview3DCanvas::mouseMoveEvent(QMouseEvent *e) {
    const QPointF pos = e->position();
    dragging_ = drag_ != Drag::None;
    EffectsWidget *fx = effectsWidget();
    switch (drag_) {
    case Drag::None:
        return;
    case Drag::Orbit: {
        const QPoint d = e->pos() - lastPos_;
        const Proj center = project(0, 0, 0);
        const double sign = pos.y() < center.p.y() ? 1.0 : -1.0; // Python Phase23D4B
        yaw_ += d.x() * 0.45 * sign;
        pitch_ = std::clamp(pitch_ - d.y() * 0.30, 18.0, 78.0);
        break;
    }
    case Drag::Widget: {
        if (!dragWidget_) return;
        const QRect desk = desktopRect();
        const QRect g = dragWidget_->geometry();
        const QPointF unit = screenToUnit(
            pos, (g.center().x() - desk.left()) / double(desk.width()),
            (g.center().y() - desk.top()) / double(desk.height()));
        double nx = desk.left() + (unit.x() - dragWidgetOffset_.x()) * desk.width();
        double ny = desk.top() + (unit.y() - dragWidgetOffset_.y()) * desk.height();
        if (snap_) {
            nx = std::round(nx / 20.0) * 20.0;
            ny = std::round(ny / 20.0) * 20.0;
        }
        const int ix = std::clamp(int(std::round(nx)), desk.left(), desk.right() - g.width() + 1);
        const int iy = std::clamp(int(std::round(ny)), desk.top(), desk.bottom() - g.height() + 1);
        dragWidget_->setGeometry(ix, iy, g.width(), g.height());
        break;
    }
    case Drag::WidgetResize: {
        if (!dragWidget_) return;
        const QRect desk = desktopRect();
        const QRect g = dragWidget_->geometry();
        const QPointF unit = screenToUnit(
            pos, (g.center().x() - desk.left()) / double(desk.width()),
            (g.center().y() - desk.top()) / double(desk.height()));
        double nw = (unit.x() * desk.width() + desk.left()) - g.left();
        double nh = (unit.y() * desk.height() + desk.top()) - g.top();
        if (snap_) {
            nw = std::round(nw / 20.0) * 20.0;
            nh = std::round(nh / 20.0) * 20.0;
        }
        const int iw = std::clamp(int(std::round(nw)), 60, desk.right() - g.left() + 1);
        const int ih = std::clamp(int(std::round(nh)), 40, desk.bottom() - g.top() + 1);
        dragWidget_->setGeometry(g.left(), g.top(), iw, ih);
        break;
    }
    case Drag::Moon: {
        if (!fx) return;
        double u = fxObj_.value(QStringLiteral("moon_x")).toDouble(0.78);
        double v = fxObj_.value(QStringLiteral("moon_y")).toDouble(0.18);
        const QPointF unit = screenToUnit(pos, u, v);
        u = unit.x();
        v = unit.y();
        if (snap_) {
            u = std::round(u / 0.01) * 0.01;
            v = std::round(v / 0.01) * 0.01;
        }
        fxObj_[QStringLiteral("moon_x")] = u;
        fxObj_[QStringLiteral("moon_y")] = v;
        fx->setEffectsValue(QStringLiteral("moon_x"), u, false);
        fx->setEffectsValue(QStringLiteral("moon_y"), v, false);
        break;
    }
    case Drag::Water: {
        if (!fx) return;
        const QPointF unit = screenToUnit(pos, 0.5, dragStartValue_);
        double y = std::clamp(dragStartValue_ + (unit.y() - dragStartUnit_.y()), 0.02, 0.98);
        if (snap_) y = std::round(y / 0.01) * 0.01;
        fxObj_[QStringLiteral("water_surface_y")] = y;
        fx->setEffectsValue(QStringLiteral("water_surface_y"), y, false);
        break;
    }
    case Drag::WaterDepth: {
        if (!fx) return;
        const double y = fxObj_.value(QStringLiteral("water_surface_y")).toDouble(0.58);
        const QPointF unit = screenToUnit(pos, 0.5, y);
        double depth = std::clamp(unit.y() - y, 0.05, 1.0 - y);
        if (snap_) depth = std::round(depth / 0.01) * 0.01;
        fxObj_[QStringLiteral("water_surface_depth")] = depth;
        fx->setEffectsValue(QStringLiteral("water_surface_depth"), depth, false);
        break;
    }
    case Drag::Ice: {
        if (!fx) return;
        const QPointF unit = screenToUnit(pos, 0.5, dragStartValue_);
        double y = std::clamp(dragStartValue_ + (unit.y() - dragStartUnit_.y()), 0.02, 0.98);
        if (snap_) y = std::round(y / 0.01) * 0.01;
        fxObj_[QStringLiteral("ice_y")] = y;
        fx->setEffectsValue(QStringLiteral("ice_y"), y, false);
        break;
    }
    }
    lastPos_ = e->pos();
    if (!frameTimer_.isActive()) frameTimer_.start(); // coalesced repaint
}

void Preview3DCanvas::mouseReleaseEvent(QMouseEvent *) {
    if (drag_ == Drag::Widget || drag_ == Drag::WidgetResize || drag_ == Drag::Moon ||
        drag_ == Drag::Water || drag_ == Drag::WaterDepth || drag_ == Drag::Ice)
        emit requestSave();
    drag_ = Drag::None;
    dragWidget_ = nullptr;
    dragging_ = false;
    update(); // one final smooth-transform frame
}

void Preview3DCanvas::wheelEvent(QWheelEvent *e) {
    if (locked_) { // camera fixed under Desktop Lock (Python)
        e->accept();
        return;
    }
    const int delta = e->angleDelta().y();
    if (delta > 0) zoom_ = std::min(1.80, zoom_ * 1.08);
    else if (delta < 0) zoom_ = std::max(0.55, zoom_ / 1.08);
    update();
}

Preview3DWindow::Preview3DWindow(const QList<WidgetWindow *> *widgets, QWidget *parent)
    : QWidget(parent) {
    setWindowTitle(tr("3Dプレビュー"));
    setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
    resize(1120, 760);
    auto *canvas = new Preview3DCanvas(widgets, this);
    connect(canvas, &Preview3DCanvas::requestSave, this, &Preview3DWindow::requestSave);
    connect(canvas, &Preview3DCanvas::requestAddWidget, this, &Preview3DWindow::requestAddWidget);
    connect(canvas, &Preview3DCanvas::requestRemoveWidget, this, &Preview3DWindow::requestRemoveWidget);
    auto *addBtn = new QPushButton(tr("＋ ウィジェットを追加"));
    auto *addMenu = new QMenu(addBtn);
    static const struct { const char *type; const char *label; } kinds[] = {
        {"clock", "時計"}, {"system", "システム"}, {"network", "通信"},
        {"calendar", "カレンダー"}, {"visualizer", "音楽Spectrum"},
        {"volume", "音量"}, {"media", "メディア"}, {"html", "テキスト"},
        {"weather", "天気"}, {"html_js", "JSHTML"},
    };
    for (const auto &k : kinds) {
        QAction *a = addMenu->addAction(QString::fromUtf8(k.label));
        const QString type = QString::fromLatin1(k.type);
        connect(a, &QAction::triggered, this, [this, type] { emit requestAddWidget(type); });
    }
    addBtn->setMenu(addMenu);
    auto *snap = new QCheckBox(tr("スナップ"));
    connect(snap, &QCheckBox::toggled, this, [canvas](bool on) { canvas->setSnapEnabled(on); });
    auto *lock = new QCheckBox(tr("画面固定"));
    lock->setToolTip(tr("ONの間は視点が固定され、ウィジェットとマーカーを編集できます"));
    connect(lock, &QCheckBox::toggled, this, [canvas](bool on) { canvas->setDesktopLocked(on); });
    auto *reset = new QPushButton(tr("視点をリセット"));
    connect(reset, &QPushButton::clicked, this, [canvas] { canvas->resetView(); });
    auto *closeBtn = new QPushButton(tr("閉じる"));
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    auto *bar = new QHBoxLayout;
    auto *hint = new QLabel(tr("画面固定ONで編集 / OFFで視点回転 (その場で反映)"));
    hint->setStyleSheet(QStringLiteral("color: gray;"));
    bar->addWidget(hint);
    bar->addStretch();
    bar->addWidget(addBtn);
    bar->addWidget(snap);
    bar->addWidget(lock);
    bar->addWidget(reset);
    bar->addWidget(closeBtn);
    auto *root = new QVBoxLayout(this);
    root->addLayout(bar);
    root->addWidget(canvas, 1);
}
