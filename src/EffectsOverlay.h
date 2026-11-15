#pragma once
#include "WidgetWindow.h"
#include <QColor>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QRandomGenerator>
#include <QTimer>
#include <QVector>

// Effects overlay widget (type "effects_overlay").
//
// v0.5 part 1 — ports the kept effect systems: rain, snow (+crystals), moon, cloud,
// water surface and ice surface. QPainter only, no OpenGL, 24fps timer,
// and the widget (and its timer) only exist when an effects_overlay widget
// is present in the config — otherwise the cost is zero.
//
// Settings come from the widget's `effects_json` field, the same JSON
// string Python stores on the widget (litedesktopstudio/effects.py,
// LIGHTWEIGHT_ROSE_PETAL_DEFAULT_SETTINGS). Keys this build does not
// understand are ignored here but preserved verbatim by WidgetConfig::extra,
// so the config still round-trips to the Python build.
//
// v0.8.0 adds the four scenic engines (uyuni / chichibugahama / pamukkale /
// sahara) with the realtime time-of-day sky, cloud mirror reflections and
// the atmosphere layer . Still NOT ported: water
// fish/spray/drops, snow accumulation, morning fog, desktop-icon scene
// integration (separate phase).
struct EffectSettings {
    // rain
    bool rainEnabled = false;
    int rainCount = 90;
    double rainSpeed = 1.0;
    QColor rainColor = QColor("#9FD7FF");
    double rainLength = 16.0;
    double rainDropMinSize = 1.0;
    double rainDropMaxSize = 2.4;
    bool rainRippleEnabled = false;
    double rainRippleChance = 0.55;
    double rainRippleSurfaceY = 0.82;
    double rainRippleMinRadius = 24.0;
    double rainRippleMaxRadius = 92.0;
    double rainDropLengthRandomness = 0.55;
    bool rippleEnabled = false; // master ripple gate (Python ripple_enabled)
    QColor rippleColor = QColor("#A8EFFF");
    double rippleSpeed = 1.0;
    // snow
    bool snowEnabled = false;
    int snowCount = 90;
    double snowSpeed = 0.18;
    double snowSize = 4.5;
    int snowAlpha = 210;
    QColor snowColor = QColor("#F5FCFF");
    QColor snowEdgeColor = QColor("#CFEFFF");
    bool snowCrystalEnabled = false;
    int snowCrystalCount = 22;
    double snowCrystalSpeed = 0.12;
    double snowCrystalSize = 15.0;
    int snowCrystalAlpha = 220;
    QColor snowCrystalColor = QColor("#EBFAFF");
    QColor snowCrystalEdgeColor = QColor("#D8F4FF");
    double snowSurfaceY = 0.86;
    // cloud
    bool cloudEnabled = false;
    int cloudCount = 13;
    double cloudSpeed = 1.05;
    double cloudSize = 120.0;
    int cloudAlpha = 200;
    QColor cloudColor = QColor("#F4FAFF");
    QColor cloudShadowColor = QColor("#B8C7D8");
    QColor cloudHighlightColor = QColor("#FFFFFF");
    double cloudAltitude = 0.22;
    double cloudDepth = 0.42;
    // moon
    bool moonBodyEnabled = false;
    bool moonlightEnabled = false;
    bool moonShadowEnabled = false;
    double moonX = 0.78;
    double moonY = 0.18;
    double moonRadius = 74.0;
    int moonAlpha = 230;
    QColor moonColor = QColor("#FFF3C4");
    QColor moonEdgeColor = QColor("#C9D7FF");
    int moonCraterCount = 9;
    int moonCraterAlpha = 54;
    double moonlightRadius = 260.0;
    int moonlightAlpha = 82;
    QColor moonlightColor = QColor("#CFE8FF");
    bool moonlightBeamEnabled = false;
    int moonlightBeamAlpha = 44;
    double moonlightBeamWidth = 0.34;
    QColor moonShadowColor = QColor("#061028");
    int moonShadowAlpha = 70;
    double moonShadowOffsetX = 28.0;
    double moonShadowOffsetY = 38.0;
    double moonShadowBlurRadius = 150.0;
    // water surface
    bool waterSurfaceEnabled = false;
    int waterSurfaceAlpha = 92;
    QColor waterSurfaceColor = QColor("#4FC3FF");
    QColor waterSurfaceHighlightColor = QColor("#D8FAFF");
    double waterSurfaceFlowSpeed = 0.55;
    int waterSurfaceWaveCount = 14;
    double waterSurfaceWaveHeight = 12.0;
    double waterSurfaceY = 0.58;
    double waterSurfaceDepth = 0.42;
    double waterSurfaceFlowAngle = 0.0;
    // ice surface
    bool iceEnabled = false;
    int iceAlpha = 178;
    QColor iceColor = QColor("#9BDDF2");
    QColor iceEdgeColor = QColor("#E8FBFF");
    QColor iceHighlightColor = QColor("#F7FFFF");
    double iceY = 0.58;
    double iceCrackIntensity = 0.46;
    double iceBubbleIntensity = 0.36;
    bool iceFogEnabled = false;
    int iceFogAlpha = 72;
    double iceFogHeight = 0.24;
    QColor iceFogColor = QColor("#EEF9FF");
    // scenic engines (v0.8.0)
    bool iconSceneEnabled = true; // desktop icon integration layer (v0.9.0)
    bool frameRateEnabled = true; // effect_frame_rate_enabled (editor, v0.10.0)
    int frameRate = 24;           // effect_frame_rate; 24 = the pre-editor fixed rate
    bool uyuniEnabled = false;
    bool chichibugahamaEnabled = false;
    bool pamukkaleEnabled = false;
    bool saharaEnabled = false;
    // realtime scenic sky: explicit key wins, otherwise auto-on when a
    // sky scene (uyuni / chichibugahama / sahara) is enabled.
    bool scenicSkyEnabled = false;
    bool sunRaysEnabled = false;   // realtime_scenic_sun_rays_enabled
    bool atmosphereEnabled = true; // scenic_atmosphere_enhancement_enabled
    double waterMirrorWave = 7.0;  // water_mirror_wave (reflection wobble)

