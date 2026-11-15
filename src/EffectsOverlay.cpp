// Effects overlay, v0.5 part 1 + v0.7.6 numeric audit fixes.
// Python reference: litedesktopstudio/effects.py (defaults) and
// LiteDesktopStudio.py _draw_rain / _update_rain / _new_raindrop /
// _draw_ripples / _new_extra_particle + _update_extra_effects (snow, cloud) /
// _draw_snow_dot / _draw_snow_crystal / _draw_moon_* / _draw_water_surface /
// _draw_ice_surface / _draw_ice_fog_fast.
//
// v0.7.6: the v0.5 port approximated several behaviours; this pass aligns
// the numbers with the Python originals:
//  - rain: per-drop wind vx (-45..45 px/s), vy = 260 + rnd*340*speed,
//    per-drop alpha 0.25..0.80 (drawn alpha = 190*a), pen thickness =
//    round(drop size) — previously a fixed 1.2px pen IGNORED the size
//    settings — length via rain_drop_length_randomness, slant = vx*0.035.
//  - ripples: spawn when a drop CROSSES rain_ripple_surface_y (previously
//    only at the bottom edge, so they almost never fired), radius from the
//    drop's speed factor, colour from ripple_color, global 0.025s cooldown,
//    drawn as easeOutCubic circles, alpha 210*(1-t), pen 3*(1-t)+1.
//  - snow: Python's flakes fall SLOWLY (dots vy = (20..48)*snow_speed px/s,
//    i.e. ~4..9 px/s at the 0.18 default) and mostly sway (sin wobble
//    10 px/s); dot radius = size (not size/2); flakes retire at
//    snow_surface_y (0.86). Crystals are proper 6-arm flakes with
//    branchlets and their own colours, rotating at rotation_speed.
//  - moon: radius is raw px (no screen scaling); moonlight gradient has
//    Python's 3 stops and a breathing radius; the light beam, the soft
//    gradient shadow and the two-tone craters are drawn per frame.
//  - water: the surface is a BAND (y .. y+depth*height) filled with
//    Python's 0.28/0.76/0.44 alpha gradient, wave rows span the band with
//    growing amplitude, plus the bright surface edge line.
//  - ice fog: vertical gradient (0.18a -> a -> 0) over the top
//    iceFogHeight of the ICE rect (was a flat rect over 0.24*screen).
//  - cloud: vx = (16..52)*cloud_speed, y centred on the altitude line.
// Deliberate deviations (startup only): particles are seeded across the
// widget so the sky is populated immediately; Python spawns everything
// above the top edge and drifts in over minutes.
#include "EffectsOverlay.h"
#include "DesktopIcons.h"
#include "EffectsEditor.h"
#include <QAction>
#include <QMenu>
#ifdef Q_OS_WIN
#include <windows.h> // WM_LBUTTONDOWN / MK_LBUTTON for the icon proxy
#endif
#ifndef Q_OS_WIN
// The icon input proxy forwards Win32 message ids to DesktopIcons (a
// no-op off Windows); define the constants so the handlers compile.
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define MK_LBUTTON 0x0001
#endif
#include <QCursor>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QTime>
#include <QtMath>
#include <algorithm>

static bool bOr(const QJsonObject &o, const char *k, bool d) {
    const auto v = o.value(QLatin1String(k));
    return v.isBool() ? v.toBool() : d;
}
static int iOr(const QJsonObject &o, const char *k, int d) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toInt() : d;
}
static double dOr(const QJsonObject &o, const char *k, double d) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : d;
}
static QColor cOr(const QJsonObject &o, const char *k, const QColor &d) {
    const auto v = o.value(QLatin1String(k));
    if (!v.isString()) return d;
    const QColor c(v.toString());
    return c.isValid() ? c : d;
}
static double easeOutCubic(double t) { return 1.0 - std::pow(1.0 - t, 3.0); }

// ---- scenic engine helpers (v0.8.0) ----
static double uni(QRandomGenerator &rng, double a, double b) {
    return a + rng.generateDouble() * (b - a);
}
static int randInt(QRandomGenerator &rng, int a, int b) {
    return a + int(rng.generateDouble() * (b - a + 1));
}
static void drawSoftVignette(QPainter &q, int w, int h, QColor color) {
    QRadialGradient grad(QPointF(w * 0.5, h * 0.48), std::max(w, h) * 0.76);
    QColor c0 = color; c0.setAlpha(0);
    grad.setColorAt(0.0, c0);
    grad.setColorAt(1.0, color);
    q.setPen(Qt::NoPen);
    q.setBrush(grad);
    q.drawRect(QRectF(0, 0, w, h));
}
// Local hour for the realtime sky; LDS_SKY_HOUR=13.5 forces a phase for
// offscreen verification (dev hook, same spirit as LDS_FAKE_AUDIO).
static double skyHour() {
    const QByteArray forced = qgetenv("LDS_SKY_HOUR");
    if (!forced.isEmpty()) {
        bool ok = false;
        const double v = forced.toDouble(&ok);
        if (ok) return v;
    }
    const QTime now = QTime::currentTime();
    return now.hour() + now.minute() / 60.0 + now.second() / 3600.0;
}
// Python _realtime_sky_state: phase 0 dawn, 1 day, 2 dusk, 3 night.
struct SkyState { int phase; double t; };
static SkyState skyState() {
    const double hour = skyHour();
    auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
    if (hour >= 4.5 && hour < 7.5) return {0, clamp01((hour - 4.5) / 3.0)};
    if (hour >= 7.5 && hour < 16.5) return {1, clamp01((hour - 7.5) / 9.0)};
    if (hour >= 16.5 && hour < 19.5) return {2, clamp01((hour - 16.5) / 3.0)};
    if (hour >= 19.5) return {3, clamp01((hour - 19.5) / 9.0)};
    return {3, clamp01((hour + 4.5) / 9.0)};
}
// Radial glow rendered once into a pixmap with a Bayer dither applied.
// A plain QRadialGradient halo bands visibly on smooth dark skies: in the
// tail the composite colour changes slower than one 8-bit level per pixel,
// so the eye reads the flat quantization steps as concentric rings (the
// user's real-machine screenshot, 2026-11-15). Smoother stops alone only
// move the bands; ±2 ordered dithering breaks them into noise instead.
// The pixmap is cached by the caller (glow positions/radii are stable).
static QPixmap makeGlowPixmap(double radius, const QVector<QPair<double, QColor>> &stops) {
    const int side = int(std::ceil(radius * 2)) + 2;
    QImage img(side, side, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    {
        QPainter q(&img);
        q.setRenderHint(QPainter::Antialiasing, true);
        QRadialGradient g(QPointF(side / 2.0, side / 2.0), radius);
        for (const auto &st : stops) g.setColorAt(st.first, st.second);
        q.setPen(Qt::NoPen);
        q.setBrush(g);
        q.drawEllipse(QPointF(side / 2.0, side / 2.0), radius, radius);
    }
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < side; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < side; ++x) {
            const int a = qAlpha(line[x]);
            if (a == 0) continue;
            const int n = (bayer[y & 3][x & 3] - 8) / 3; // -2..+2
            if (n == 0) continue;
            line[x] = qRgba(qBound(0, qRed(line[x]) + n, 255),
                            qBound(0, qGreen(line[x]) + n, 255),
                            qBound(0, qBlue(line[x]) + n, 255), a);
        }
    }
    return QPixmap::fromImage(img);
}

// Python _sahara_dry_grass_sprite: radial strand ball, cached per weed.
static QPixmap makeWeedSprite(double size, quint32 seed) {
    const int side = int(size * 2.4 + 8);
    QPixmap pix(side, side);
    pix.fill(Qt::transparent);
    QPainter q(&pix);
    q.setRenderHint(QPainter::Antialiasing, true);
    q.translate(side * 0.5, side * 0.5);
    QRandomGenerator rng(seed);
    for (int ring = 0; ring < 3; ++ring) {
        const int alpha = 118 - ring * 24;
        QPen pen(QColor(138, 91, 38, alpha), std::max(0.8, size * (0.026 + ring * 0.006)));
        pen.setCapStyle(Qt::RoundCap);
        q.setPen(pen);
        const int strands = 18 - ring * 3;
        for (int i = 0; i < strands; ++i) {
            const double a = 6.2831853 * i / strands + uni(rng, -0.18, 0.18);
            const double r0 = size * uni(rng, 0.08, 0.23);
            const double r1 = size * uni(rng, 0.36, 0.72);
            const double a1 = a + uni(rng, -0.30, 0.30);
            q.drawLine(QPointF(std::cos(a) * r0, std::sin(a) * r0),
                       QPointF(std::cos(a1) * r1, std::sin(a1) * r1));
        }
    }
    q.setPen(Qt::NoPen);
    q.setBrush(QColor(185, 132, 65, 34));
    q.drawEllipse(QRectF(-size * 0.36, -size * 0.22, size * 0.72, size * 0.44));
    return pix;
}

EffectSettings EffectSettings::fromJson(const QJsonObject &o) {
    EffectSettings s;
    s.rainEnabled = bOr(o, "rain_enabled", false);
    s.rainCount = iOr(o, "rain_count", 90);
    s.rainSpeed = dOr(o, "rain_speed", 1.0);
    s.rainColor = cOr(o, "rain_color", s.rainColor);
    s.rainLength = dOr(o, "rain_length", 16.0);
    s.rainDropMinSize = dOr(o, "rain_drop_min_size", 1.0);
    s.rainDropMaxSize = dOr(o, "rain_drop_max_size", 2.4);
    s.rainRippleEnabled = bOr(o, "rain_ripple_enabled", false);
    s.rainRippleChance = dOr(o, "rain_ripple_chance", 0.55);
    s.rainRippleSurfaceY = dOr(o, "rain_ripple_surface_y", 0.82);
    s.rainRippleMinRadius = dOr(o, "rain_ripple_min_radius", 24.0);
    s.rainRippleMaxRadius = dOr(o, "rain_ripple_max_radius", 92.0);
    s.rainDropLengthRandomness = dOr(o, "rain_drop_length_randomness", 0.55);
    s.rippleEnabled = bOr(o, "ripple_enabled", false);
    s.rippleColor = cOr(o, "ripple_color", s.rippleColor);
    s.rippleSpeed = dOr(o, "ripple_speed", 1.0);
    s.snowEnabled = bOr(o, "snow_enabled", false);
    s.snowCount = iOr(o, "snow_count", 90);
    s.snowSpeed = dOr(o, "snow_speed", 0.18);
    s.snowSize = dOr(o, "snow_size", 4.5);
    s.snowAlpha = iOr(o, "snow_alpha", 210);
    s.snowColor = cOr(o, "snow_color", s.snowColor);
    s.snowEdgeColor = cOr(o, "snow_edge_color", s.snowEdgeColor);
    s.snowCrystalEnabled = bOr(o, "snow_crystal_enabled", false);
    s.snowCrystalCount = iOr(o, "snow_crystal_count", 22);
    s.snowCrystalSpeed = dOr(o, "snow_crystal_speed", 0.12);
    s.snowCrystalSize = dOr(o, "snow_crystal_size", 15.0);
    s.snowCrystalAlpha = iOr(o, "snow_crystal_alpha", 220);
    s.snowCrystalColor = cOr(o, "snow_crystal_color", s.snowCrystalColor);
    s.snowCrystalEdgeColor = cOr(o, "snow_crystal_edge_color", s.snowCrystalEdgeColor);
    s.snowSurfaceY = dOr(o, "snow_surface_y", 0.86);
    s.cloudEnabled = bOr(o, "cloud_enabled", false);
    s.cloudCount = iOr(o, "cloud_count", 13);
    s.cloudSpeed = dOr(o, "cloud_speed", 1.05);
    s.cloudSize = dOr(o, "cloud_size", 120.0);
    s.cloudAlpha = iOr(o, "cloud_alpha", 200);
    s.cloudColor = cOr(o, "cloud_color", s.cloudColor);
    s.cloudShadowColor = cOr(o, "cloud_shadow_color", s.cloudShadowColor);
    s.cloudHighlightColor = cOr(o, "cloud_highlight_color", s.cloudHighlightColor);
    s.cloudAltitude = dOr(o, "cloud_altitude", 0.22);
    s.cloudDepth = dOr(o, "cloud_depth", 0.42);
    s.moonBodyEnabled = bOr(o, "moon_body_enabled", false);
    s.moonlightEnabled = bOr(o, "moonlight_enabled", false);
    s.moonShadowEnabled = bOr(o, "moon_shadow_enabled", false);
    s.moonX = dOr(o, "moon_x", 0.78);
    s.moonY = dOr(o, "moon_y", 0.18);
    s.moonRadius = dOr(o, "moon_radius", 74.0);
    s.moonAlpha = iOr(o, "moon_alpha", 230);
    s.moonColor = cOr(o, "moon_color", s.moonColor);
    s.moonEdgeColor = cOr(o, "moon_edge_color", s.moonEdgeColor);
    s.moonCraterCount = iOr(o, "moon_crater_count", 9);
    s.moonCraterAlpha = iOr(o, "moon_crater_alpha", 54);
    s.moonlightRadius = dOr(o, "moonlight_radius", 260.0);
    s.moonlightAlpha = iOr(o, "moonlight_alpha", 82);
    s.moonlightColor = cOr(o, "moonlight_color", s.moonlightColor);
    s.moonlightBeamEnabled = bOr(o, "moonlight_beam_enabled", false);
    s.moonlightBeamAlpha = iOr(o, "moonlight_beam_alpha", 44);
    s.moonlightBeamWidth = dOr(o, "moonlight_beam_width", 0.34);
    s.moonShadowColor = cOr(o, "moon_shadow_color", s.moonShadowColor);
    s.moonShadowAlpha = iOr(o, "moon_shadow_alpha", 70);
    s.moonShadowOffsetX = dOr(o, "moon_shadow_offset_x", 28.0);
    s.moonShadowOffsetY = dOr(o, "moon_shadow_offset_y", 38.0);
    s.moonShadowBlurRadius = dOr(o, "moon_shadow_blur_radius", 150.0);
    s.waterSurfaceEnabled = bOr(o, "water_surface_enabled", false);
    s.waterSurfaceAlpha = iOr(o, "water_surface_alpha", 92);
    s.waterSurfaceColor = cOr(o, "water_surface_color", s.waterSurfaceColor);
    s.waterSurfaceHighlightColor = cOr(o, "water_surface_highlight_color", s.waterSurfaceHighlightColor);
    s.waterSurfaceFlowSpeed = dOr(o, "water_surface_flow_speed", 0.55);
    s.waterSurfaceWaveCount = iOr(o, "water_surface_wave_count", 14);
    s.waterSurfaceWaveHeight = dOr(o, "water_surface_wave_height", 12.0);
    s.waterSurfaceY = dOr(o, "water_surface_y", 0.58);
    s.waterSurfaceDepth = dOr(o, "water_surface_depth", 0.42);
    s.waterSurfaceFlowAngle = dOr(o, "water_surface_flow_angle", 0.0);
    s.iceEnabled = bOr(o, "ice_enabled", false);
    s.iceAlpha = iOr(o, "ice_alpha", 178);
    s.iceColor = cOr(o, "ice_color", s.iceColor);
    s.iceEdgeColor = cOr(o, "ice_edge_color", s.iceEdgeColor);
    s.iceHighlightColor = cOr(o, "ice_highlight_color", s.iceHighlightColor);
    s.iceY = dOr(o, "ice_y", 0.58);
    s.iceCrackIntensity = dOr(o, "ice_crack_intensity", 0.46);
    s.iceBubbleIntensity = dOr(o, "ice_bubble_intensity", 0.36);
    s.iceFogEnabled = bOr(o, "ice_fog_enabled", false);
    s.iceFogAlpha = iOr(o, "ice_fog_alpha", 72);
    s.iceFogHeight = dOr(o, "ice_fog_height", 0.24);
    s.iceFogColor = cOr(o, "ice_fog_color", s.iceFogColor);
    s.iconSceneEnabled = bOr(o, "icon_scene_enabled", true);
    s.frameRateEnabled = bOr(o, "effect_frame_rate_enabled", true);
    s.frameRate = iOr(o, "effect_frame_rate", 24);
    s.uyuniEnabled = bOr(o, "uyuni_salt_flat_engine_enabled", false);
    s.chichibugahamaEnabled = bOr(o, "chichibugahama_mirror_engine_enabled", false);
    s.pamukkaleEnabled = bOr(o, "pamukkale_terrace_lake_engine_enabled", false);
    s.saharaEnabled = bOr(o, "sahara_desert_engine_enabled", false);
    // Python _realtime_scenic_sky_enabled: explicit key wins, else auto-on
    // when sahara / uyuni / chichibugahama is enabled.
    if (o.contains(QLatin1String("realtime_scenic_sky_enabled")))
        s.scenicSkyEnabled = bOr(o, "realtime_scenic_sky_enabled", false);
    else
        s.scenicSkyEnabled = s.uyuniEnabled || s.chichibugahamaEnabled || s.saharaEnabled;
    s.sunRaysEnabled = bOr(o, "realtime_scenic_sun_rays_enabled", false);
    s.atmosphereEnabled = bOr(o, "scenic_atmosphere_enhancement_enabled", true);
    s.waterMirrorWave = dOr(o, "water_mirror_wave", 7.0);
    return s;
}

