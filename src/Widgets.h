#pragma once
#include "WidgetWindow.h"
#include "SystemMonitor.h"
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTextDocument>
#include <QTimer>
#include <deque>

class AudioEngine;

class ClockWidget : public WidgetWindow {
public:
    explicit ClockWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {}
    void onTick(const SystemSnapshot &, const QDateTime &now) override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
private:
    QDateTime last_;
};

class SystemWidget : public WidgetWindow {
public:
    explicit SystemWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {}
    void onTick(const SystemSnapshot &s, const QDateTime &) override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
private:
    int cpu_ = -1, mem_ = -1, disk_ = -1;
};

class NetworkWidget : public WidgetWindow {
public:
    explicit NetworkWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {}
    void onTick(const SystemSnapshot &s, const QDateTime &now) override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
private:
    quint64 prevDown_ = 0, prevUp_ = 0;
    quint64 totalDown_ = 0, totalUp_ = 0;
    qint64 prevMs_ = 0;
    double downBps_ = 0, upBps_ = 0;
    std::deque<double> histDown_, histUp_;
};

class CalendarWidget : public WidgetWindow {
public:
    explicit CalendarWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {}
    void onTick(const SystemSnapshot &, const QDateTime &now) override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
private:
    QDate date_;
};

class TextWidget : public WidgetWindow {
public:
    explicit TextWidget(WidgetConfig cfg);
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
    void resizeEvent(QResizeEvent *) override;
private:
    QTextDocument doc_;
};

class VisualizerWidget : public WidgetWindow {
    Q_OBJECT
public:
    VisualizerWidget(WidgetConfig cfg, AudioEngine *audio);
    ~VisualizerWidget() override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
    void addContextActions(QMenu &menu) override;
private:
    void applyFrameRate();
    void syncArtworkNeed();
    void paintClassic(QPainter &p, const QRect &area);
    void paintBarScene(QPainter &p, const QRect &area, const QString &style);
    void paintCircularScene(QPainter &p, const QRect &area, const QString &style);
    // Radial capsule, the Python build's _cap() primitive.
    void capsule(QPainter &p, const QPointF &c, double ang, double r0, double len, double w, const QColor &col) const;
    // Album cover disc (ellipse rx/ry around c); falls back to a tinted
    // disc with a note glyph when the media helper has no artwork.
    void drawCover(QPainter &p, const QPointF &c, double rx, double ry, double spinDeg = 0.0) const;
    static QColor rainbowColor(double phase, int alpha);
    AudioEngine *audio_;
    QTimer timer_;
    QElapsedTimer clock_;
    QVector<float> bars_, peaks_;
    qint64 lastTickMs_ = 0;
    double now_ = 0.0, avg_ = 0.0, bass_ = 0.0;
    bool artworkHeld_ = false;
    // rainbow powder particles
    struct Powder { double birth, x, y, tone, seed; int spark; };
    QVector<Powder> powder_;
    QVector<float> rainbowPrevTone_;
    QVector<double> rainbowLastSpawn_;
    // flat_spectrum / electro_dubstep temporal state
    QVector<float> flatSmooth_;
    double flatGate_ = 0.0, flatPrevEnergy_ = 0.0;
    QVector<float> electroSmooth_;
    double electroBurst_ = 0.0, electroBurstVel_ = 0.0, electroPrevEnergy_ = 0.0;
};

class VolumeWidget : public WidgetWindow {
public:
    explicit VolumeWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {}
    void onTick(const SystemSnapshot &, const QDateTime &) override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
    void onClicked(const QPoint &) override;
    void onWheel(int delta) override;
private:
    int volume_ = -1;
    bool mute_ = false;
};

class MediaWidget : public WidgetWindow {
public:
    explicit MediaWidget(WidgetConfig cfg);
    ~MediaWidget() override;
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
    void onClicked(const QPoint &pos) override;
    bool onPressed(const QPoint &pos) override;
private:
    bool triggerAt(const QPoint &pos);
    QRect btnPrev_, btnPlay_, btnNext_, btnStop_;
    int pressedBtn_ = -1; // for the press flash feedback
    QTimer flashTimer_;
};

class WeatherEngine;
class WeatherWidget : public WidgetWindow {
    Q_OBJECT
public:
    explicit WeatherWidget(WidgetConfig cfg);
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
private:
    WeatherEngine *engine_;
};

class PlaceholderWidget : public WidgetWindow {
public:
    explicit PlaceholderWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {}
protected:
    void paintContent(QPainter &p, const QRect &rect) override;
};

// Factory mirroring create_widget() in the Python build.
// Unknown types (html_js, …) become placeholders so the user can see and
// delete them, instead of crashing. effects_overlay is a real widget now
// (EffectsOverlay.h, v0.5 part 1).
WidgetWindow *createWidget(const WidgetConfig &cfg, AudioEngine *audio);
