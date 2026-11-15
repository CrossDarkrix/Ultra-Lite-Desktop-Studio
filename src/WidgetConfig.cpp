#include "WidgetConfig.h"
#include <QJsonValue>
#include <algorithm>
#include <cmath>

static QString strOr(const QJsonObject &o, const char *k, const QString &def) {
    const auto v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : def;
}
static int intOr(const QJsonObject &o, const char *k, int def) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toInt() : def;
}
static double dblOr(const QJsonObject &o, const char *k, double def) {
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : def;
}
static bool boolOr(const QJsonObject &o, const char *k, bool def) {
    const auto v = o.value(QLatin1String(k));
    return v.isBool() ? v.toBool() : def;
}

WidgetConfig WidgetConfig::fromJson(const QJsonObject &obj) {
    WidgetConfig c;
    c.type = strOr(obj, "type", c.type);
    c.x = intOr(obj, "x", c.x);
    c.y = intOr(obj, "y", c.y);
    c.w = intOr(obj, "w", c.w);
    c.h = intOr(obj, "h", c.h);
    c.rotationDegrees = dblOr(obj, "rotation_degrees", dblOr(obj, "rotation", 0.0));
    c.title = strOr(obj, "title", c.title);
    c.color = strOr(obj, "color", c.color);
    c.bg = strOr(obj, "bg", c.bg);
    c.text = strOr(obj, "text", c.text);
    c.fontSize = intOr(obj, "font_size", c.fontSize);
    c.bgAlpha = intOr(obj, "bg_alpha", c.bgAlpha);
    c.cpuColor = strOr(obj, "cpu_color", c.cpuColor);
    c.memoryColor = strOr(obj, "memory_color", c.memoryColor);
    c.diskColor = strOr(obj, "disk_color", c.diskColor);
    c.clockShowDigital = boolOr(obj, "clock_show_digital", c.clockShowDigital);
    c.weatherLocation = strOr(obj, "weather_location", c.weatherLocation);
    c.networkDownColor = strOr(obj, "network_down_color", c.networkDownColor);
    c.networkUpColor = strOr(obj, "network_up_color", c.networkUpColor);

    static const char *known[] = {
        "type","x","y","w","h","rotation_degrees","rotation","title","color","bg",
        "text","font_size","bg_alpha","cpu_color","memory_color","disk_color",
        "clock_show_digital","weather_location","network_down_color","network_up_color",
    };
    c.extra = obj;
    for (const char *k : known) c.extra.remove(QLatin1String(k));
    // v0.13.1 migration: v0.13.0 stored a whole-window "opacity" in
    // extra. Transparency is background-only now, so fold that factor
    // into bg_alpha (the background's effective alpha was
    // bg_alpha * opacity) and drop the key — otherwise the value
    // would round-trip forever with no effect.
    if (c.extra.contains(QStringLiteral("opacity"))) {
        const double op = c.extra.value(QStringLiteral("opacity")).toDouble(1.0);
        c.bgAlpha = int(std::lround(std::clamp(c.bgAlpha * std::clamp(op, 0.0, 1.0), 0.0, 255.0)));
        c.extra.remove(QStringLiteral("opacity"));
    }
    return c;
}

QString WidgetConfig::vizStyle() const {
    return strOr(extra, "visualizer_style", QStringLiteral("classic")).toLower();
}
void WidgetConfig::setVizStyle(const QString &s) { extra["visualizer_style"] = s; }
bool WidgetConfig::vizBool(const char *key, bool def) const { return boolOr(extra, key, def); }
void WidgetConfig::setVizBool(const char *key, bool v) { extra[QLatin1String(key)] = v; }
double WidgetConfig::vizDouble(const char *key, double def) const { return dblOr(extra, key, def); }
void WidgetConfig::setVizDouble(const char *key, double v) { extra[QLatin1String(key)] = v; }
int WidgetConfig::vizInt(const char *key, int def) const { return intOr(extra, key, def); }

QJsonObject WidgetConfig::toJson() const {
    QJsonObject o = extra; // preserve unknown keys first
    o["type"] = type;
    o["x"] = x; o["y"] = y; o["w"] = w; o["h"] = h;
    o["rotation_degrees"] = rotationDegrees;
    o["rotation"] = rotationDegrees;
    o["title"] = title;
    o["color"] = color;
    o["bg"] = bg;
    o["text"] = text;
    o["font_size"] = fontSize;
    o["bg_alpha"] = bgAlpha;
    o["cpu_color"] = cpuColor;
    o["memory_color"] = memoryColor;
    o["disk_color"] = diskColor;
    o["clock_show_digital"] = clockShowDigital;
    o["weather_location"] = weatherLocation;
    o["network_down_color"] = networkDownColor;
    o["network_up_color"] = networkUpColor;
    return o;
}