EffectsWidget::EffectsWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {
    // No panel / title: this is a transparent overlay, not a panel widget.
    loadSettings();
    reseed();
    applyFrameRate();
    connect(&timer_, &QTimer::timeout, this, [this] {
        const double dt = std::min(0.1, clock_.restart() / 1000.0);
        time_ += dt;
        step(dt);
        update();
    });
    clock_.start();
    timer_.start();
}

void EffectsWidget::loadSettings() {
    const QString raw = cfg_.extra.value(QStringLiteral("effects_json")).toString();
    if (raw.isEmpty()) { s_ = EffectSettings(); return; }
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject()) { s_ = EffectSettings(); return; }
    s_ = EffectSettings::fromJson(doc.object());
}

void EffectsWidget::applyFrameRate() {
    // effect_frame_rate (default 24, the rate the overlay always ran
    // at); Python's theme presets set 30/45/60. Clamp to [5, 60]: the
    // point of this app is being light, and scenes are mostly static.
    const int fps = s_.frameRateEnabled ? std::clamp(s_.frameRate, 5, 60) : 24;
    timer_.setInterval(std::max(1, 1000 / fps));
}

QJsonObject EffectsWidget::effectsJson() const {
    const QString raw = cfg_.extra.value(QStringLiteral("effects_json")).toString();
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject();
}

void EffectsWidget::setEffectsJson(const QJsonObject &obj) {
    cfg_.extra[QStringLiteral("effects_json")] =
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    reloadEffects();
    emit requestSave();
}

void EffectsWidget::reloadEffects() {
    loadSettings();
    applyFrameRate();
    sceneStaticCache_.clear();
    mistCache_.clear();
    nightHaloCache_ = QPixmap();
    moonGlowCache_ = QPixmap();
    reseed();
    update();
}

void EffectsWidget::setEffectsValue(const QString &key, const QJsonValue &value, bool commitSave) {
    QJsonObject obj = effectsJson();
    obj[key] = value;
    cfg_.extra[QStringLiteral("effects_json")] =
        QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    loadSettings();
    applyFrameRate();
    update();
    if (commitSave) emit requestSave();
}

void EffectsWidget::addContextActions(QMenu &menu) {
    QAction *edit = menu.addAction(tr("エフェクトを編集…"));
    connect(edit, &QAction::triggered, this, [this] { openEffectsEditor(); });
    QAction *pv = menu.addAction(tr("3Dプレビューを開く"));
    connect(pv, &QAction::triggered, this, [this] { emit requestOpenPreview3D(); });
}

void EffectsWidget::openEffectsEditor() {
    if (!editor_) {
        editor_ = new EffectsEditorDialog(this);
        editor_->setAttribute(Qt::WA_DeleteOnClose);
    }
    editor_->show();
    editor_->raise();
    editor_->activateWindow();
}

// Python _new_raindrop, shared by seeding and respawn.
EffectsWidget::Drop EffectsWidget::makeDrop(QRandomGenerator &rng) const {
    Drop d;
    const double minSize = std::max(0.2, s_.rainDropMinSize);
    const double maxSize = std::max(minSize, s_.rainDropMaxSize);
    d.vy = 260.0 + rng.generateDouble() * 340.0 * std::max(0.1, s_.rainSpeed);
    d.vx = -45.0 + rng.generateDouble() * 90.0;
    d.size = minSize + rng.generateDouble() * (maxSize - minSize);
    d.alpha = 0.25 + rng.generateDouble() * 0.55;
    d.seed = rng.generateDouble() * 10000.0;
    d.x = rng.bounded(std::max(1, width()));
    d.y = 0;
    return d;
}

void EffectsWidget::reseed() {
    QRandomGenerator rng(12345); // deterministic layout; same seed, same sky
    const int w = std::max(1, width()), h = std::max(1, height());
    rain_.clear();
    if (s_.rainEnabled) {
        for (int i = 0; i < std::clamp(s_.rainCount, 0, 400); ++i) {
            Drop d = makeDrop(rng);
            d.y = rng.bounded(h); // startup only: pre-populate the sky
            rain_.append(d);
        }
    }
    snow_.clear();
    auto seedFlake = [&](bool crystal) {
        Flake f;
        const double speed = std::max(0.01, crystal ? s_.snowCrystalSpeed : s_.snowSpeed);
        if (crystal) {
            f.size = std::max(3.0, s_.snowCrystalSize) * (0.75 + rng.generateDouble() * 0.55);
            f.vx = (-10 + rng.generateDouble() * 20) * speed;
            f.vy = (16 + rng.generateDouble() * 20) * speed;
            f.alphaF = 0.60 + rng.generateDouble() * 0.40;
            f.rotSpeed = (-0.9 + rng.generateDouble() * 1.8) * speed;
        } else {
            f.size = std::max(1.0, s_.snowSize) * (0.55 + rng.generateDouble() * 0.9);
            f.vx = (-12 + rng.generateDouble() * 24) * speed;
            f.vy = (20 + rng.generateDouble() * 28) * speed;
            f.alphaF = 0.55 + rng.generateDouble() * 0.45;
            f.rotSpeed = (-1 + rng.generateDouble() * 2) * speed;
        }
        f.x = rng.bounded(w);
        f.y = rng.bounded(h); // startup only
        f.seed = rng.generateDouble() * 10000.0;
        f.rotation = rng.generateDouble() * 6.283;
        f.crystal = crystal;
        return f;
    };
    if (s_.snowEnabled)
        for (int i = 0; i < std::clamp(s_.snowCount, 0, 400); ++i) snow_.append(seedFlake(false));
    if (s_.snowCrystalEnabled)
        for (int i = 0; i < std::clamp(s_.snowCrystalCount, 0, 120); ++i) snow_.append(seedFlake(true));
    clouds_.clear();
    if (s_.cloudEnabled) {
        const double randomness = 0.85; // cloud_shape_randomness default
        const double band = std::max(18.0, h * std::max(0.04, s_.cloudDepth));
        for (int i = 0; i < std::clamp(s_.cloudCount, 0, 40); ++i) {
            Cloud c;
            c.scale = 0.66 + rng.generateDouble() * (0.55 + randomness * 0.30);
            c.x = rng.bounded(w * 1.2);
            c.y = h * s_.cloudAltitude + (rng.generateDouble() - 0.5) * band;
            c.vx = (16 + rng.generateDouble() * 36) * s_.cloudSpeed;
            c.alphaF = 0.60 + rng.generateDouble() * 0.40;
            c.pix = makeCloudPixmap(c.scale);
            clouds_.append(c);
        }
    }
    // Ice cracks / bubbles: seeded once per resize, drawn as a static layer.
    iceCracks_.clear(); iceBubbles_.clear();
    if (s_.iceEnabled) {
        const int cracks = int(16 * s_.iceCrackIntensity) + 2;
        for (int i = 0; i < cracks; ++i) {
            double x = rng.bounded(w), y = h * s_.iceY + rng.bounded(int(h * 0.4));
            for (int k = 0; k < 4; ++k) {
                iceCracks_.append(QPointF(x, y));
                x += rng.bounded(90) - 20; y += rng.bounded(26) - 6;
            }
        }
        const int bubbles = int(34 * s_.iceBubbleIntensity);
        for (int i = 0; i < bubbles; ++i)
            iceBubbles_.append(QPointF(rng.bounded(w), h * s_.iceY + rng.bounded(int(h * 0.4))));
    }
    if (s_.moonBodyEnabled || s_.moonlightEnabled || s_.moonShadowEnabled) {
        moonCenter_ = QPointF(w * std::clamp(s_.moonX, 0.0, 1.0),
                              h * std::clamp(s_.moonY, 0.0, 1.0));
        moonRadius_ = std::max(2.0, s_.moonRadius); // Python: raw px
    }
    ripples_.clear();
    // ---- scenic engines: star field, tumbleweeds, static caches ----
    sceneStaticCache_.clear();
    mistCache_.clear();
    nightHaloCache_ = QPixmap();
    sceneCacheSize_ = QSize(w, h);
    stars_.clear();
    {
        QRandomGenerator srng(0x515459u);
        const int count = std::clamp(int(w * double(h) / 10500.0), 48, 180);
        for (int i = 0; i < count; ++i) {
            Star st;
            st.x = srng.generateDouble();
            st.y = srng.generateDouble();
            st.base = randInt(srng, 78, 190);
            st.twSpeed = 0.35 + srng.generateDouble() * 0.85;
            st.twPhase = srng.generateDouble() * 6.2832;
            st.cross = srng.generateDouble() < 0.028;
            stars_.append(st);
        }
    }
    weeds_.clear();
    if (s_.saharaEnabled) {
        QRandomGenerator wrng(0xD647u);
        const int count = std::clamp(int(w / 210.0), 4, 9);
        for (int i = 0; i < count; ++i) {
            Weed wd;
            wd.size = uni(wrng, 18.0, 42.0);
            wd.baseY = h * uni(wrng, 0.68, 0.93);
            wd.y = wd.baseY;
            wd.x = uni(wrng, -w * 0.25, w * 0.95);
            wd.speed = uni(wrng, 7.0, 18.0);
            wd.rot = uni(wrng, 0.0, 360.0);
            wd.spin = uni(wrng, 8.0, 22.0);
            wd.phase = uni(wrng, 0.0, 6.2832);
            wd.sprite = makeWeedSprite(wd.size, wrng.generate());
            weeds_.append(wd);
        }
    }
}