    bool anyEnabled() const {
        return rainEnabled || snowEnabled || snowCrystalEnabled || cloudEnabled ||
               moonBodyEnabled || moonlightEnabled || moonShadowEnabled ||
               waterSurfaceEnabled || iceEnabled ||
               uyuniEnabled || chichibugahamaEnabled || pamukkaleEnabled || saharaEnabled;
    }
    static EffectSettings fromJson(const QJsonObject &o);
};

class EffectsWidget : public WidgetWindow {
    Q_OBJECT
public:
    explicit EffectsWidget(WidgetConfig cfg);
    void onEditModeChanged() override { update(); }

    // Effects editor plumbing (v0.10.0): the editor dialog works on the
    // raw effects_json object so keys the C++ build doesn't know (from
    // the Python era) survive a round-trip untouched.
    QJsonObject effectsJson() const;
    void setEffectsJson(const QJsonObject &obj); // store + reload + save
    void reloadEffects();                        // re-read, re-seed, drop caches
    void openEffectsEditor();
    // Light-weight single-key update for the 3D preview's marker drags:
    // patches effects_json and re-reads settings WITHOUT dropping the
    // scene caches or re-seeding particles (marker keys don't affect
    // either). commitSave=false during a drag, true on release.
    void setEffectsValue(const QString &key, const QJsonValue &value, bool commitSave);

signals:
    void requestOpenPreview3D();

public:
    // Desktop embedding (v0.8.2, Windows): while a scenic engine is on, the
    // overlay is hosted by the desktop WorkerW layer — docked under the
    // desktop icons and all widgets (see DesktopHost in main.cpp).
    bool wantsDesktopHost() const {
        return s_.uyuniEnabled || s_.chichibugahamaEnabled || s_.pamukkaleEnabled || s_.saharaEnabled;
    }

protected:
    void addContextActions(class QMenu &menu) override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    // WidgetWindow's pure virtual; the overlay paints in paintEvent instead
    // (no panel), so this is intentionally a no-op.
    void paintContent(class QPainter &, const QRect &) override {}

private:
    struct Drop { double x, y, vx, vy, size, alpha, seed; };
    struct Flake { double x, y, vx, vy, size, alphaF, seed, rotation, rotSpeed; bool crystal; };
    struct Cloud { double x, y, vx, scale, alphaF; QPixmap pix; };
    struct Ripple { double x, y, age, maxR, speed; QColor color; };
    // Scenic engines (v0.8.0). Scene kinds: 1 uyuni, 2 chichibugahama,
    // 3 pamukkale, 4 sahara. The sky function takes 0=desert, 1 uyuni,
    // 2 chichibugahama (matching Python's scene strings).
    struct Star { double x, y; int base; double twSpeed, twPhase; bool cross; };
    struct Weed { double x, y, baseY, size, speed, rot, spin, phase; QPixmap sprite; };

