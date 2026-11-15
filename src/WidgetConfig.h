#pragma once
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <cmath>

// Compatible with the Python version's WidgetConfig dataclass
// (litedesktopstudio/widgets.py) and ~/LiteDesktopStudio_config.json.
// Keys this C++ build does not understand are kept in `extra` and written
// back unchanged, so the same config file can round-trip to the Python build.
struct WidgetConfig {
    QString type = QStringLiteral("system");
    int x = 100;
    int y = 100;
    int w = 300;
    int h = 120;
    double rotationDegrees = 0.0;
    QString title = QStringLiteral("Widget");
    QString color = QStringLiteral("#5BE7FF");
    QString bg = QStringLiteral("#141820");
    // Background-only transparency (v0.13.1, the user's refinement of
    // v0.13.0): the right-click 透明度 controls bgAlpha — the alpha of
    // the background color (bg), stored as the Python-compatible
    // `bg_alpha` key (0-255) — and nothing else. Text, icons, bars and
    // artwork stay fully opaque, and the native window itself is
    // always fully opaque (no setWindowOpacity). The percent helpers
    // below are the UI's view of bgAlpha. A legacy v0.13.0 "opacity"
    // key, if present, is folded into bgAlpha on load (see
    // WidgetConfig.cpp) so a config saved by that one build keeps
    // roughly the background strength it had.
    int bgAlphaPercent() const {
        return int(std::lround(std::clamp(bgAlpha, 0, 255) / 255.0 * 100.0));
    }
    void setBgAlphaPercent(int pct) {
        bgAlpha = int(std::lround(std::clamp(pct, 0, 100) / 100.0 * 255.0));
    }
    QString text;
    int fontSize = 14;
    int bgAlpha = 155;
    QString cpuColor = QStringLiteral("#5BE7FF");
    QString memoryColor = QStringLiteral("#B388FF");
    QString diskColor = QStringLiteral("#80FF9F");
    bool clockShowDigital = true;
    QString weatherLocation;
    QString networkDownColor = QStringLiteral("#5BE7FF");
    QString networkUpColor = QStringLiteral("#80FF9F");

    QJsonObject extra; // unknown keys, preserved verbatim

    static WidgetConfig fromJson(const QJsonObject &obj);
    QJsonObject toJson() const;

    // Visualizer settings live in `extra` under the Python build's exact
    // keys (visualizer_style, visualizer_flip_vertical, ...), so they
    // round-trip even for styles this build cannot render yet.
    QString vizStyle() const;
    void setVizStyle(const QString &s);
    bool vizBool(const char *key, bool def) const;
    void setVizBool(const char *key, bool v);
    double vizDouble(const char *key, double def) const;
    void setVizDouble(const char *key, double v);
    int vizInt(const char *key, int def) const;
};