QPixmap EffectsWidget::makeCloudPixmap(double scale) const {
    // Soft cumulus: gradient lobes along a curved spine, cached per cloud
    // (Python's cloud_cache idea — the pixmap is built once, then blitted).
    // v0.8.0: lobes stay inside the canvas (the old version clipped puff
    // gradients at the pixmap edge, leaving square outlines on dark skies,
    // and its solid highlight/shadow ellipses read as flat discs).
    const int pw = int(s_.cloudSize * 2.6 * scale), ph = int(s_.cloudSize * 1.35 * scale);
    QPixmap pix(pw, ph);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRandomGenerator rng(quint32(pw * 31 + ph * 7 + int(scale * 100)));
    auto puff = [&](double cx, double cy, double rx, const QColor &base, int alpha) {
        QRadialGradient g(cx, cy, rx);
        QColor col = base; col.setAlpha(alpha);
        g.setColorAt(0.0, col);
        col.setAlpha(0);
        g.setColorAt(1.0, col);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QRectF(cx - rx, cy - rx * 0.58, rx * 2, rx * 1.16));
    };
    const int lobes = 11;
    for (int i = 0; i < lobes; ++i) {
        const double fx = i / double(lobes - 1);
        const double cx = pw * (0.30 + 0.40 * fx) + (rng.generateDouble() - 0.5) * pw * 0.06;
        const double cy = ph * (0.54 - 0.10 * std::sin(fx * 3.14159265)) + (rng.generateDouble() - 0.5) * ph * 0.10;
        const double rx = pw * (0.13 + 0.05 * std::sin(fx * 3.14159265)) * (0.85 + rng.generateDouble() * 0.3);
        puff(cx, cy, rx, s_.cloudColor, 120);
    }
    // Lit tops and shaded underside, as smaller gradient lobes.
    for (int i = 0; i < 4; ++i) {
        const double fx = 0.15 + 0.7 * i / 3.0;
        puff(pw * (0.30 + 0.40 * fx), ph * (0.40 - 0.08 * std::sin(fx * 3.14159265)),
             pw * 0.085, s_.cloudHighlightColor, 70);
        puff(pw * (0.30 + 0.40 * fx), ph * 0.66, pw * 0.10, s_.cloudShadowColor, 46);
    }
    return pix;
}

void EffectsWidget::step(double dt) {
    const int w = width(), h = height();
    auto *g = QRandomGenerator::global();
    // ---- rain (Python _update_rain) ----
    const double surfaceY = h * std::clamp(s_.rainRippleSurfaceY, 0.0, 1.0);
    for (Drop &d : rain_) {
        const double prevY = d.y;
        d.x += d.vx * dt;
        d.y += d.vy * dt;
        const bool hitSurface = prevY < surfaceY && surfaceY <= d.y;
        const bool out = d.y > h + 60 || d.x < -80 || d.x > w + 80;
        if (hitSurface) {
            if (s_.rainRippleEnabled && s_.rippleEnabled &&
                time_ - lastRippleAt_ >= 0.025 &&
                g->generateDouble() < std::clamp(s_.rainRippleChance, 0.0, 1.0)) {
                const double speedFactor = std::clamp(std::abs(d.vy) / 520.0, 0.2, 2.5);
                Ripple rp;
                rp.x = d.x; rp.y = surfaceY; rp.age = 0;
                rp.maxR = s_.rainRippleMinRadius +
                    (s_.rainRippleMaxRadius - s_.rainRippleMinRadius) *
                    std::min(1.0, speedFactor);
                rp.speed = std::max(0.05, s_.rippleSpeed) * (0.9 + speedFactor * 0.2);
                rp.color = s_.rippleColor;
                ripples_.append(rp);
                if (ripples_.size() > 60) ripples_.removeFirst();
                lastRippleAt_ = time_;
            }
            d = makeDrop(*g);
            d.y = -g->generateDouble() * 160.0;
            continue;
        }
        if (out) {
            d = makeDrop(*g);
            d.y = -g->generateDouble() * 160.0;
        }
    }
    for (int i = ripples_.size() - 1; i >= 0; --i) {
        ripples_[i].age += dt;
        if (ripples_[i].age > 1.1 / std::max(0.05, ripples_[i].speed)) ripples_.removeAt(i);
    }
    // ---- snow (Python _update_extra_effects; retires at snow_surface_y) ----
    const double snowY = h * std::clamp(s_.snowSurfaceY, 0.0, 1.0);
    for (Flake &f : snow_) {
        const double prevY = f.y;
        f.x += (f.vx + std::sin(time_ * 1.2 + f.seed) * 10.0) * dt;
        f.y += f.vy * dt;
        f.rotation += f.rotSpeed * dt;
        const bool hit = prevY < snowY && snowY <= f.y;
        const bool out = f.y > h + 120 || f.y < -220 || f.x < -220 || f.x > w + 220;
        if (hit || out) {
            const bool crystal = f.crystal;
            const double speed = std::max(0.01, crystal ? s_.snowCrystalSpeed : s_.snowSpeed);
            if (crystal) {
                f.size = std::max(3.0, s_.snowCrystalSize) * (0.75 + g->generateDouble() * 0.55);
                f.vx = (-10 + g->generateDouble() * 20) * speed;
                f.vy = (16 + g->generateDouble() * 20) * speed;
                f.alphaF = 0.60 + g->generateDouble() * 0.40;
                f.rotSpeed = (-0.9 + g->generateDouble() * 1.8) * speed;
            } else {
                f.size = std::max(1.0, s_.snowSize) * (0.55 + g->generateDouble() * 0.9);
                f.vx = (-12 + g->generateDouble() * 24) * speed;
                f.vy = (20 + g->generateDouble() * 28) * speed;
                f.alphaF = 0.55 + g->generateDouble() * 0.45;
                f.rotSpeed = (-1 + g->generateDouble() * 2) * speed;
            }
            f.x = g->bounded(w);
            f.y = -g->generateDouble() * std::max(80.0, h * (crystal ? 0.55 : 0.35));
            f.seed = g->generateDouble() * 10000.0;
        }
    }
    // ---- clouds ----
    for (Cloud &c : clouds_) {
        c.x += (c.vx + std::sin(time_ * 0.18 + c.alphaF * 997.0) * 1.8) * dt;
        if (c.x - c.pix.width() / 2 > w) c.x = -c.pix.width() / 2;
    }
    // ---- sahara tumbleweeds (Python _update_sahara_dry_grass) ----
    if (s_.saharaEnabled) {
        for (Weed &wd : weeds_) {
            wd.x += wd.speed * dt;
            wd.rot = std::fmod(wd.rot + wd.spin * dt, 360.0);
            wd.y = wd.baseY + std::sin(time_ * 0.55 + wd.phase) * wd.size * 0.12;
            if (wd.x > w + wd.size * 2) {
                wd.x = -wd.size * 2;
                wd.baseY = h * (0.68 + 0.25 * std::fmod(wd.phase * 1.37, 1.0));
            }
        }
    }
}

void EffectsWidget::drawMoon(QPainter &p, const QRect &r, double t) {
    const QPointF c = moonCenter_;
    const double rad = moonRadius_;
    // Shadow: soft radial-gradient blob (Python _draw_moon_shadow).
    if (s_.moonShadowEnabled) {
        const double blurR = std::max(rad, s_.moonShadowBlurRadius);
        const QPointF sc = c + QPointF(s_.moonShadowOffsetX, s_.moonShadowOffsetY);
        QRadialGradient g(sc, blurR);
        QColor col = s_.moonShadowColor; col.setAlpha(s_.moonShadowAlpha);
        g.setColorAt(0.0, col);
        col.setAlpha(int(s_.moonShadowAlpha * 0.28));
        g.setColorAt(0.48, col);
        col.setAlpha(0);
        g.setColorAt(1.0, col);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.save();
        p.translate(sc);
        p.scale(1.0, 0.86);
        p.translate(-sc);
        p.drawEllipse(sc, blurR, blurR);
        p.restore();
    }
    // Moonlight: breathing radial glow + optional beam (_draw_moonlight).
    // v0.8.1: the glow is a dithered cached pixmap (anti-banding), scaled
    // ±8% for the breathing instead of re-rendering the gradient.
    if (s_.moonlightEnabled) {
        const double baseR = std::max(rad * 1.2, s_.moonlightRadius);
        const double lightR = baseR * (0.92 + 0.08 * std::sin(t * 0.85));
        if (moonGlowCache_.isNull() || moonGlowRadius_ != int(baseR)) {
            QColor c0 = s_.moonlightColor; c0.setAlpha(s_.moonlightAlpha);
            QColor c1 = s_.moonlightColor; c1.setAlpha(int(s_.moonlightAlpha * 0.30));
            QColor c2 = s_.moonlightColor; c2.setAlpha(int(s_.moonlightAlpha * 0.10));
            QColor c3 = s_.moonlightColor; c3.setAlpha(0);
            moonGlowCache_ = makeGlowPixmap(baseR, {{0.0, c0}, {0.38, c1}, {0.65, c2}, {1.0, c3}});
            moonGlowRadius_ = int(baseR);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::NoBrush);
        p.drawPixmap(QRectF(c.x() - lightR, c.y() - lightR, lightR * 2, lightR * 2), moonGlowCache_,
                     QRectF(moonGlowCache_.rect()));
        if (s_.moonlightBeamEnabled) {
            const double bw = std::clamp(s_.moonlightBeamWidth, 0.05, 1.0);
            const double topW = rad * (0.35 + bw);
            const double bottomW = std::max(topW * 1.4, std::max(r.width(), r.height()) * bw);
            double reach = 0;
            for (const QPointF corner : {QPointF(r.topLeft()), QPointF(r.topRight()),
                                         QPointF(r.bottomLeft()), QPointF(r.bottomRight())})
                reach = std::max(reach, QLineF(c, corner).length());
            reach += rad * 0.8;
            const double startY = rad * 0.22, endY = reach;
            QPainterPath beam;
            beam.moveTo(-topW, startY);
            beam.cubicTo(-bottomW * 0.22, lightR * 0.34, -bottomW * 0.48, endY - rad * 0.4, -bottomW, endY);
            beam.lineTo(bottomW, endY);
            beam.cubicTo(bottomW * 0.48, endY - rad * 0.4, bottomW * 0.22, lightR * 0.34, topW, startY);
            beam.closeSubpath();
            QLinearGradient bg(0, 0, 0, endY);
            QColor bc = s_.moonlightColor; bc.setAlpha(s_.moonlightBeamAlpha);
            bg.setColorAt(0.0, bc);
            bc.setAlpha(int(s_.moonlightBeamAlpha * 0.20));
            bg.setColorAt(0.58, bc);
            bc.setAlpha(0);
            bg.setColorAt(1.0, bc);
            p.save();
            p.translate(c);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawPath(beam);
            p.restore();
        }
    }
    // Body: offset radial gradient + two-tone craters (_draw_moon_body).
    if (s_.moonBodyEnabled) {
        QRadialGradient g(c + QPointF(-rad * 0.26, -rad * 0.30), rad * 1.25);
        QColor c0(255, 255, 245); c0.setAlpha(s_.moonAlpha);
        QColor c1 = s_.moonColor; c1.setAlpha(s_.moonAlpha);
        QColor c2 = s_.moonEdgeColor; c2.setAlpha(int(s_.moonAlpha * 0.92));
        g.setColorAt(0.0, c0);
        g.setColorAt(0.52, c1);
        g.setColorAt(1.0, c2);
        QColor penCol = s_.moonEdgeColor; penCol.setAlpha(int(s_.moonAlpha * 0.72));
        p.setPen(QPen(penCol, std::max(1.0, rad * 0.025)));
        p.setBrush(g);
        p.drawEllipse(c, rad, rad);
        const int craterAlpha = int(s_.moonCraterAlpha * s_.moonAlpha / 255.0);
        QRandomGenerator rng(7331);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < s_.moonCraterCount; ++i) {
            const double ang = rng.generateDouble() * 6.2832;
            const double dist = rad * (0.12 + rng.generateDouble() * 0.62);
            const QPointF cc(c.x() + std::cos(ang) * dist * 0.88,
                             c.y() + std::sin(ang) * dist * 0.78);
            const double cr = rad * (0.035 + rng.generateDouble() * 0.075);
            p.setBrush(QColor(86, 92, 112, craterAlpha));
            p.drawEllipse(cc, cr * 1.08, cr * 0.82);
            p.setBrush(QColor(255, 255, 245, int(craterAlpha * 0.36)));
            p.drawEllipse(cc + QPointF(-cr * 0.25, -cr * 0.22),
                          std::max(1.0, cr * 0.42), std::max(1.0, cr * 0.28));
        }
    }
}