    void loadSettings();
    void applyFrameRate();
    QPointer<class EffectsEditorDialog> editor_;
    void reseed();
    void step(double dt);
    Drop makeDrop(QRandomGenerator &rng) const;
    QPixmap makeCloudPixmap(double scale) const;
    void drawMoon(QPainter &p, const QRect &r, double t);
    void drawWater(QPainter &p, const QRect &r, double t);
    void drawIce(QPainter &p, const QRect &r, double t);
    // scenes
    void drawScenes(QPainter &p, const QRect &r);
    QPixmap sceneStatic(int kind, int w, int h);
    QPixmap renderUyuniStatic(int w, int h);
    QPixmap renderChichibugahamaStatic(int w, int h);
    QPixmap renderPamukkaleStatic(int w, int h);
    QPixmap renderSaharaStatic(int w, int h);
    QPixmap mistPixmap(int kind, int w, int h);
    void drawRealtimeSky(QPainter &p, const QRect &r, int skyScene);
    void drawSunGlow(QPainter &p, double x, double y, double radius, double strength,
                     bool hot, bool cool, double rayCenter, double raySpread, double rayAlphaScale);
    void drawStars(QPainter &p, const QRect &r, double horizonRatio, double alphaScale);
    void drawShootingStar(QPainter &p, const QRect &r, double horizonRatio);
    void drawCloudReflection(QPainter &p, const QRect &r, int sceneKind);
    void drawChichibugahamaAccents(QPainter &p, const QRect &r, int phase, double t);
    void drawAtmosphere(QPainter &p, const QRect &r, int sceneKind);
    void drawPamukkaleDynamic(QPainter &p, const QRect &r);
    void drawChichibugahamaDynamic(QPainter &p, const QRect &r);
    void drawTumbleweeds(QPainter &p, const QRect &r);
    // desktop icon integration (v0.9.0): paints the Explorer icons (the
    // overlay covers the real ones) with Python's scene treatment, and
    // hit-tests them for the input proxy.
    void drawIconScene(QPainter &p, const QRect &r);
    bool iconSceneActive() const {
        return s_.iconSceneEnabled &&
               (s_.uyuniEnabled || s_.chichibugahamaEnabled || s_.pamukkaleEnabled || s_.saharaEnabled);
    }

    EffectSettings s_;
    QTimer timer_;
    QElapsedTimer clock_;
    double time_ = 0.0;
    QVector<Drop> rain_;
    QVector<Flake> snow_;
    QVector<Cloud> clouds_;
    QVector<Ripple> ripples_;
    QVector<QPointF> iceCracks_;
    QVector<QPointF> iceBubbles_;
    QPointF moonCenter_;
    double moonRadius_ = 0;
    double lastRippleAt_ = -1.0; // rain ripple global cooldown (0.025s)
    // scene caches (regenerated on size change only)
    QMap<int, QPixmap> sceneStaticCache_;
    QMap<int, QPixmap> mistCache_;
    QSize sceneCacheSize_;
    QVector<Star> stars_;
    QVector<Weed> weeds_;
    // dithered glow caches (anti-banding, v0.8.1)
    QPixmap nightHaloCache_;
    QPixmap moonGlowCache_;
    int moonGlowRadius_ = 0;
    // icon input proxy state (v0.9.0)
    bool iconPressActive_ = false;
    QPoint iconPressPos_;
};