void EffectsWidget::drawWater(QPainter &p, const QRect &r, double t) {
    const double alpha = s_.waterSurfaceAlpha;
    const double y0 = r.top() + r.height() * std::clamp(s_.waterSurfaceY, 0.0, 1.0);
    const double depth = r.height() * std::clamp(s_.waterSurfaceDepth, 0.05, 1.0);
    const double y1 = std::min(double(r.bottom()), y0 + depth);
    if (y1 <= y0) return;
    const QRectF band(r.left(), y0, r.width(), y1 - y0);
    p.save();
    p.setClipRect(band);
    // Body: vertical gradient, Python's 0.28 / 0.76 / 0.44 alpha profile.
    QLinearGradient g(0, band.top(), 0, band.bottom());
    QColor c0 = s_.waterSurfaceColor; c0.setAlpha(int(alpha * 0.28));
    QColor c1 = s_.waterSurfaceColor; c1.setAlpha(int(alpha * 0.76));
    QColor c2 = s_.waterSurfaceColor; c2.setAlpha(int(alpha * 0.44));
    g.setColorAt(0.0, c0);
    g.setColorAt(0.45, c1);
    g.setColorAt(1.0, c2);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRect(band);
    // Wave rows across the band (Python's rotated span layout; angle 0 by
    // default, rotation applied around the band centre when configured).
    const int waves = std::clamp(s_.waterSurfaceWaveCount, 0, 120);
    const double phase = t * (0.65 + s_.waterSurfaceFlowSpeed * 1.75);
    const double span = std::hypot(double(r.width()), double(r.height())) * 1.35;
    const QPointF center = band.center();
    p.translate(center);
    p.rotate(s_.waterSurfaceFlowAngle);
    p.translate(-center);
    if (waves > 0) {
        const double stepY = std::max(4.0, band.height() / waves);
        QColor hi = s_.waterSurfaceHighlightColor; hi.setAlpha(int(alpha * 0.62));
        QColor baseLine = s_.waterSurfaceColor; baseLine.setAlpha(int(alpha * 0.36));
        for (int i = 0; i < waves; ++i) {
            const double yy0 = band.top() + (i + 0.5) * stepY;
            const double amp = s_.waterSurfaceWaveHeight * (0.22 + 0.78 * (i + 1) / double(waves));
            const double wobble = std::sin(phase * 0.73 + i * 1.71);
            QPainterPath path;
            for (int sm = 0; sm <= 32; ++sm) {
                const double tt = sm / 32.0;
                const double x = center.x() - span * 0.5 + span * tt;
                const double yy = yy0 + std::sin(tt * 6.2832 * (1.5 + (i % 4) * 0.35) + phase + i * 0.91) * amp * 0.35
                                  + wobble * amp * 0.22;
                if (sm == 0) path.moveTo(x, yy); else path.lineTo(x, yy);
            }
            QPen pen(i % 2 == 0 ? hi : baseLine, std::max(1.0, 0.8 + amp * 0.035));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }
    // Bright surface edge line.
    QColor edge = s_.waterSurfaceHighlightColor; edge.setAlpha(int(alpha * 0.78));
    QPen edgePen(edge, std::max(1.0, 1.3 + s_.waterSurfaceWaveHeight * 0.025));
    edgePen.setCapStyle(Qt::RoundCap);
    p.setPen(edgePen);
    p.drawLine(QPointF(r.left() - span * 0.15, y0),
               QPointF(r.right() + span * 0.15, y0 + std::sin(phase) * std::max(1.0, s_.waterSurfaceWaveHeight * 0.08)));
    p.restore();
}

void EffectsWidget::drawIce(QPainter &p, const QRect &r, double t) {
    Q_UNUSED(t);
    const double y0 = r.top() + r.height() * s_.iceY;
    const QRectF iceRect(r.left(), y0, r.width(), r.bottom() - y0);
    QLinearGradient g(0, y0, 0, r.bottom());
    QColor top = s_.iceColor; top.setAlpha(s_.iceAlpha);
    QColor bot = s_.iceColor; bot.setAlpha(int(s_.iceAlpha * 0.55));
    g.setColorAt(0.0, top);
    g.setColorAt(1.0, bot);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRect(iceRect);
    // Seeded crack polylines.
    QColor edge = s_.iceEdgeColor; edge.setAlpha(140);
    p.setPen(QPen(edge, 1.0));
    for (int i = 0; i + 3 < iceCracks_.size(); i += 4)
        p.drawPolyline(iceCracks_.constData() + i, 4);
    // Bubbles.
    QColor hi = s_.iceHighlightColor; hi.setAlpha(120);
    p.setPen(QPen(hi, 1.0));
    p.setBrush(Qt::NoBrush);
    for (const QPointF &b : iceBubbles_)
        p.drawEllipse(b, 3.0, 2.2);
    // Fog: gradient band over the top of the ice (Python _draw_ice_fog_fast).
    if (s_.iceFogEnabled && s_.iceFogAlpha > 0) {
        const double fogH = iceRect.height() * std::clamp(s_.iceFogHeight, 0.02, 0.80);
        const QRectF fogRect(iceRect.left(), iceRect.top(), iceRect.width(), fogH);
        QLinearGradient fg(0, fogRect.top(), 0, fogRect.bottom());
        QColor f0 = s_.iceFogColor; f0.setAlpha(int(s_.iceFogAlpha * 0.18));
        QColor f1 = s_.iceFogColor; f1.setAlpha(s_.iceFogAlpha);
        QColor f2 = s_.iceFogColor; f2.setAlpha(0);
        fg.setColorAt(0.0, f0);
        fg.setColorAt(0.42, f1);
        fg.setColorAt(1.0, f2);
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRect(fogRect);
    }
}

// =====================================================================
// Scenic engines (v0.8.0) — Python _draw_uyuni_salt_flat_engine /
// _draw_fullscreen_scenic_engine / _draw_sahara_desert_engine and the
// realtime sky they share. Static layers are cached pixmaps (regenerated
// on resize only); per-frame work is gradients, points and blits.
// =====================================================================
void EffectsWidget::drawScenes(QPainter &p, const QRect &r) {
    const int w = r.width(), h = r.height();
    if (s_.uyuniEnabled) {
        p.drawPixmap(r.topLeft(), sceneStatic(1, w, h));
        if (s_.scenicSkyEnabled) drawRealtimeSky(p, r, 1);
        drawAtmosphere(p, r, 1);
    }
    if (s_.chichibugahamaEnabled) {
        p.drawPixmap(r.topLeft(), sceneStatic(2, w, h));
        if (s_.scenicSkyEnabled) drawRealtimeSky(p, r, 2);
        drawAtmosphere(p, r, 2);
        drawChichibugahamaDynamic(p, r);
    }
    if (s_.pamukkaleEnabled) {
        p.drawPixmap(r.topLeft(), sceneStatic(3, w, h));
        drawPamukkaleDynamic(p, r);
    }
    if (s_.saharaEnabled) {
        p.drawPixmap(r.topLeft(), sceneStatic(4, w, h));
        if (s_.scenicSkyEnabled) drawRealtimeSky(p, r, 0);
        drawTumbleweeds(p, r);
    }
}

QPixmap EffectsWidget::sceneStatic(int kind, int w, int h) {
    if (sceneCacheSize_ != QSize(w, h)) {
        sceneStaticCache_.clear();
        mistCache_.clear();
        sceneCacheSize_ = QSize(w, h);
    }
    auto it = sceneStaticCache_.find(kind);
    if (it != sceneStaticCache_.end()) return it.value();
    QPixmap pm;
    switch (kind) {
        case 1: pm = renderUyuniStatic(w, h); break;
        case 2: pm = renderChichibugahamaStatic(w, h); break;
        case 3: pm = renderPamukkaleStatic(w, h); break;
        default: pm = renderSaharaStatic(w, h); break;
    }
    sceneStaticCache_.insert(kind, pm);
    return pm;
}

QPixmap EffectsWidget::renderUyuniStatic(int w, int h) {
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter q(&pm);
    q.setRenderHint(QPainter::Antialiasing, true);
    const double horizon = h * 0.50;
    QLinearGradient sky(0, 0, 0, horizon);
    sky.setColorAt(0.0, QColor("#CDEBFF"));
    sky.setColorAt(0.55, QColor("#F4FCFF"));
    sky.setColorAt(1.0, QColor("#FFFFFF"));
    q.setPen(Qt::NoPen);
    q.setBrush(sky);
    q.drawRect(QRectF(0, 0, w, horizon + 1));
    QLinearGradient water(0, horizon, 0, h);
    QColor c0("#FFFFFF"); c0.setAlpha(238);
    QColor c1("#C7EAF7"); c1.setAlpha(205);
    QColor c2("#F9FEFF"); c2.setAlpha(242);
    water.setColorAt(0.0, c0);
    water.setColorAt(0.45, c1);
    water.setColorAt(1.0, c2);
    q.setBrush(water);
    q.drawRect(QRectF(0, horizon, w, h - horizon));
    QRandomGenerator rng(quint32((w * 1009) ^ (h * 9176) ^ 0x5551));
    // 10 distant cloud streaks, each with a compressed reflection.
    for (int band = 0; band < 10; ++band) {
        const double y = horizon * uni(rng, 0.10, 0.73);
        const double x = uni(rng, -w * 0.1, w * 0.88);
        const double ww = uni(rng, w * 0.18, w * 0.45);
        const double hh = uni(rng, 3.0, 10.0);
        QColor col("#FFFFFF"); col.setAlpha(randInt(rng, 42, 78));
        q.setBrush(col);
        q.setPen(Qt::NoPen);
        q.drawRoundedRect(QRectF(x, y, ww, hh), hh, hh);
        QColor ref("#FFFFFF"); ref.setAlpha(std::max(18, col.alpha() - 22));
        q.setBrush(ref);
        q.drawRoundedRect(QRectF(x, horizon + (horizon - y) * 0.84, ww, std::max(1.0, hh * 0.55)), hh, hh);
    }
    QColor hc("#E6F8FF"); hc.setAlpha(190);
    q.setPen(QPen(hc, 1.0));
    q.drawLine(QPointF(0, horizon), QPointF(w, horizon));
    // 38 shimmer lines, denser near the horizon.
    for (int i = 0; i < 38; ++i) {
        const double y = horizon + std::pow(i / 38.0, 1.65) * (h - horizon) * 0.92;
        QColor col("#FFFFFF"); col.setAlpha(std::max(8, int(38 * (1.0 - i / 46.0))));
        q.setPen(QPen(col, 1.0));
        const double margin = w * (0.04 + 0.08 * std::sin(i * 1.73));
        q.drawLine(QPointF(margin, y), QPointF(w - margin, y + std::sin(i) * 0.8));
    }
    // Salt cells in the foreground.
    QColor salt("#FFFFFF"); salt.setAlpha(48);
    q.setPen(QPen(salt, 1.0));
    q.setBrush(Qt::NoBrush);
    const int cell = std::max(24, w / 32);
    const int y0 = int(h * 0.76);
    for (int yy = y0; yy < h + cell; yy += cell)
        for (int xx = -cell; xx < w + cell; xx += cell)
            if ((xx / cell + yy / cell) % 3 == 0)
                q.drawEllipse(QRectF(xx, yy, cell * 1.5, cell * 0.42));
    // Central light axis.
    QLinearGradient axis(0, 0, 0, h);
    QColor a("#FFFFFF"); a.setAlpha(0);
    QColor b("#FFFFFF"); b.setAlpha(38);
    axis.setColorAt(0.0, a);
    axis.setColorAt(0.5, b);
    axis.setColorAt(1.0, a);
    q.setPen(Qt::NoPen);
    q.setBrush(axis);
    q.drawRect(QRectF(w * 0.47, 0, w * 0.06, h));
    return pm;
}

QPixmap EffectsWidget::renderChichibugahamaStatic(int w, int h) {
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter q(&pm);
    q.setRenderHint(QPainter::Antialiasing, true);
    const double horizon = h * 0.48;
    q.setPen(Qt::NoPen);
    QLinearGradient sky(0, 0, 0, horizon);
    sky.setColorAt(0.0, QColor("#BFE9FF"));
    sky.setColorAt(0.50, QColor("#F2FBFF"));
    sky.setColorAt(1.0, QColor("#FFF8E8"));
    q.setBrush(sky);
    q.drawRect(QRectF(0, 0, w, horizon + 2));
    QLinearGradient water(0, horizon, 0, h);
    water.setColorAt(0.0, QColor("#FFF8EA"));
    water.setColorAt(0.18, QColor("#F0FAFF"));
    water.setColorAt(0.54, QColor("#CFEAF5"));
    water.setColorAt(1.0, QColor("#89AFC8"));
    q.setBrush(water);
    q.drawRect(QRectF(0, horizon, w, h - horizon));
    QLinearGradient bloom(0, horizon - h * 0.035, 0, horizon + h * 0.16);
    bloom.setColorAt(0.0, QColor(255, 255, 255, 0));
    bloom.setColorAt(0.34, QColor(255, 250, 230, 70));
    bloom.setColorAt(1.0, QColor(255, 255, 255, 0));
    q.setBrush(bloom);
    q.drawRect(QRectF(0, horizon - h * 0.035, w, h * 0.20));
    q.setBrush(Qt::NoBrush);
    for (int i = 0; i < 18; ++i) {
        const double y = horizon + (h - horizon) * (i + 1) / 21.0;
        q.setPen(QPen(QColor(255, 255, 255, std::max(10, 52 - i * 2)), 1.0));
        q.drawLine(QPointF(w * 0.035, y), QPointF(w * 0.965, y + std::sin(i * 0.7) * 1.1));
    }
    q.setPen(QPen(QColor(105, 116, 118, 34), std::max(1.0, h * 0.0013)));
    q.drawLine(QPointF(0, horizon + h * 0.012), QPointF(w, horizon + h * 0.006));
    q.setPen(QPen(QColor(255, 255, 255, 34), 1.0));
    q.drawLine(QPointF(0, horizon + h * 0.021), QPointF(w, horizon + h * 0.018));
    drawSoftVignette(q, w, h, QColor(20, 46, 90, 22));
    return pm;
}

QPixmap EffectsWidget::renderPamukkaleStatic(int w, int h) {
    QPixmap pm(w, h);
    pm.fill(QColor("#F8FFFF"));
    QPainter q(&pm);
    q.setRenderHint(QPainter::Antialiasing, true);
    q.setPen(Qt::NoPen);
    const double skyH = h * 0.245;
    QRandomGenerator rng(quint32((w * 12007) ^ (h * 27011) ^ 0x50414D74));
    auto reliefLine = [&](double y, double amp, int seed, double left, double right, int points) {
        QRandomGenerator rr{quint32(seed)};
        const double x0 = w * left, x1 = w * right;
        QPainterPath path;
        path.moveTo(QPointF(x0, y));
        for (int i = 1; i <= points; ++i) {
            const double t = i / double(points);
            const double x = x0 + (x1 - x0) * t;
            const double yy = y + std::sin(t * 6.2831853 + seed * 0.019) * amp * 0.42
                              + uni(rr, -amp, amp) * 0.30;
            path.lineTo(QPointF(x, yy));
        }
        return path;
    };
    auto reliefBand = [&](double y, double height, int seed, QPainterPath &rimOut) {
        QRandomGenerator rr{quint32(seed + 777)};
        QVector<QPointF> topPts, botPts;
        for (int i = 0; i < 28; ++i) {
            const double t = i / 27.0;
            const double x = w * -0.10 + (w * 1.10 - w * -0.10) * t;
            topPts.append(QPointF(x, y + std::sin(t * 6.2831853 + seed * 0.021) * height * 0.045
                                     + uni(rr, -height * 0.045, height * 0.045)));
            botPts.append(QPointF(x, y + height + std::sin(t * 6.2831853 + seed * 0.031) * height * 0.070
                                     + uni(rr, -height * 0.070, height * 0.070)));
        }
        QPainterPath path;
        path.moveTo(topPts.first());
        for (int i = 1; i < topPts.size(); ++i) path.lineTo(topPts[i]);
        for (int i = botPts.size() - 1; i >= 0; --i) path.lineTo(botPts[i]);
        path.closeSubpath();
        rimOut = QPainterPath();
        rimOut.moveTo(topPts.first());
        for (int i = 1; i < topPts.size(); ++i) rimOut.lineTo(topPts[i]);
        return path;
    };
    QLinearGradient sky(0, 0, 0, skyH);
    sky.setColorAt(0.0, QColor("#EAFBFF"));
    sky.setColorAt(0.72, QColor("#FBFEFF"));
    sky.setColorAt(1.0, QColor("#FFFFFF"));
    q.setBrush(sky);
    q.drawRect(QRectF(0, 0, w, skyH + 2));
    QLinearGradient base(0, skyH, 0, h);
    base.setColorAt(0.0, QColor("#FFFFFF"));
    base.setColorAt(0.48, QColor("#F5FCFA"));
    base.setColorAt(0.82, QColor("#DDECEA"));
    base.setColorAt(1.0, QColor("#FAFFFF"));
    q.setBrush(base);
    q.drawRect(QRectF(0, skyH, w, h - skyH));
    // One continuous travertine field.
    QPainterPath field;
    field.moveTo(-w * 0.16, skyH + h * 0.11);
    field.cubicTo(w * 0.12, skyH + h * 0.045, w * 0.34, skyH + h * 0.145, w * 0.53, skyH + h * 0.090);
    field.cubicTo(w * 0.72, skyH + h * 0.040, w * 0.92, skyH + h * 0.135, w * 1.16, skyH + h * 0.080);
    field.lineTo(w * 1.18, h * 1.12);
    field.lineTo(-w * 0.18, h * 1.12);
    field.closeSubpath();
    QLinearGradient fieldGrad(0, skyH, 0, h);
    fieldGrad.setColorAt(0.0, QColor(255, 255, 255, 248));
    fieldGrad.setColorAt(0.58, QColor(247, 253, 250, 244));
    fieldGrad.setColorAt(1.0, QColor(220, 236, 233, 242));
    q.setBrush(fieldGrad);
    q.drawPath(field);
    QLinearGradient light(0, skyH, 0, h * 0.60);
    light.setColorAt(0.0, QColor(255, 255, 255, 92));
    light.setColorAt(0.50, QColor(255, 255, 255, 30));
    light.setColorAt(1.0, QColor(255, 255, 255, 0));
    q.setBrush(light);
    q.drawRect(QRectF(0, skyH, w, h * 0.50));
    // Terrace rows, growing with depth.
    double terraceY = skyH + h * 0.045;
    double step = h * 0.030;
    int row = 0;
    QVector<QPointF> prevFlows;
    while (terraceY < h * 0.925 && row < 400) {
        const double depth = std::clamp((terraceY - skyH) / std::max(1.0, h - skyH), 0.0, 1.0);
        const double scale = 0.42 + std::pow(depth, 1.18);
        const double bandH = h * (0.014 + 0.040 * scale);
        const int seed = row * 149 + 31;
        QPainterPath rim;
        const QPainterPath band = reliefBand(terraceY, bandH, seed, rim);
        const double shadowY = terraceY + std::max(5.0, h * (0.006 + 0.006 * depth));
        const int shadowAlpha = int(18 + 54 * depth);
        QLinearGradient shadow(0, shadowY, 0, shadowY + bandH * 2.05);
        shadow.setColorAt(0.0, QColor(24, 72, 78, std::min(82, shadowAlpha)));
        shadow.setColorAt(0.45, QColor(24, 72, 78, int(shadowAlpha * 0.48)));
        shadow.setColorAt(1.0, QColor(24, 72, 78, 0));
        q.setBrush(shadow);
        q.setPen(Qt::NoPen);
        q.drawRect(QRectF(0, shadowY, w, bandH * 2.10));
        QLinearGradient face(0, terraceY - bandH * 0.10, 0, terraceY + bandH * 1.10);
        face.setColorAt(0.0, QColor(255, 255, 255, 255));
        face.setColorAt(0.50, QColor(250, 255, 253, int(250 - 8 * depth)));
        face.setColorAt(0.82, QColor(237, 248, 245, int(236 + 8 * depth)));
        face.setColorAt(1.0, QColor(215, 235, 232, int(210 + 26 * depth)));
        q.setBrush(face);
        q.drawPath(band);
        const double edgeW = std::max(1.6, h * (0.0020 + 0.0023 * depth));
        q.setBrush(Qt::NoBrush);
        q.setPen(QPen(QColor(255, 255, 255, 255), edgeW * 1.85));
        q.drawPath(rim);
        q.setPen(QPen(QColor(156, 191, 188, int(24 + 56 * depth)), std::max(1.0, edgeW * 0.65)));
        q.drawPath(reliefLine(terraceY + bandH * 0.94, std::max(1.0, bandH * 0.045), seed + 61, -0.02, 1.02, 20));
        q.setPen(QPen(QColor(255, 255, 255, int(18 + 12 * (1.0 - depth))), 1.0));
        const int scratches = 6 + int(8 * depth);
        for (int i = 0; i < scratches; ++i) {
            const double x = uni(rng, w * 0.04, w * 0.96);
            const double y = terraceY + uni(rng, bandH * 0.18, bandH * 0.88);
            const double len = w * uni(rng, 0.010, 0.040) * scale;
            q.drawLine(QPointF(x - len * 0.5, y), QPointF(x + len * 0.5, y + uni(rng, -1.0, 1.0)));
        }
        const int waterAlpha = int(5 + 24 * depth);
        if (waterAlpha > 7) {
            q.setPen(QPen(QColor(210, 244, 248, waterAlpha),
                          std::max(1.0, h * (0.0008 + 0.0012 * depth))));
            q.drawPath(reliefLine(terraceY + bandH * 0.14, std::max(1.0, bandH * 0.030), seed + 83, 0.06, 0.94, 16));
        }
        const int flowCount = depth < 0.50 ? 2 : 3;
        QVector<QPointF> flows;
        for (int i = 0; i < flowCount; ++i)
            flows.append(QPointF(w * (0.28 + 0.20 * i) + std::sin(row * 0.66 + i) * w * 0.020,
                                 terraceY + bandH * 0.56));
        if (!prevFlows.isEmpty()) {
            q.setPen(QPen(QColor(214, 247, 249, int(7 + 22 * depth)),
                          std::max(1.0, h * (0.0010 + 0.0020 * depth))));
            q.setBrush(Qt::NoBrush);
            for (int i = 0; i < flows.size(); ++i) {
                const QPointF pv = prevFlows[std::min(int(prevFlows.size()) - 1, i)];
                const QPointF cur = flows[i];
                const double midX = (pv.x() + cur.x()) * 0.5 + std::sin(row + i) * w * 0.008;
                QPainterPath flow;
                flow.moveTo(QPointF(pv.x(), pv.y() + h * 0.010));
                flow.cubicTo(QPointF(midX, pv.y() + h * 0.021), QPointF(midX, cur.y() - h * 0.021),
                             QPointF(cur.x(), cur.y() - h * 0.010));
                q.drawPath(flow);
            }
        }
        prevFlows = flows;
        terraceY += step * uni(rng, 0.990, 1.035);
        step *= 1.105;
        ++row;
    }
    const double floorY = h * 0.860;
    QLinearGradient floorG(0, floorY, 0, h);
    floorG.setColorAt(0.0, QColor(255, 255, 255, 226));
    floorG.setColorAt(0.52, QColor(247, 252, 250, 236));
    floorG.setColorAt(1.0, QColor(223, 238, 235, 238));
    q.setBrush(floorG);
    q.setPen(Qt::NoPen);
    q.drawRect(QRectF(0, floorY, w, h - floorY));
    const int grain = std::max(260, int(w * double(h) / 6800));
    for (int i = 0; i < grain; ++i) {
        const double tx = rng.generateDouble() * w;
        const double ty = skyH + rng.generateDouble() * (h - skyH);
        if (rng.generateDouble() < 0.80)
            q.setPen(QPen(QColor(255, 255, 255, randInt(rng, 9, 38)), 1.0));
        else
            q.setPen(QPen(QColor(202, 220, 216, randInt(rng, 6, 18)), 1.0));
        q.drawPoint(QPointF(tx, ty));
    }
    QRadialGradient dry(QPointF(w * 0.50, h * 0.52), std::max(w, h) * 0.78);
    dry.setColorAt(0.0, QColor(255, 255, 255, 58));
    dry.setColorAt(0.58, QColor(255, 255, 255, 28));
    dry.setColorAt(1.0, QColor(255, 255, 255, 0));
    q.setBrush(dry);
    q.drawRect(QRectF(0, skyH, w, h - skyH));
    drawSoftVignette(q, w, h, QColor(45, 116, 126, 12));
    return pm;
}

QPixmap EffectsWidget::renderSaharaStatic(int w, int h) {
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter q(&pm);
    q.setRenderHint(QPainter::Antialiasing, true);
    QRandomGenerator rng(quint32((w * 104729) ^ (h * 13007) ^ 0x5A484152));
    const double horizon = h * 0.44;
    QLinearGradient sky(0, 0, 0, horizon);
    sky.setColorAt(0.0, QColor("#5EA7E8"));
    sky.setColorAt(0.48, QColor("#BBDFFF"));
    sky.setColorAt(0.82, QColor("#FFE2A6"));
    sky.setColorAt(1.0, QColor("#F9C27B"));
    q.setPen(Qt::NoPen);
    q.setBrush(sky);
    q.drawRect(QRectF(0, 0, w, horizon + 2));
    for (int i = 0; i < 16; ++i) {
        const double y = horizon + i * h * 0.012;
        QColor c("#FFF2C8"); c.setAlpha(24 - std::min(20, i));
        q.setPen(QPen(c, 1.0));
        q.drawLine(QPointF(w * uni(rng, -0.05, 0.12), y),
                   QPointF(w * uni(rng, 0.80, 1.08), y + uni(rng, -1.0, 1.0)));
    }
    QLinearGradient sand(0, horizon, 0, h);
    sand.setColorAt(0.0, QColor("#E5A85D"));
    sand.setColorAt(0.45, QColor("#D58C3F"));
    sand.setColorAt(1.0, QColor("#8B4E25"));
    q.setBrush(sand);
    q.setPen(Qt::NoPen);
    q.drawRect(QRectF(0, horizon, w, h - horizon));
    struct Dune { double base; const char *hi; const char *sh; double amp; };
    const Dune dunes[4] = {{0.50, "#F1BE76", "#C77B36", 0.14}, {0.60, "#E6A65A", "#B96C31", 0.20},
                           {0.73, "#D68A3F", "#8F4C25", 0.30}, {0.88, "#B76632", "#5B2E18", 0.40}};
    for (int idx = 0; idx < 4; ++idx) {
        const Dune &d = dunes[idx];
        const double y0 = h * d.base;
        QPainterPath path;
        path.moveTo(0, h);
        path.lineTo(0, y0);
        for (int sIdx = 0; sIdx < 8; ++sIdx) {
            const double x = w * sIdx / 7.0;
            const double y = y0 + std::sin(sIdx * 1.25 + idx * 0.8) * h * d.amp * 0.13
                             + std::cos(sIdx * 0.53 + idx) * h * d.amp * 0.06;
            path.lineTo(x, y);
        }
        path.lineTo(w, h);
        path.closeSubpath();
        QLinearGradient grad(0, y0, 0, h);
        grad.setColorAt(0.0, QColor(d.hi));
        grad.setColorAt(1.0, QColor(d.sh));
        q.setPen(Qt::NoPen);
        q.setBrush(grad);
        q.drawPath(path);
        q.setPen(QPen(QColor(255, 219, 155, 45), 1.0));
        for (int line = 0; line < 5; ++line) {
            const double yy = y0 + h * (0.025 + line * 0.036 + idx * 0.01);
            q.drawLine(QPointF(w * 0.04, yy), QPointF(w * 0.96, yy + std::sin(line + idx) * 2.0));
        }
    }
    const int grain = std::max(120, int(w * double(h) / 9000));
    for (int i = 0; i < grain; ++i) {
        QColor c("#FFE0A0"); c.setAlpha(randInt(rng, 12, 45));
        q.setPen(QPen(c, 1.0));
        q.drawPoint(QPointF(rng.generateDouble() * w, uni(rng, h * 0.50, h * 0.98)));
    }
    // Static dry tufts.
    const int tufts = std::max(12, w / 95);
    for (int i = 0; i < tufts; ++i) {
        const double x = uni(rng, w * 0.02, w * 0.98);
        const double y = uni(rng, h * 0.70, h * 0.96);
        const double size = uni(rng, 8.0, 22.0) * (0.75 + 0.55 * (y / std::max(1.0, double(h))));
        q.setPen(QPen(QColor(112, 74, 33, 130), std::max(0.8, size * 0.045)));
        for (int k = 0; k < 7; ++k) {
            const double a = -3.14159265 * 0.92 + k * 3.14159265 * 0.30 + uni(rng, -0.10, 0.10);
            const double len = size * uni(rng, 0.45, 1.05);
            q.drawLine(QPointF(x, y), QPointF(x + std::cos(a) * len, y - std::abs(std::sin(a)) * len));
        }
        q.setPen(Qt::NoPen);
        q.setBrush(QColor(50, 28, 8, 38));
        q.drawEllipse(QRectF(x - size * 0.38, y - size * 0.03, size * 0.76, size * 0.12));
    }
    QRadialGradient vig(QPointF(w * 0.50, h * 0.48), std::max(w, h) * 0.76);
    vig.setColorAt(0.0, QColor(255, 220, 130, 0));
    vig.setColorAt(0.68, QColor(255, 150, 55, 18));
    vig.setColorAt(1.0, QColor(54, 23, 5, 92));
    q.setPen(Qt::NoPen);
    q.setBrush(vig);
    q.drawRect(QRectF(0, 0, w, h));
    return pm;
}

void EffectsWidget::drawSunGlow(QPainter &p, double x, double y, double radius, double strength,
                                 bool hot, bool cool, double rayCenter, double raySpread, double rayAlphaScale) {
    auto clampA = [](double v) { return int(std::clamp(v, 0.0, 255.0)); };
    const double outer = radius * (hot ? 4.4 : 3.5);
    QRadialGradient grad(QPointF(x, y), outer);
    if (cool) {
        grad.setColorAt(0.0, QColor(255, 255, 255, clampA(190 * strength)));
        grad.setColorAt(0.16, QColor(246, 252, 255, clampA(112 * strength)));
        grad.setColorAt(0.46, QColor(214, 238, 255, clampA((hot ? 42 : 30) * strength)));
        grad.setColorAt(1.0, QColor(210, 235, 255, 0));
    } else {
        grad.setColorAt(0.0, QColor(255, 255, 222, clampA(210 * strength)));
        grad.setColorAt(0.13, QColor(255, 234, 130, clampA(145 * strength)));
        grad.setColorAt(0.42, QColor(255, 176, 56, clampA((hot ? 70 : 48) * strength)));
        grad.setColorAt(1.0, QColor(255, 132, 28, 0));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(grad);
    p.drawEllipse(QPointF(x, y), outer, outer);
    if (cool)
        p.setBrush(QColor(255, 255, 255, clampA(210 * std::min(1.0, strength))));
    else
        p.setBrush(QColor(255, 248, 194, clampA(232 * std::min(1.0, strength))));
    p.drawEllipse(QPointF(x, y), radius, radius);
    if (s_.sunRaysEnabled) {
        double baseAlpha = cool ? 36 : 54;
        double baseWidth = radius * (cool ? 0.020 : 0.025);
        if (!hot) { baseAlpha *= 0.62; baseWidth *= 0.78; }
        const int rayAlpha = clampA(baseAlpha * strength * std::clamp(rayAlphaScale, 0.0, 2.0));
        QPen pen(cool ? QColor(245, 252, 255, rayAlpha) : QColor(255, 234, 155, rayAlpha),
                 std::max(1.0, baseWidth));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const double spread = std::clamp(raySpread, 0.05, 2.40);
        for (int i = 0; i < 18; ++i) {
            const double a = rayCenter - spread + (spread * 2.0) * i / 17.0;
            p.drawLine(QPointF(x, y), QPointF(x + std::cos(a) * outer * 1.55, y + std::sin(a) * outer * 1.55));
        }
    }
}

void EffectsWidget::drawStars(QPainter &p, const QRect &r, double horizonRatio, double alphaScale) {
    const double w = r.width(), h = r.height();
    const double horizon = r.top() + h * horizonRatio;
    const double twinkle = 0.5 + 0.5 * std::sin(time_ * 0.9);
    for (const Star &st : stars_) {
        const double local = 0.65 + 0.35 * std::sin(time_ * st.twSpeed + st.twPhase);
        const int alpha = int(std::clamp(st.base * alphaScale * (0.72 + 0.28 * twinkle) * local, 0.0, 255.0));
        if (alpha <= 3) continue;
        const QPointF pt(r.left() + st.x * w, r.top() + st.y * std::max(1.0, horizon - r.top()) * 0.92);
        p.setPen(QPen(QColor(245, 250, 255, alpha), 1.0));
        p.drawPoint(pt);
        if (st.cross) {
            p.setPen(QPen(QColor(210, 225, 255, std::max(8, alpha / 3)), 1.0));
            p.drawLine(QPointF(pt.x() - 2, pt.y()), QPointF(pt.x() + 2, pt.y()));
            p.drawLine(QPointF(pt.x(), pt.y() - 2), QPointF(pt.x(), pt.y() + 2));
        }
    }
}

void EffectsWidget::drawShootingStar(QPainter &p, const QRect &r, double horizonRatio) {
    const double period = 28.0, visible = 1.6;
    const double phase = std::fmod(time_, period);
    if (phase > visible) return;
    const double progress = std::clamp(phase / visible, 0.0, 1.0);
    const double w = r.width(), h = r.height();
    const double horizon = r.top() + h * horizonRatio;
    QRandomGenerator rng(quint32(int(time_ / period) ^ int(w * 13) ^ int(h * 17)));
    const double startX = r.left() + w * uni(rng, 0.08, 0.62);
    const double startY = r.top() + (horizon - r.top()) * uni(rng, 0.08, 0.38);
    const double travel = w * uni(rng, 0.22, 0.36);
    const double drop = h * uni(rng, 0.055, 0.12);
    const QPointF head(startX + travel * progress, startY + drop * progress);
    const QPointF tail(head.x() - travel * 0.42, head.y() - drop * 0.42);
    const int alpha = int(230 * std::sin(progress * 3.14159265));
    if (alpha <= 2) return;
    p.setPen(QPen(QColor(230, 245, 255, alpha), 2.0));
    p.drawLine(tail, head);
    p.setPen(QPen(QColor(255, 255, 255, std::min(255, alpha + 25)), 3.0));
    p.drawPoint(head);
}

void EffectsWidget::drawRealtimeSky(QPainter &p, const QRect &r, int skyScene) {
    // skyScene: 0 desert, 1 uyuni, 2 chichibugahama (Python scene strings).
    const SkyState st = skyState();
    const double w = r.width(), h = r.height();
    const double left = r.left(), top = r.top();
    const double horizonRatio = skyScene == 1 ? 0.50 : (skyScene == 2 ? 0.48 : 0.44);
    const double horizon = top + h * horizonRatio;
    const QRectF skyRect(left, top, w, std::max(1.0, horizon - top + 2.0));
    const QRectF groundRect(left, horizon, w, std::max(1.0, top + h - horizon));
    const bool uyuni = skyScene == 1;
    p.save();
    p.setPen(Qt::NoPen);
    if (st.phase == 3) { // night
        QLinearGradient sky(0, top, 0, horizon);
        sky.setColorAt(0.0, QColor(4, 8, 32, 214));
        sky.setColorAt(0.48, QColor(10, 18, 48, 196));
        sky.setColorAt(1.0, QColor(32, 24, 52, skyScene == 0 ? 148 : 118));
        p.setBrush(sky);
        p.drawRect(skyRect);
        QLinearGradient ground(0, horizon, 0, top + h);
        ground.setColorAt(0.0, QColor(12, 20, 42, uyuni ? 70 : 92));
        ground.setColorAt(1.0, QColor(2, 3, 12, uyuni ? 118 : 150));
        p.setBrush(ground);
        p.drawRect(groundRect);
        drawStars(p, r, horizonRatio, 1.0);
        drawShootingStar(p, r, horizonRatio);
        const double mx = left + w * 0.78, my = top + h * 0.16;
        const double mr = std::max(18.0, std::min(w, h) * 0.045);
        // Halo: dithered cached glow with a smoothed stop tail (v0.8.1) —
        // Python's 3-stop gradient bands into visible rings on dark skies.
        if (nightHaloCache_.isNull() || nightHaloCache_.width() != int(std::ceil(mr * 8.4)) + 2) {
            nightHaloCache_ = makeGlowPixmap(mr * 4.2, {
                {0.0, QColor(210, 230, 255, 92)},
                {0.30, QColor(140, 175, 255, 38)},
                {0.55, QColor(110, 150, 245, 15)},
                {0.78, QColor(95, 135, 230, 5)},
                {1.0, QColor(85, 125, 225, 0)}});
        }
        p.drawPixmap(QPointF(mx - nightHaloCache_.width() / 2.0, my - nightHaloCache_.height() / 2.0),
                     nightHaloCache_);
        p.setBrush(QColor(238, 242, 255, 150));
        p.drawEllipse(QPointF(mx, my), mr, mr);
    } else if (st.phase == 0) { // dawn
        const double t = st.t;
        QLinearGradient sky(0, top, 0, horizon);
        sky.setColorAt(0.0, QColor(20, 30, 84, int(150 * (1.0 - t) + 40 * t)));
        sky.setColorAt(0.40, QColor(255, 116, 92, int(120 + 55 * t)));
        sky.setColorAt(0.74, QColor(255, 196, 112, int(150 + 50 * t)));
        sky.setColorAt(1.0, QColor(255, 236, 175, int(160 + 35 * t)));
        p.setBrush(sky);
        p.drawRect(skyRect);
        drawSunGlow(p, left + w * (0.22 + 0.12 * t), horizon - h * (0.03 + 0.18 * t),
                    std::max(44.0, std::min(w, h) * (0.075 + 0.025 * t)),
                    0.78 + 0.18 * t, false, uyuni, 0.30, 0.38, uyuni ? 0.26 : 0.36);
        QLinearGradient warm(0, horizon, 0, top + h);
        warm.setColorAt(0.0, QColor(255, 174, 96, uyuni ? 58 : 74));
        warm.setColorAt(1.0, QColor(115, 54, 18, uyuni ? 24 : 52));
        p.setBrush(warm);
        p.drawRect(groundRect);
    } else if (st.phase == 1) { // day
        const double t = st.t;
        const double noon = std::clamp(1.0 - std::abs(t - 0.5) * 2.0, 0.0, 1.0);
        QLinearGradient sky(0, top, 0, horizon);
        sky.setColorAt(0.0, QColor(64, 146, 232, int(62 + 36 * noon)));
        sky.setColorAt(0.58, QColor(180, 225, 255, int(48 + 30 * noon)));
        sky.setColorAt(1.0, QColor(255, 235, 174, int(74 + 36 * noon)));
        p.setBrush(sky);
        p.drawRect(skyRect);
        const double sunX = left + w * (0.18 + 0.64 * t);
        const double sunY = top + h * (0.12 + 0.08 * std::sin(t * 3.14159265));
        drawSunGlow(p, sunX, sunY, std::max(54.0, std::min(w, h) * 0.105),
                    1.08 + 0.34 * noon, true, uyuni,
                    sunX > left + w * 0.52 ? 3.14159265 - 0.30 : 0.30, 0.48, uyuni ? 0.66 : 0.92);
        QRadialGradient glare(QPointF(sunX, sunY), std::max(w, h) * (skyScene == 0 ? 0.70 : 0.54));
        if (uyuni) {
            glare.setColorAt(0.0, QColor(255, 255, 255, int(38 + 38 * noon)));
            glare.setColorAt(0.34, QColor(230, 248, 255, int(18 + 22 * noon)));
            glare.setColorAt(1.0, QColor(220, 245, 255, 0));
        } else {
            glare.setColorAt(0.0, QColor(255, 244, 180, int(52 + 58 * noon)));
            glare.setColorAt(0.34, QColor(255, 202, 92, int(24 + 30 * noon)));
            glare.setColorAt(1.0, QColor(255, 160, 50, 0));
        }
        p.setBrush(glare);
        p.drawRect(QRectF(left, top, w, h));
        if (skyScene == 0) {
            QLinearGradient heat(0, horizon, 0, top + h);
            heat.setColorAt(0.0, QColor(255, 220, 130, int(42 + 30 * noon)));
            heat.setColorAt(0.46, QColor(255, 126, 28, int(26 + 26 * noon)));
            heat.setColorAt(1.0, QColor(110, 42, 8, int(36 + 34 * noon)));
            p.setBrush(heat);
            p.drawRect(groundRect);
        } else {
            QLinearGradient mirror(0, horizon, 0, top + h);
            mirror.setColorAt(0.0, QColor(255, 255, 255, int(38 + 30 * noon)));
            mirror.setColorAt(0.55, QColor(218, 246, 255, int(22 + 18 * noon)));
            mirror.setColorAt(1.0, QColor(255, 255, 255, int(24 + 22 * noon)));
            p.setBrush(mirror);
            p.drawRect(groundRect);
        }
    } else { // dusk
        const double t = st.t;
        QLinearGradient sky(0, top, 0, horizon);
        sky.setColorAt(0.0, QColor(84, 112, 190, int(92 + 48 * t)));
        sky.setColorAt(0.44, QColor(255, 147, 86, int(105 + 58 * t)));
        sky.setColorAt(0.76, QColor(255, 194, 116, int(124 + 45 * t)));
        sky.setColorAt(1.0, QColor(178, 70, 40, int(78 + 74 * t)));
        p.setBrush(sky);
        p.drawRect(skyRect);
        drawSunGlow(p, left + w * (0.78 - 0.08 * t), horizon - h * (0.16 * (1.0 - t) + 0.015),
                    std::max(42.0, std::min(w, h) * 0.082),
                    0.82, false, uyuni, 3.14159265 - 0.38, 0.42, uyuni ? 0.36 : 0.52);
        QLinearGradient amber(0, horizon, 0, top + h);
        amber.setColorAt(0.0, QColor(255, 148, 70, uyuni ? 62 : 82));
        amber.setColorAt(0.54, QColor(198, 84, 36, uyuni ? 38 : 70));
        amber.setColorAt(1.0, QColor(70, 24, 10, uyuni ? 34 : 84));
        p.setBrush(amber);
        p.drawRect(groundRect);
        if (t > 0.58) drawStars(p, r, horizonRatio, (t - 0.58) / 0.42);
    }
    p.restore();
    if (skyScene == 1 || skyScene == 2) drawCloudReflection(p, r, skyScene);
    if (skyScene == 2) drawChichibugahamaAccents(p, r, st.phase, st.t);
}

void EffectsWidget::drawCloudReflection(QPainter &p, const QRect &r, int sceneKind) {
    if (clouds_.isEmpty()) return;
    const double w = r.width(), h = r.height();
    const double horizonRatio = sceneKind == 1 ? 0.50 : 0.48;
    const QRectF waterRect(r.left(), r.top() + h * horizonRatio, w, h * (1.0 - horizonRatio));
    const int alphaBase = sceneKind == 1 ? 150 : 118;
    const double wave = std::max(0.0, s_.waterMirrorWave);
    p.save();
    p.setClipRect(waterRect);
    for (const Cloud &c : clouds_) {
        const double surfaceY = waterRect.top();
        const double reflectedY = surfaceY + (surfaceY - c.y) * 0.34;
        if (reflectedY < waterRect.top() - c.pix.height() * 0.5 ||
            reflectedY > waterRect.bottom() + c.pix.height() * 0.8)
            continue;
        const double depthT = std::clamp((reflectedY - waterRect.top()) / std::max(1.0, waterRect.height()), 0.0, 1.0);
        const int a = std::clamp(int(alphaBase * (0.26 - depthT * 0.10)), 0, 255);
        if (a <= 0) continue;
        const double wobble = std::sin(time_ * 0.85 + reflectedY * 0.045 + c.x * 0.012) * wave * (1.0 - depthT * 0.60);
        const double scaleF = 0.84 - depthT * 0.18;
        p.save();
        p.translate(c.x + wobble, reflectedY);
        p.scale(scaleF, 0.52 * scaleF);
        p.setOpacity(a * c.alphaF / 255.0);
        p.drawPixmap(QPointF(-c.pix.width() / 2.0, -c.pix.height() / 2.0), c.pix);
        p.restore();
    }
    const int lineAlpha = sceneKind == 1 ? 14 : 10;
    const int lines = sceneKind == 1 ? 8 : 6;
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < lines; ++i) {
        const double y = waterRect.top() + waterRect.height() * (0.10 + i * 0.075);
        p.setPen(QPen(QColor(255, 255, 255, std::max(2, lineAlpha - i)), 1.0));
        p.drawLine(QPointF(r.left() + w * 0.04, y), QPointF(r.left() + w * 0.96, y + std::sin(i * 0.7) * 0.7));
    }
    p.restore();
}

void EffectsWidget::drawChichibugahamaAccents(QPainter &p, const QRect &r, int phase, double t) {
    Q_UNUSED(t);
    const double w = r.width(), h = r.height();
    const double left = r.left(), top = r.top();
    const double horizon = top + h * 0.48;
    const double waterH = std::max(1.0, top + h - horizon);
    p.save();
    p.setPen(Qt::NoPen);
    if (phase == 1) { // day: shimmer lines, reseeded every 5s
        QRandomGenerator rng(quint32(int(time_ / 5.0) ^ 0x43484943));
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 20; ++i) {
            const double y = horizon + waterH * uni(rng, 0.05, 0.62);
            const double x0 = left + w * uni(rng, 0.06, 0.78);
            const double x1 = std::min(left + w * 0.96, x0 + w * uni(rng, 0.05, 0.18));
            p.setPen(QPen(QColor(255, 255, 224, randInt(rng, 20, 46)), 1.0));
            p.drawLine(QPointF(x0, y), QPointF(x1, y + std::sin(i * 0.9) * 0.8));
        }
    } else if (phase == 2) { // dusk: warm wash over the water
        QLinearGradient warm(0, horizon, 0, top + h);
        warm.setColorAt(0.0, QColor(255, 176, 104, 34));
        warm.setColorAt(0.34, QColor(255, 128, 84, 22));
        warm.setColorAt(1.0, QColor(130, 76, 92, 0));
        p.setBrush(warm);
        p.drawRect(QRectF(left, horizon, w, waterH));
    } else if (phase == 0) { // dawn: horizon glow band
        QLinearGradient glow(0, horizon - h * 0.06, 0, horizon + h * 0.26);
        glow.setColorAt(0.0, QColor(255, 190, 128, 34));
        glow.setColorAt(0.42, QColor(255, 214, 172, 16));
        glow.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setBrush(glow);
        p.drawRect(QRectF(left, horizon - h * 0.06, w, h * 0.32));
    } else { // night: thin moon-reflection strokes
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i < 7; ++i) {
            const double y = horizon + waterH * (0.10 + i * 0.070);
            const double span = w * (0.16 + i * 0.018);
            const double cx = left + w * (0.73 + std::sin(i * 0.8) * 0.015);
            p.setPen(QPen(QColor(205, 226, 255, std::max(3, 14 - i * 2)), 1.0));
            p.drawLine(QPointF(cx - span * 0.50, y), QPointF(cx + span * 0.50, y + std::sin(i) * 0.7));
        }
    }
    p.restore();
}

QPixmap EffectsWidget::mistPixmap(int kind, int w, int h) {
    if (sceneCacheSize_ != QSize(w, h)) {
        sceneStaticCache_.clear();
        mistCache_.clear();
        sceneCacheSize_ = QSize(w, h);
    }
    auto it = mistCache_.find(kind);
    if (it != mistCache_.end()) return it.value();
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter q(&pm);
    q.setRenderHint(QPainter::Antialiasing, true);
    const double horizon = h * (kind == 1 ? 0.50 : 0.48);
    const int mistAlpha = kind == 1 ? 24 : 20;
    QLinearGradient mist(0, horizon - h * 0.12, 0, horizon + h * 0.34);
    mist.setColorAt(0.0, QColor(255, 255, 255, 0));
    mist.setColorAt(0.38, QColor(232, 248, 250, mistAlpha));
    mist.setColorAt(0.70, QColor(214, 238, 240, int(mistAlpha * 0.55)));
    mist.setColorAt(1.0, QColor(255, 255, 255, 0));
    q.setPen(Qt::NoPen);
    q.setBrush(mist);
    q.drawRect(QRectF(0, horizon - h * 0.12, w, h * 0.46));
    for (int i = 0; i < 5; ++i) {
        const double cx = w * (0.12 + i * 0.20);
        const double cy = horizon + h * (0.04 + 0.020 * std::sin(i));
        const double rad = w * (0.18 + 0.03 * i);
        QRadialGradient g(QPointF(cx, cy), rad);
        g.setColorAt(0.0, QColor(235, 250, 250, 9));
        g.setColorAt(1.0, QColor(235, 250, 250, 0));
        q.setBrush(g);
        q.drawEllipse(QRectF(cx - rad, cy - rad * 0.35, rad * 2.0, rad * 0.70));
    }
    mistCache_.insert(kind, pm);
    return pm;
}

void EffectsWidget::drawAtmosphere(QPainter &p, const QRect &r, int sceneKind) {
    if (!s_.atmosphereEnabled) return;
    const int w = r.width(), h = r.height();
    // Thin mist: cached image, breathing opacity.
    p.save();
    p.setOpacity(std::clamp(0.78 + 0.22 * std::sin(time_ * 0.028), 0.58, 1.0));
    p.drawPixmap(r.topLeft(), mistPixmap(sceneKind, w, h));
    p.restore();
    // Refractive wobble: Python regenerates an image 5x/s; here the same
    // seeded streaks are drawn directly with Screen composition.
    const int bucket = int(time_ * 5.0);
    QRandomGenerator rng(quint32((w * 977) ^ (h * 1319) ^ (bucket * 811) ^ (sceneKind == 1 ? 0x554E49 : 0x434849)));
    const int count = sceneKind == 1 ? 30 : 22;
    const double horizon = h * (sceneKind == 1 ? 0.50 : 0.48);
    const double waterH = std::max(1.0, h - horizon);
    const double phaseBase = bucket * 0.32;
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    p.setBrush(Qt::NoBrush);
    for (int i = 0; i < count; ++i) {
        const double y = horizon + waterH * std::pow((i + 1) / double(count + 2), sceneKind == 1 ? 1.35 : 1.18);
        const double length = w * uni(rng, 0.10, 0.34);
        const double cx = w * uni(rng, 0.08, 0.92);
        const double amp = h * uni(rng, 0.0008, 0.0024);
        const double phase = phaseBase * uni(rng, 0.55, 0.95) + i * 0.63;
        QPainterPath path;
        path.moveTo(QPointF(cx - length * 0.5, y));
        for (int k = 1; k < 9; ++k) {
            const double tt = k / 8.0;
            path.lineTo(QPointF(cx - length * 0.5 + length * tt, y + std::sin(tt * 6.2831853 + phase) * amp));
        }
        const int alpha = int(uni(rng, 9, 24) * (1.0 - std::min(0.72, (y - horizon) / waterH * 0.55)));
        p.setPen(QPen(QColor(245, 255, 255, alpha), std::max(1.0, h * 0.0012)));
        p.drawPath(path);
    }
    p.restore();
}

void EffectsWidget::drawPamukkaleDynamic(QPainter &p, const QRect &r) {
    const double w = r.width(), h = r.height();
    const double left = r.left(), top = r.top();
    const int tick = int(time_ / 2.0);
    QRandomGenerator rng(quint32((int(w) * 1009) ^ (int(h) * 9176) ^ (tick * 37) ^ 0x50414D));
    double terraceY = top + h * 0.315;
    double step = h * 0.064;
    QVector<QPointF> previous;
    p.save();
    for (int row = 0; row < 8; ++row) {
        const int count = 2 + (row % 2 == 0 ? 1 : 0);
        QVector<QPointF> current;
        for (int col = 0; col < count; ++col) {
            const double cx = left + w * (0.25 + col * 0.225 + uni(rng, -0.018, 0.018));
            const double cy = terraceY + uni(rng, -h * 0.002, h * 0.002);
            current.append(QPointF(cx, cy));
            const double ww = w * uni(rng, 0.034, 0.080);
            const double hh = std::max(1.4, h * uni(rng, 0.0022, 0.0040));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, randInt(rng, 7, 17)));
            p.drawRoundedRect(QRectF(cx - ww * 0.42, cy - hh * 0.5, ww, hh), hh, hh);
        }
        if (!previous.isEmpty()) {
            p.setPen(QPen(QColor(226, 248, 249, 12), std::max(1.0, h * 0.0020)));
            p.setBrush(Qt::NoBrush);
            for (int i = 0; i < current.size(); ++i) {
                const QPointF pv = previous[std::min(int(previous.size()) - 1, i)];
                const QPointF cur = current[i];
                const double midX = (pv.x() + cur.x()) * 0.5 + uni(rng, -w * 0.010, w * 0.010);
                QPainterPath path;
                path.moveTo(QPointF(pv.x(), pv.y() + h * 0.017));
                path.cubicTo(QPointF(midX, pv.y() + h * 0.024), QPointF(midX, cur.y() - h * 0.022),
                             QPointF(cur.x(), cur.y() - h * 0.017));
                p.drawPath(path);
            }
        }
        previous = current;
        terraceY += step * uni(rng, 0.95, 1.05);
        step *= 1.025;
    }
    p.restore();
}

void EffectsWidget::drawChichibugahamaDynamic(QPainter &p, const QRect &r) {
    const double w = r.width(), h = r.height();
    const int tick = int(time_ / 2.0);
    QRandomGenerator rng(quint32((int(w) * 1009) ^ (int(h) * 9176) ^ (tick * 37) ^ 0x434849));
    p.save();
    for (int i = 0; i < 26; ++i) {
        const double x = r.left() + rng.generateDouble() * w;
        const double y = r.top() + h * uni(rng, 0.38, 0.92);
        p.setPen(QPen(QColor(255, 255, 255, int(uni(rng, 24, 72))), 1.0));
        p.drawLine(QPointF(x - 12, y), QPointF(x + 12, y + uni(rng, -1.5, 1.5)));
    }
    p.restore();
}

void EffectsWidget::drawTumbleweeds(QPainter &p, const QRect &r) {
    Q_UNUSED(r);
    for (const Weed &wd : weeds_) {
        p.save();
        p.translate(wd.x, wd.y);
        p.rotate(wd.rot);
        p.scale(1.0, 0.78 + 0.08 * std::sin(time_ * 0.4 + wd.phase));
        p.drawPixmap(QPointF(-wd.sprite.width() / 2.0, -wd.sprite.height() / 2.0), wd.sprite);
        p.restore();
    }
}

void EffectsWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect r = rect();
    drawScenes(p, r); // scenic engines are fullscreen backgrounds (v0.8.0)
    if (s_.waterSurfaceEnabled) drawWater(p, r, time_);
    if (s_.iceEnabled) drawIce(p, r, time_);
    if (s_.moonBodyEnabled || s_.moonlightEnabled || s_.moonShadowEnabled) drawMoon(p, r, time_);
    if (s_.cloudEnabled) {
        for (const Cloud &c : clouds_) {
            p.setOpacity(std::clamp(s_.cloudAlpha * c.alphaF, 0.0, 255.0) / 255.0);
            p.drawPixmap(QPointF(c.x - c.pix.width() / 2.0, c.y - c.pix.height() / 2.0), c.pix);
        }
        p.setOpacity(1.0);
    }
    if (s_.snowEnabled || s_.snowCrystalEnabled) {
        for (const Flake &f : snow_) {
            if (f.crystal) {
                // Python _draw_snow_crystal: 6 arms + branchlets + centre ring.
                const int alpha = int(s_.snowCrystalAlpha * f.alphaF);
                QColor col = s_.snowCrystalColor; col.setAlpha(alpha);
                QColor edge = s_.snowCrystalEdgeColor;
                edge.setAlpha(int(alpha * 0.70));
                p.save();
                p.translate(f.x, f.y);
                p.rotate(qRadiansToDegrees(f.rotation));
                QPen pen(col, std::max(1.0, std::floor(f.size * 0.08)));
                pen.setCapStyle(Qt::RoundCap);
                p.setPen(pen);
                for (int i = 0; i < 6; ++i) {
                    const double a = 6.2832 * i / 6.0;
                    const double ex = std::cos(a) * f.size, ey = std::sin(a) * f.size;
                    p.drawLine(QPointF(0, 0), QPointF(ex, ey));
                    const double bx = std::cos(a) * f.size * 0.58, by = std::sin(a) * f.size * 0.58;
                    for (const double sign : {-1.0, 1.0}) {
                        const double ba = a + sign * 0.65;
                        p.drawLine(QPointF(bx, by),
                                   QPointF(bx + std::cos(ba) * f.size * 0.25,
                                           by + std::sin(ba) * f.size * 0.25));
                    }
                }
                p.setPen(QPen(edge, std::max(1.0, std::floor(f.size * 0.045))));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(QPointF(0, 0), std::max(1.0, f.size * 0.16), std::max(1.0, f.size * 0.16));
                p.restore();
            } else {
                // Python _draw_snow_dot: radius = size, edge 0.72 alpha.
                const int alpha = int(s_.snowAlpha * f.alphaF);
                QColor col = s_.snowColor; col.setAlpha(alpha);
                QColor edge = s_.snowEdgeColor; edge.setAlpha(int(alpha * 0.72));
                const double radius = std::max(1.0, f.size);
                p.setPen(QPen(edge, std::max(1.0, std::floor(radius * 0.20))));
                p.setBrush(col);
                p.drawEllipse(QPointF(f.x, f.y), radius, radius);
            }
        }
    }
    if (s_.rainEnabled) {
        // Python _draw_rain: thickness = round(size), alpha = 190*a,
        // length from rain_drop_length_randomness, slant = vx*0.035.
        const double randomness = std::clamp(s_.rainDropLengthRandomness, 0.0, 2.0);
        for (const Drop &d : rain_) {
            QColor col = s_.rainColor;
            col.setAlpha(int(190 * d.alpha));
            QPen pen(col, std::max(1, int(std::lround(d.size))));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            const double variance = 1.0 - randomness * 0.5 +
                std::abs(std::sin(d.seed)) * randomness;
            const double length = std::max(1.0, s_.rainLength * variance);
            const double slant = d.vx * 0.035;
            p.drawLine(QPointF(d.x, d.y), QPointF(d.x - slant, d.y - length));
        }
    }
    if (!ripples_.isEmpty()) {
        // Python _draw_ripples: circles, easeOutCubic radius, fading pen.
        for (const Ripple &rp : ripples_) {
            const double duration = 1.1 / std::max(0.05, rp.speed);
            const double tt = std::clamp(rp.age / duration, 0.0, 1.0);
            const double rad = rp.maxR * easeOutCubic(tt);
            QColor col = rp.color; col.setAlpha(int(210 * (1.0 - tt)));
            QPen pen(col, std::max(1.0, 3 * (1.0 - tt) + 1));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(rp.x, rp.y), rad, rad);
        }
    }
    drawIconScene(p, r); // desktop icons integrated into the scene (v0.9.0)
    if (editMode()) {
        p.setPen(QPen(accent(), 1.5, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(1, 1, -2, -2), 10, 10);
        if (!s_.anyEnabled()) {
            p.setPen(QColor(200, 208, 216));
            p.drawText(r.adjusted(12, 12, -12, -12), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                       tr("エフェクト (effects_overlay)\n"
                          "effects_json に rain_enabled / snow_enabled 等を\n"
                          "入れたウィジェットです。現在は何も有効になっていません。"));
        }
    }
}

// ---------- Desktop icon scene integration (v0.9.0) ----------
// Port of Python's _lds_paint_desktop_icon_scene_overlay (strengths are
// the env defaults: glow .34 / fog .20 / shadow .28 / reflection .16 /
// selection .42, radius 74, boost hover +0.45 / selected +0.55), plus the
// icon images and labels themselves: the C++ overlay window sits above
// the desktop, so without this layer the real icons would be painted
// over by the (mostly opaque) scenes. Input is forwarded to the real
// ListView by the mouse handlers below, keeping Explorer in charge.
void EffectsWidget::drawIconScene(QPainter &p, const QRect &r) {
    if (!iconSceneActive() || editMode()) return;
    const QPoint origin = mapToGlobal(QPoint(0, 0));
    const QVector<DesktopIcons::IconItem> items = DesktopIcons::readItems(origin, r.size());
    if (items.isEmpty()) return;
    const QPoint mouse = mapFromGlobal(QCursor::pos());
    const double kGlow = 0.34, kFog = 0.20, kShadow = 0.28, kReflection = 0.16, kSel = 0.42;
    const double radius = 74.0;
    const int iconSize = 48;
    QFont labelFont(QStringLiteral("Segoe UI"), 9);
    for (const DesktopIcons::IconItem &it : items) {
        const QRectF b = it.bounds;
        const double cx = b.center().x();
        const double bottom = b.bottom();
        const bool hovered = b.adjusted(-8, -8, 8, 8).contains(mouse);
        const double boost = 1.0 + (hovered ? 0.45 : 0.0) + (it.selected ? 0.55 : 0.0);
        p.setPen(Qt::NoPen);
        if (kShadow > 0) { // grounding shadow under the item
            QRadialGradient g(QPointF(cx, bottom + 10), radius * 0.78);
            g.setColorAt(0.0, QColor(0, 0, 0, int(42 * kShadow * boost)));
            g.setColorAt(0.65, QColor(0, 0, 0, int(14 * kShadow * boost)));
            g.setColorAt(1.0, QColor(0, 0, 0, 0));
            p.setBrush(g);
            p.drawEllipse(QRectF(cx - radius * 0.75, bottom - 16, radius * 1.5, radius * 0.55));
        }
        if (kReflection > 0) { // pale reflection streak below (mirror scenes)
            QLinearGradient rg(QPointF(cx, bottom + 2), QPointF(cx, bottom + 54));
            rg.setColorAt(0.0, QColor(205, 232, 255, int(34 * kReflection * boost)));
            rg.setColorAt(1.0, QColor(205, 232, 255, 0));
            p.setBrush(rg);
            p.drawEllipse(QRectF(b.left() + 4, bottom + 4, std::max(24.0, b.width() - 8), 34));
        }
        if (kGlow > 0) {
            QRadialGradient gg(QPointF(cx, b.center().y()), radius);
            gg.setColorAt(0.0, QColor(190, 225, 255, int(30 * kGlow * boost)));
            gg.setColorAt(0.48, QColor(140, 205, 255, int(16 * kGlow * boost)));
            gg.setColorAt(1.0, QColor(140, 205, 255, 0));
            p.setBrush(gg);
            p.drawEllipse(QRectF(cx - radius, b.center().y() - radius, radius * 2, radius * 2));
        }
        if (kFog > 0) {
            const double cy = b.center().y();
            QRadialGradient fg(QPointF(cx + radius * 0.08, cy - radius * 0.05), radius * 1.25);
            fg.setColorAt(0.0, QColor(235, 246, 255, int(24 * kFog * boost)));
            fg.setColorAt(0.58, QColor(225, 240, 255, int(11 * kFog * boost)));
            fg.setColorAt(1.0, QColor(225, 240, 255, 0));
            p.setBrush(fg);
            p.drawEllipse(QRectF(cx - radius * 1.18, cy - radius * 1.02, radius * 2.36, radius * 2.04));
        }
        // The icon image itself (shell icon resolved by label, cached).
        const QPixmap icon = DesktopIcons::iconImageFor(it);
        const double ix = cx - iconSize / 2.0, iy = b.top() + 4;
        if (!icon.isNull())
            p.drawPixmap(QRectF(ix, iy, iconSize, iconSize), icon, QRectF(icon.rect()));
        // Label, Python's _lds_make_icon_label_image recipe (v0.9.5):
        // character-wrapped to at most 2 lines (max width 128, ellipsis),
        // a faint dark backing (alpha 24, selected 58, radius 8), a dark
        // text glow in 4 directions (alpha 38, selected 86) and white
        // text (alpha 224, selected 238). The v0.9.0-0.9.4 version drew
        // bare white text with a 1px shadow — invisible on the bright
        // Pamukkale terraces, which is why most labels "weren't there".
        if (!it.text.isEmpty()) {
            const QFontMetrics fm(labelFont);
            const int maxTextW = 128 - 14;
            QStringList lines;
            QString rest = it.text;
            for (int li = 0; li < 2 && !rest.isEmpty(); ++li) {
                if (fm.horizontalAdvance(rest) <= maxTextW) { lines.append(rest); rest.clear(); break; }
                int cut = rest.size();
                while (cut > 1 && fm.horizontalAdvance(rest.left(cut) + QStringLiteral("…")) > maxTextW) --cut;
                lines.append(rest.left(cut) + QStringLiteral("…"));
                rest = rest.mid(cut);
            }
            if (!lines.isEmpty()) {
                int textW = 0;
                for (const QString &ln : lines) textW = std::max(textW, fm.horizontalAdvance(ln));
                const double bw = std::min(128.0, textW + 14.0);
                const double bh = lines.size() * (fm.height() + 1) + 8;
                QRectF bg(cx - bw / 2.0, iy + iconSize + 2, bw, bh);
                if (bg.bottom() > bottom + 6) bg.moveTop(bottom + 6 - bh);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(8, 14, 24, it.selected ? 58 : 24));
                p.drawRoundedRect(bg, 8, 8);
                const QRectF textRect(bg.left() + 7, bg.top() + 4, bw - 14, bh - 8);
                const QString joined = lines.join(QStringLiteral("\n"));
                p.setFont(labelFont);
                p.setPen(QColor(8, 14, 24, it.selected ? 86 : 38));
                for (const QPointF off : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)})
                    p.drawText(textRect.translated(off), Qt::AlignHCenter | Qt::AlignTop, joined);
                p.setPen(QColor(255, 255, 255, it.selected ? 238 : 224));
                p.drawText(textRect, Qt::AlignHCenter | Qt::AlignTop, joined);
            }
        }
        if (it.selected || it.focused) { // Python's selection frame
            const QRectF sr(b.left() - 5, b.top() - 4, std::max(24.0, b.width() + 10),
                            std::max(24.0, b.height() + 8));
            p.setPen(QPen(QColor(150, 210, 255, int(90 * kSel)), 1.2));
            p.setBrush(QColor(70, 150, 255, int(30 * kSel)));
            p.drawRoundedRect(sr, 10, 10);
        }
    }
}

// Input proxy: while a scene is up, the overlay receives the mouse
// instead of the desktop. Forward the button traffic to the real
// ListView (converted to its client coordinates inside postMouse), so
// clicks select, double-clicks open, and drags move the REAL icons;
// our painted copies follow through the invalidated read cache.
void EffectsWidget::mousePressEvent(QMouseEvent *e) {
    if (iconSceneActive() && !editMode() && e->button() == Qt::LeftButton) {
        const QPoint origin = mapToGlobal(QPoint(0, 0));
        if (!DesktopIcons::readItems(origin, size()).isEmpty()) {
            iconPressActive_ = true;
            iconPressPos_ = e->pos();
            DesktopIcons::postMouse(WM_LBUTTONDOWN, e->pos(), origin, MK_LBUTTON);
            update();
            e->accept();
            return;
        }
    }
    WidgetWindow::mousePressEvent(e);
}

void EffectsWidget::mouseMoveEvent(QMouseEvent *e) {
    if (iconPressActive_) {
        const QPoint origin = mapToGlobal(QPoint(0, 0));
        DesktopIcons::postMouse(WM_MOUSEMOVE, e->pos(), origin, MK_LBUTTON);
        e->accept();
        return;
    }
    WidgetWindow::mouseMoveEvent(e);
}

void EffectsWidget::mouseReleaseEvent(QMouseEvent *e) {
    if (iconPressActive_ && e->button() == Qt::LeftButton) {
        iconPressActive_ = false;
        const QPoint origin = mapToGlobal(QPoint(0, 0));
        DesktopIcons::postMouse(WM_LBUTTONUP, e->pos(), origin, 0);
        update();
        e->accept();
        return;
    }
    WidgetWindow::mouseReleaseEvent(e);
}

void EffectsWidget::mouseDoubleClickEvent(QMouseEvent *e) {
    if (iconSceneActive() && !editMode() && e->button() == Qt::LeftButton) {
        const QPoint origin = mapToGlobal(QPoint(0, 0));
        if (!DesktopIcons::readItems(origin, size()).isEmpty()) {
            DesktopIcons::postMouse(WM_LBUTTONDBLCLK, e->pos(), origin, MK_LBUTTON);
            update();
            e->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(e);
}

void EffectsWidget::resizeEvent(QResizeEvent *) {
    reseed();
}
