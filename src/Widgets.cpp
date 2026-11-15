#include "Widgets.h"
#include "AudioEngine.h"
#include "EffectsOverlay.h"
#include "JsHtmlWidget.h"
#include "MediaHelperClient.h"
#include "Platform.h"
#include "WeatherEngine.h"
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTextStream>
#include <algorithm>
#include <cmath>

static void contentFont(QPainter &p, int size, bool bold = false) {
    QFont f = p.font();
    f.setPointSize(std::max(8, size));
    f.setBold(bold);
    p.setFont(f);
}

// ---------- Clock ----------
void ClockWidget::onTick(const SystemSnapshot &, const QDateTime &now) {
    if (now.time().second() != last_.time().second() || now.date() != last_.date()) {
        last_ = now;
        update();
    }
}
void ClockWidget::paintContent(QPainter &p, const QRect &rect) {
    const QTime t = last_.isValid() ? last_.time() : QTime::currentTime();
    const int side = std::min(rect.width(), rect.height()) - 16;
    const QPointF c(rect.center().x(), rect.center().y() - (cfg_.clockShowDigital ? 8 : 0));
    const double r = side / 2.0;
    p.setRenderHint(QPainter::Antialiasing, true);
    // Dial face (v0.13.0, the user's refinement of the v0.7.2 "fixed
    // face" decision): the face follows the widget's background color
    // setting (right-click 背景色を変更) and its default is #1a1a1a.
    // Configs still carrying the generic default bg (#141820, i.e.
    // never customized) count as unset. The face stays OPAQUE:
    // v0.13.1 briefly let the background-only transparency (bgAlpha)
    // apply to it too, but the user asked for the dial to be excluded
    // (v0.13.2) — only the panel behind it follows bgAlpha.
    QColor face(cfg_.bg);
    if (face == QColor(QStringLiteral("#141820")))
        face = QColor(QStringLiteral("#1a1a1a"));
    p.setPen(Qt::NoPen);
    p.setBrush(face);
    p.drawEllipse(c, r, r);
    p.setPen(QPen(accent(), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r, r);
    for (int i = 0; i < 12; ++i) {
        const double a = i * 30.0 * M_PI / 180.0;
        const QPointF o(c.x() + std::sin(a) * (r - 4), c.y() - std::cos(a) * (r - 4));
        const QPointF in(c.x() + std::sin(a) * (r - (i % 3 == 0 ? 12 : 8)), c.y() - std::cos(a) * (r - (i % 3 == 0 ? 12 : 8)));
        p.setPen(QPen(QColor(230, 235, 240), i % 3 == 0 ? 2 : 1));
        p.drawLine(in, o);
    }
    if (side >= 110) { // hour numerals, like the Python build (skip when tiny)
        QFont nf = p.font();
        nf.setPointSize(std::max(8, int(side / 13)));
        nf.setBold(true);
        p.setFont(nf);
        p.setPen(QColor(240, 244, 248));
        for (int i = 0; i < 12; ++i) {
            const double a = i * 30.0 * M_PI / 180.0;
            const double nr = r * 0.76;
            const QPointF pos(c.x() + std::sin(a) * nr, c.y() - std::cos(a) * nr);
            const QRectF tr(pos.x() - 14, pos.y() - 10, 28, 20);
            p.drawText(tr, Qt::AlignCenter, QString::number(i == 0 ? 12 : i));
        }
    }
    auto hand = [&](double angleDeg, double len, double width, const QColor &col) {
        const double a = angleDeg * M_PI / 180.0;
        p.setPen(QPen(col, width, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, QPointF(c.x() + std::sin(a) * len, c.y() - std::cos(a) * len));
    };
    hand((t.hour() % 12 + t.minute() / 60.0) * 30.0, r * 0.5, 4, QColor(240, 244, 248));
    hand((t.minute() + t.second() / 60.0) * 6.0, r * 0.75, 3, QColor(200, 210, 220));
    hand(t.second() * 6.0, r * 0.85, 1.5, accent());
    if (cfg_.clockShowDigital) {
        contentFont(p, cfg_.fontSize, true);
        p.setPen(QColor(240, 244, 248));
        const QRect tr(rect.left(), int(c.y() + r + 2), rect.width(), 24);
        p.drawText(tr, Qt::AlignHCenter | Qt::AlignTop,
                   t.toString(QStringLiteral("HH:mm:ss")));
    }
}

// ---------- System ----------
void SystemWidget::onTick(const SystemSnapshot &s, const QDateTime &) {
    const int c = s.cpuPercent < 0 ? -1 : int(s.cpuPercent + 0.5);
    const int m = int(s.memPercent + 0.5), d = int(s.diskPercent + 0.5);
    if (c != cpu_ || m != mem_ || d != disk_) { cpu_ = c; mem_ = m; disk_ = d; update(); }
}
void SystemWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    struct Row { QString label; int pct; QColor col; };
    const Row rows[] = {
        {QStringLiteral("CPU"), cpu_, QColor(cfg_.cpuColor)},
        {QStringLiteral("MEM"), mem_, QColor(cfg_.memoryColor)},
        {QStringLiteral("DISK"), disk_, QColor(cfg_.diskColor)},
    };
    const int top = rect.top() + (cfg_.title.isEmpty() ? 10 : 34);
    const int rowH = std::max(18, (rect.height() - (top - rect.top()) - 10) / 3);
    contentFont(p, cfg_.fontSize - 2);
    for (int i = 0; i < 3; ++i) {
        const int y = top + i * rowH;
        p.setPen(QColor(230, 235, 240));
        p.drawText(QRect(rect.left() + 10, y, 52, rowH), Qt::AlignVCenter, rows[i].label);
        const QRect bar(rect.left() + 66, y + 3, rect.width() - 66 - 52, rowH - 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 28));
        p.drawRoundedRect(bar, 4, 4);
        if (rows[i].pct > 0) {
            QRect fill = bar;
            fill.setWidth(int(bar.width() * std::min(100, rows[i].pct) / 100.0));
            p.setBrush(rows[i].col);
            p.drawRoundedRect(fill, 4, 4);
        }
        p.setPen(QColor(230, 235, 240));
        p.drawText(QRect(bar.right() + 6, y, 46, rowH), Qt::AlignVCenter,
                   rows[i].pct < 0 ? QStringLiteral("--%") : QStringLiteral("%1%").arg(rows[i].pct));
    }
}

// ---------- Network ----------
// Layout ported 1:1 from the Python NetworkWidget (widgets.py): up/down
// rows with pill arrow icons and right-aligned rates at +42/+68, history
// graph panel from +98 (up line filled, "last 30s" / "max ..." labels),
// and a centered totals row at the bottom. Formatting matches the
// Python format_bytes_per_sec / _format_total_bytes (always 1 decimal).
static QString pyBytes(double v, bool perSec) {
    static const char *base[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    const int maxU = perSec ? 4 : 5;
    int u = 0;
    double value = v;
    while (value >= 1024.0 && u < maxU) { value /= 1024.0; ++u; }
    return QStringLiteral("%1 %2%3").arg(value, 0, 'f', 1).arg(QLatin1String(base[u])).arg(perSec ? QStringLiteral("/s") : QString());
}
static void drawNetArrow(QPainter &p, const QRectF &rect, bool up, const QColor &color, bool pill) {
    p.save();
    if (pill) {
        p.setPen(QPen(QColor(255, 255, 255, 34), 1));
        p.setBrush(QColor(255, 255, 255, 26));
        p.drawRoundedRect(QRectF(rect.left() + 2, rect.top() + 1, std::min(rect.width() - 4, 22.0), rect.height() - 2), 9, 9);
    }
    QColor c = color;
    c.setAlpha(std::max(150, c.alpha()));
    QPen pen(c, 2.3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double cx = rect.left() + std::min(rect.width(), 24.0) / 2.0;
    const double cy = rect.center().y();
    const double top = cy - 6.2, bottom = cy + 6.2, head = 5.0;
    if (up) {
        p.drawLine(QPointF(cx, bottom), QPointF(cx, top));
        p.drawLine(QPointF(cx, top), QPointF(cx - head, top + head));
        p.drawLine(QPointF(cx, top), QPointF(cx + head, top + head));
    } else {
        p.drawLine(QPointF(cx, top), QPointF(cx, bottom));
        p.drawLine(QPointF(cx, bottom), QPointF(cx - head, bottom - head));
        p.drawLine(QPointF(cx, bottom), QPointF(cx + head, bottom - head));
    }
    p.restore();
}

void NetworkWidget::onTick(const SystemSnapshot &s, const QDateTime &nowDt) {
    const qint64 now = nowDt.toMSecsSinceEpoch();
    totalDown_ = s.netDownTotal;
    totalUp_ = s.netUpTotal;
    if (prevMs_ > 0 && now > prevMs_ && s.netDownTotal >= prevDown_ && s.netUpTotal >= prevUp_) {
        // (v0.9.9: if the totals went DOWN — an interface vanished or
        // its counters reset — re-baseline silently instead of pushing
        // an underflowed garbage rate into the graph.)
        const double dt = (now - prevMs_) / 1000.0;
        downBps_ = double(s.netDownTotal - prevDown_) / dt;
        upBps_ = double(s.netUpTotal - prevUp_) / dt;
        histDown_.push_back(downBps_);
        histUp_.push_back(upBps_);
        while (histDown_.size() > 30) histDown_.pop_front(); // Python history limit: 30
        while (histUp_.size() > 30) histUp_.pop_front();
    }
    prevDown_ = s.netDownTotal; prevUp_ = s.netUpTotal; prevMs_ = now;
    // v0.9.10: this update() was MISSING — the widget computed rates
    // every second but never repainted, so it showed its very first
    // paint ("waiting for network data...") forever. The user's debug
    // log proved the counters were moving all along (+276 KB in one
    // second). Every other ticking widget calls update() in onTick.
    update();
}
void NetworkWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    const QColor downColor(QStringLiteral("#4DB2FF"));
    const QColor upColor(QStringLiteral("#FF5C5C"));
    const double contentX = rect.left() + 16;
    const double contentW = rect.width() - 32;

    auto drawRow = [&](double y, bool up, double bps, const QColor &color) {
        drawNetArrow(p, QRectF(contentX, y, 28, 20), up, color, true);
        p.setFont(QFont(QStringLiteral("Segoe UI"), 9, QFont::Bold));
        const QString label = up ? QStringLiteral("上り") : QStringLiteral("下り");
        const int labelW = std::min(42, std::max(28, p.fontMetrics().horizontalAdvance(label) + 8));
        p.setPen(QColor(235, 240, 250, 210));
        p.drawText(QRectF(contentX + 34, y, labelW, 20), Qt::AlignLeft | Qt::AlignVCenter, label);
        p.setFont(QFont(QStringLiteral("Segoe UI"), 9));
        p.setPen(color);
        p.drawText(QRectF(contentX + 34 + labelW + 6, y, std::max(20.0, contentW - (34 + labelW + 6)), 20),
                   Qt::AlignRight | Qt::AlignVCenter, pyBytes(bps, true));
    };
    drawRow(rect.top() + 42, true, upBps_, upColor);
    drawRow(rect.top() + 68, false, downBps_, downColor);

    // History graph panel.
    const double graphTop = rect.top() + 98;
    double graphH = rect.bottom() - graphTop - 32;
    if (graphH < 36) graphH = 36;
    const QRectF gr(contentX, graphTop, contentW, graphH);
    p.setBrush(QColor(255, 255, 255, 18));
    p.setPen(QPen(QColor(255, 255, 255, 35), 1));
    p.drawRoundedRect(gr, 8, 8);
    p.setPen(QPen(QColor(255, 255, 255, 26), 1));
    for (int i = 1; i < 4; ++i) {
        const double y = gr.top() + gr.height() * i / 4.0;
        p.drawLine(QPointF(gr.left(), y), QPointF(gr.right(), y));
    }
    for (int i = 1; i < 6; ++i) {
        const double x = gr.left() + gr.width() * i / 6.0;
        p.drawLine(QPointF(x, gr.top()), QPointF(x, gr.bottom()));
    }
    double maxV = 0;
    for (double v : histDown_) maxV = std::max(maxV, v);
    for (double v : histUp_) maxV = std::max(maxV, v);
    if (histDown_.empty() || maxV <= 0.0) {
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        p.setPen(QColor(210, 218, 230, 120));
        p.drawText(gr, Qt::AlignCenter, QStringLiteral("waiting for network data..."));
    } else {
        const double paddedMax = maxV * 1.15;
        auto drawLine = [&](const std::deque<double> &hist, const QColor &color, bool fill) {
            if (hist.empty()) return;
            QVector<QPointF> pts;
            const qsizetype count = hist.size();
            if (count == 1) pts.append(QPointF(gr.left(), gr.bottom()));
            for (qsizetype i = 0; i < count; ++i) {
                const double ratio = std::clamp(hist[i] / paddedMax, 0.0, 1.0);
                pts.append(QPointF(gr.left() + gr.width() * double(i) / std::max<qsizetype>(1, count - 1),
                                   gr.bottom() - gr.height() * ratio));
            }
            if (fill && pts.size() >= 2) {
                QPainterPath path;
                path.moveTo(pts.first().x(), gr.bottom());
                for (const QPointF &pt : pts) path.lineTo(pt);
                path.lineTo(pts.last().x(), gr.bottom());
                path.closeSubpath();
                QColor fc = color; fc.setAlpha(36);
                p.setBrush(fc);
                p.setPen(Qt::NoPen);
                p.drawPath(path);
            }
            QPen pen(color, 2);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            for (int i = 1; i < pts.size(); ++i) p.drawLine(pts[i - 1], pts[i]);
            QColor dc = color; dc.setAlpha(230);
            p.setBrush(dc);
            p.setPen(Qt::NoPen);
            p.drawEllipse(pts.last(), 3, 3);
        };
        drawLine(histUp_, upColor, true);
        drawLine(histDown_, downColor, false);
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        p.setPen(QColor(230, 235, 245, 160));
        p.drawText(QRectF(gr.left() + 8, gr.top() + 4, gr.width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("last 30s"));
        p.drawText(QRectF(gr.left() + 8, gr.bottom() - 20, gr.width() - 16, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("max ") + pyBytes(maxV, true));
    }

    // Totals row, centered: Total [down icon] 下り X [up icon] 上り Y
    {
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        const QFontMetrics fm = p.fontMetrics();
        const QString totalText = QStringLiteral("Total");
        const QString downLabel = QStringLiteral("下り"), upLabel = QStringLiteral("上り");
        const QString downVal = pyBytes(double(totalDown_), false);
        const QString upVal = pyBytes(double(totalUp_), false);
        const int gap = 5, iconW = 16;
        const int fullW = fm.horizontalAdvance(totalText) + gap
                        + iconW + 2 + fm.horizontalAdvance(downLabel) + 4 + fm.horizontalAdvance(downVal)
                        + gap * 2
                        + iconW + 2 + fm.horizontalAdvance(upLabel) + 4 + fm.horizontalAdvance(upVal);
        double x = rect.left() + 16 + std::max(0.0, (contentW - fullW) / 2.0);
        const QRectF row(rect.left(), rect.bottom() - 26, rect.width(), 18);
        const double y = row.top();
        auto text = [&](const QString &s, int w, const QColor &c) {
            p.setPen(c);
            p.drawText(QRectF(x, y, w, 18), Qt::AlignLeft | Qt::AlignVCenter, s);
            x += w;
        };
        text(totalText, fm.horizontalAdvance(totalText), QColor(210, 218, 230, 160));
        x += gap;
        drawNetArrow(p, QRectF(x, y, iconW, 18), false, downColor, false);
        x += iconW + 2;
        text(downLabel, fm.horizontalAdvance(downLabel), QColor(210, 218, 230, 170));
        x += 4;
        text(downVal, fm.horizontalAdvance(downVal), QColor(210, 218, 230, 170));
        x += gap * 2;
        drawNetArrow(p, QRectF(x, y, iconW, 18), true, upColor, false);
        x += iconW + 2;
        text(upLabel, fm.horizontalAdvance(upLabel), QColor(210, 218, 230, 170));
        x += 4;
        text(upVal, fm.horizontalAdvance(upVal), QColor(210, 218, 230, 170));
    }
}

// ---------- Calendar ----------
void CalendarWidget::onTick(const SystemSnapshot &, const QDateTime &now) {
    if (now.date() != date_) { date_ = now.date(); update(); }
}
void CalendarWidget::paintContent(QPainter &p, const QRect &rect) {
    const QDate d = date_.isValid() ? date_ : QDate::currentDate();
    contentFont(p, cfg_.fontSize, true);
    p.setPen(accent());
    p.drawText(QRect(rect.left(), rect.top() + 6, rect.width(), 22), Qt::AlignHCenter,
               d.toString(QStringLiteral("yyyy / MM")));
    const QDate first(d.year(), d.month(), 1);
    const int startDow = first.dayOfWeek() % 7; // Sunday = 0
    const int days = first.daysInMonth();
    const double cellW = (rect.width() - 16) / 7.0;
    const double cellH = std::max(14.0, (rect.height() - 58) / 6.0);
    const double top = rect.top() + 32;
    static const char *dowNames[] = {"日", "月", "火", "水", "木", "金", "土"};
    contentFont(p, cfg_.fontSize - 4, true);
    for (int i = 0; i < 7; ++i) {
        p.setPen(i == 0 ? QColor(255, 120, 120) : i == 6 ? QColor(120, 170, 255) : QColor(200, 208, 216));
        p.drawText(QRectF(rect.left() + 8 + i * cellW, top, cellW, cellH), Qt::AlignCenter, QString::fromUtf8(dowNames[i]));
    }
    contentFont(p, cfg_.fontSize - 3);
    for (int day = 1; day <= days; ++day) {
        const int idx = startDow + day - 1;
        const int col = idx % 7, row = idx / 7;
        const QRectF cell(rect.left() + 8 + col * cellW, top + cellH * (row + 1), cellW, cellH);
        if (day == d.day()) {
            p.setPen(Qt::NoPen);
            p.setBrush(accent());
            p.drawRoundedRect(cell.adjusted(2, 1, -2, -1), 4, 4);
            p.setPen(QColor(16, 20, 28));
        } else {
            p.setPen(col == 0 ? QColor(255, 150, 150) : col == 6 ? QColor(150, 190, 255) : QColor(230, 235, 240));
        }
        p.drawText(cell, Qt::AlignCenter, QString::number(day));
    }
}

// ---------- Text (HTML-lite) ----------
TextWidget::TextWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {
    doc_.setDefaultFont(QFont(QStringLiteral("Yu Gothic UI"), cfg_.fontSize));
    doc_.setHtml(cfg_.text);
    doc_.setTextWidth(width() - 20);
}
void TextWidget::resizeEvent(QResizeEvent *e) {
    WidgetWindow::resizeEvent(e);
    doc_.setTextWidth(width() - 20);
}
void TextWidget::paintContent(QPainter &p, const QRect &) {
    p.save();
    p.translate(10, 8);
    doc_.drawContents(&p);
    p.restore();
}

// ---------- Visualizer ----------
// v0.7: faithful re-implementation of the Python build's 25 styles from
// the Python widgets.py painters. Value pipeline matches
// runtime.py (52 geomspace/log1p/max-normalized bands, engine-smoothed).
// Styles the Python build itself left unfinished (music_lower_third_audio,
// digital_base_audio) fall back to classic, exactly as documented there.
static bool vizStyleNeedsArtwork(const QString &s) {
    static const char *kArt[] = {
        "turntable", "flat_spectrum", "audio_ripple", "hologram",
        "neon_soundwave", "glow_beat_music", "reactive_lights",
        "electro_dubstep", "minimal_beat", "cosmic_fusion", "round_base_audio"};
    for (const char *a : kArt)
        if (s == QLatin1String(a)) return true;
    return false;
}
static bool vizStyleIsCircular(const QString &s) {
    return vizStyleNeedsArtwork(s) || s == QLatin1String("ellipse") ||
           s == QLatin1String("spotlight_beat") || s == QLatin1String("hud_equalizer");
}
static bool vizStyleIsBarScene(const QString &s) {
    static const char *kBar[] = {
        "rainbow", "cyber", "minimal", "space", "enigmatic_echo_sound",
        "music_beat_wall", "luminance", "aurora"};
    for (const char *a : kBar)
        if (s == QLatin1String(a)) return true;
    return false;
}
// Styles whose look moves with time even in silence (rotation, falling
// dots, particles, sway): they must keep repainting at their frame rate.
static bool vizStyleIsTimeAnimated(const QString &s) {
    static const char *kAnim[] = {
        "rainbow", "cyber", "space", "aurora", "turntable", "spotlight_beat",
        "flat_spectrum", "neon_soundwave", "glow_beat_music", "reactive_lights",
        "electro_dubstep", "cosmic_fusion", "hud_equalizer", "audio_ripple"};
    for (const char *a : kAnim)
        if (s == QLatin1String(a)) return true;
    return false;
}

VisualizerWidget::VisualizerWidget(WidgetConfig cfg, AudioEngine *audio)
    : WidgetWindow(std::move(cfg)), audio_(audio),
      bars_(AudioEngine::kBands, 0.0f), peaks_(AudioEngine::kBands, 0.0f) {
    clock_.start();
    connect(&timer_, &QTimer::timeout, this, [this] {
        const QVector<float> s = audio_ ? audio_->spectrum() : QVector<float>(AudioEngine::kBands, 0.0f);
        const qint64 ms = clock_.elapsed();
        const double dt = std::max(0.001, std::min(0.08, (ms - lastTickMs_) / 1000.0));
        lastTickMs_ = ms;
        float maxDiff = 0.0f, maxVal = 0.0f;
        for (int i = 0; i < AudioEngine::kBands; ++i) {
            maxDiff = std::max(maxDiff, std::abs(s[i] - bars_[i]));
            maxVal = std::max(maxVal, s[i]);
            // Python peak rule: peak = max(value, peak - 1.15*dt)
            peaks_[i] = std::max(s[i], std::max(0.0f, peaks_[i] - float(1.15 * dt)));
        }
        bars_ = s;
        const QString style = cfg_.vizStyle();
        const bool settled = maxVal < 0.003f && maxDiff < 0.003f;
        if (settled && !vizStyleIsTimeAnimated(style) && powder_.isEmpty()) return;
        update();
    });
    applyFrameRate();
    syncArtworkNeed();
    connect(MediaHelperClient::instance(), &MediaHelperClient::mediaChanged, this, [this] { update(); });
}

VisualizerWidget::~VisualizerWidget() {
    if (artworkHeld_) MediaHelperClient::instance()->release();
}

void VisualizerWidget::syncArtworkNeed() {
    const bool need = vizStyleNeedsArtwork(cfg_.vizStyle());
    if (need == artworkHeld_) return;
    artworkHeld_ = need;
    if (need) MediaHelperClient::instance()->acquire();
    else MediaHelperClient::instance()->release();
}

void VisualizerWidget::applyFrameRate() {
    // Python default is 60fps; the C++ build caps animation at 30fps by
    // design (design principle 4). The configured value is honored
    // below the cap, so a user who set 15fps gets 15fps.
    int fps = 30;
    if (cfg_.vizBool("visualizer_frame_rate_enabled", true))
        fps = std::max(10, std::min(30, cfg_.vizInt("visualizer_frame_rate", 60)));
    timer_.start(1000 / fps);
}

QColor VisualizerWidget::rainbowColor(double phase, int alpha) {
    // Python _rainbow_color: HSV, saturation 0.82, value 1.0.
    double ph = std::fmod(phase, 1.0);
    if (ph < 0.0) ph += 1.0;
    return QColor::fromHsvF(ph, 0.82, 1.0, std::max(0, std::min(255, alpha)) / 255.0);
}

void VisualizerWidget::capsule(QPainter &p, const QPointF &c, double ang, double r0, double len, double w, const QColor &col) const {
    if (len <= 0.0 || w <= 0.0) return;
    p.save();
    p.translate(c.x() + std::cos(ang) * r0, c.y() + std::sin(ang) * r0);
    p.rotate(ang * 180.0 / M_PI);
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawRoundedRect(QRectF(0, -w / 2.0, len, w), 4, 4);
    p.restore();
}

void VisualizerWidget::drawCover(QPainter &p, const QPointF &c, double rx, double ry, double spinDeg) const {
    if (rx < 2.0 || ry < 2.0) return;
    const QImage art = MediaHelperClient::instance()->info().artwork;
    p.save();
    QPainterPath clip;
    clip.addEllipse(c, rx, ry);
    p.setClipPath(clip);
    if (!art.isNull()) {
        // Cover-fit the artwork into the disc's bounding box, with the
        // optional spin the Python turntable applies (now*0.55 rad/s).
        p.translate(c);
        if (spinDeg != 0.0) p.rotate(spinDeg);
        const double scale = std::max(2.0 * rx / art.width(), 2.0 * ry / art.height());
        const QSizeF sz(art.width() * scale, art.height() * scale);
        p.drawImage(QRectF(QPointF(-sz.width() / 2.0, -sz.height() / 2.0), sz), art);
        p.translate(-c.x(), -c.y());
    } else {
        QRadialGradient g(c, rx);
        g.setColorAt(0.0, QColor(52, 58, 74, 255));
        g.setColorAt(0.75, QColor(26, 29, 40, 255));
        g.setColorAt(1.0, QColor(12, 13, 20, 255));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, rx, ry);
        QFont f = p.font();
        f.setPointSize(int(std::max(10.0, ry * 0.55)));
        p.setFont(f);
        QColor note = accent();
        note.setAlpha(150);
        p.setPen(note);
        p.drawText(QRectF(c.x() - rx, c.y() - ry, 2 * rx, 2 * ry), Qt::AlignCenter, QStringLiteral("♪"));
    }
    p.restore();
    p.save();
    p.setPen(QPen(QColor(255, 255, 255, 44), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, rx, ry);
    p.restore();
}

void VisualizerWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    const int top = rect.top() + (cfg_.title.isEmpty() ? 8 : 30);
    const QRect area(rect.left() + 10, top, rect.width() - 20, rect.height() - (top - rect.top()) - 10);
    if (area.width() < 12 || area.height() < 12) return;
    now_ = clock_.elapsed() / 1000.0;
    const int n = AudioEngine::kBands;
    double sum = 0.0, bassSum = 0.0;
    const int bassN = std::max(1, n / 5);
    for (int i = 0; i < n; ++i) {
        sum += bars_[i];
        if (i < bassN) bassSum += bars_[i];
    }
    avg_ = sum / n;
    bass_ = bassSum / bassN;
    QString style = cfg_.vizStyle();
    // Legacy styles the Python build itself removed fall back to classic.
    static const char *removed[] = {
        "alternative","audio_react","audio_tunnel","audio_tunnel_sphere","bass_drop",
        "beat_fluorescent_app","circle","circle_waveform","dynamic_glitch","electric_pulse",
        "energy_shield","euphoria_motion","futuristic_tunnel","led_audio_wave",
        "liquid_audio_spectrum","lofi_vibes","matrix","melodic_vibe","meteor_shower",
        "music_logo_reveal","nebula","neon_tunnel_wire","parallax_waves",
        "particle_audio_visualizer","radar_scan","rainbow_ring_dj","retro_future","urban_timelapse"};
    for (const char *r : removed)
        if (style == QLatin1String(r)) { style = QStringLiteral("classic"); break; }
    p.setRenderHint(QPainter::Antialiasing, true);
    if (style == QLatin1String("classic")) { paintClassic(p, area); return; }
    if (vizStyleIsBarScene(style)) { paintBarScene(p, area, style); return; }
    if (vizStyleIsCircular(style)) { paintCircularScene(p, area, style); return; }
    paintClassic(p, area); // unfinished upstream styles + anything unknown
}

// classic = the Python "direct" generic bars: white-tipped vertical
// gradient, radius 3, gap 28% of slot, peak caps 2px above the peak.
void VisualizerWidget::paintClassic(QPainter &p, const QRect &area) {
    const int n = AudioEngine::kBands;
    const bool flip = cfg_.vizBool("visualizer_flip_vertical", false);
    const bool vertical = cfg_.extra.value(QStringLiteral("visualizer_orientation")).toString() == QLatin1String("vertical");
    const bool peakOn = cfg_.vizBool("visualizer_peak_bar_enabled", true);
    const bool glowOn = cfg_.vizBool("visualizer_glow_enabled", true);
    const bool shadowOn = cfg_.vizBool("visualizer_shadow_enabled", true);
    const double widthScale = std::max(0.2, std::min(3.0, cfg_.vizDouble("visualizer_bar_width_scale", 1.0)));
    const QColor base = accent();
    const double shOx = cfg_.vizDouble("visualizer_shadow_offset_x", 3.0);
    const double shOy = cfg_.vizDouble("visualizer_shadow_offset_y", 4.0);
    const double shAlpha = 255.0 * std::max(0.0, std::min(1.0,
        cfg_.vizDouble("visualizer_shadow_opacity", 0.65) * cfg_.vizDouble("visualizer_shadow_strength", 1.0)));
    const double shDepth = std::max(0.2, std::min(2.5, cfg_.vizDouble("visualizer_shadow_depth", 1.0)));

    const double axisLen = vertical ? area.height() : area.width();
    const double crossLen = vertical ? area.width() : area.height();
    const double slot = axisLen / n;
    const double gap = std::min(3.0, std::max(1.0, slot * 0.28));
    const double thick = std::max(2.0, std::min(slot * 0.96, slot * widthScale - gap));

    auto barRect = [&](int i, double v, double depthScale, QPointF &peakPos) {
        const double len = std::max(2.0, crossLen * v * depthScale);
        const double aPos = (vertical ? area.top() : area.left()) + i * slot + (slot - thick) / 2.0;
        QRectF r;
        if (!vertical) {
            const double y = flip ? area.top() : area.bottom() - len;
            r = QRectF(aPos, y, thick, len);
            peakPos = QPointF(aPos, flip ? area.top() + crossLen * peaks_[i] : area.bottom() - crossLen * peaks_[i]);
        } else {
            const double x = flip ? area.right() - len : area.left();
            r = QRectF(x, aPos, len, thick);
            peakPos = QPointF(flip ? area.right() - crossLen * peaks_[i] : area.left() + crossLen * peaks_[i], aPos);
        }
        return r;
    };

    if (shadowOn) { // config shadow pass (kept from v0.6, classic only)
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, int(shAlpha)));
        for (int i = 0; i < n; ++i) {
            QPointF pp;
            QRectF r = barRect(i, bars_[i], shDepth, pp);
            r.translate(flip && vertical ? -shOx : shOx, flip && !vertical ? -shOy : shOy);
            p.drawRoundedRect(r, 3, 3);
        }
    }
    for (int i = 0; i < n; ++i) {
        const double v = bars_[i];
        QPointF pp;
        const QRectF r = barRect(i, v, 1.0, pp);
        if (glowOn && v > 0.025) { // Python _draw_visualizer_bar_glow
            QColor glow = base;
            glow.setAlpha(std::max(18, std::min(145, int(42 + v * 125))));
            p.setPen(Qt::NoPen);
            p.setBrush(glow);
            if (vertical)
                p.drawRoundedRect(QRectF(r.left() - 6, r.top() - std::max(2.0, r.height() * 0.45), r.width() + 12, r.height() + std::max(4.0, r.height() * 0.9)), 5, 5);
            else
                p.drawRoundedRect(QRectF(r.left() - std::max(2.0, r.width() * 0.45), r.top() - 5, r.width() + std::max(4.0, r.width() * 0.9), r.height() + 10), 5, 5);
            QColor edge = base;
            edge.setAlpha(std::max(20, std::min(120, int(30 + v * 100))));
            p.setBrush(edge);
            if (vertical)
                p.drawRoundedRect(QRectF(flip ? r.left() - 3 : r.right() - 3, r.top() - 1, 6, r.height() + 2), 3, 3);
            else if (flip)
                p.drawRoundedRect(QRectF(r.left() - 1, r.bottom() - 3, r.width() + 2, 6), 3, 3);
            else
                p.drawRoundedRect(QRectF(r.left() - 1, r.top() - 3, r.width() + 2, 6), 3, 3);
        }
        QLinearGradient grad;
        if (!vertical) {
            grad = QLinearGradient(r.left(), r.top(), r.left(), r.bottom());
            if (flip) {
                grad.setColorAt(0.0, QColor(255, 255, 255, 105));
                grad.setColorAt(0.38, QColor(base.red(), base.green(), base.blue(), 245));
                grad.setColorAt(1.0, QColor(base.red(), base.green(), base.blue(), 165));
            } else {
                grad.setColorAt(0.0, QColor(255, 255, 255, 110));
                grad.setColorAt(0.35, QColor(base.red(), base.green(), base.blue(), 245));
                grad.setColorAt(1.0, QColor(base.red(), base.green(), base.blue(), 165));
            }
        } else {
            grad = QLinearGradient(r.left(), r.top(), r.right(), r.top());
            if (flip) { grad.setStart(r.right(), r.top()); grad.setFinalStop(r.left(), r.top()); }
            grad.setColorAt(0.0, QColor(base.red(), base.green(), base.blue(), 165));
            grad.setColorAt(0.62, QColor(base.red(), base.green(), base.blue(), 245));
            grad.setColorAt(1.0, QColor(255, 255, 255, 110));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawRoundedRect(r, 3, 3);
        if (peakOn) { // Python _draw_visualizer_peak_bar
            QColor capCol = base;
            capCol.setAlpha(std::max(150, std::min(255, int(165 + v * 90))));
            p.setBrush(capCol);
            if (!vertical) {
                const double capY = flip ? pp.y() + 2.0 : pp.y() - 3.0 - 2.0;
                p.drawRoundedRect(QRectF(r.left() - 0.75, capY, std::max(6.0, thick + 1.5), 3.0), 1.5, 1.5);
            } else {
                const double capX = flip ? pp.x() - 2.0 - 3.0 : pp.x() + 2.0;
                p.drawRoundedRect(QRectF(capX, r.top() - 0.75, 3.0, std::max(6.0, thick + 1.5)), 1.5, 1.5);
            }
        }
    }
}

void VisualizerWidget::paintBarScene(QPainter &p, const QRect &area, const QString &style) {
    const int n = AudioEngine::kBands;
    const double now = now_;
    const double aw = area.width(), ah = area.height();
    const double cx = area.center().x(), cy = area.center().y();
    const double widthScale = std::max(0.2, std::min(3.0, cfg_.vizDouble("visualizer_bar_width_scale", 1.0)));
    const QColor base = accent();
    p.save();
    if (cfg_.vizBool("visualizer_flip_vertical", false)) { // Python mirrors the whole scene
        p.translate(0.0, area.top() + area.bottom());
        p.scale(1.0, -1.0);
    }
    const double slot = aw / n;
    p.setPen(Qt::NoPen);

    if (style == QLatin1String("minimal")) {
        QColor c = base;
        c.setAlpha(195);
        p.setBrush(c);
        for (int i = 0; i < n; ++i) {
            const double h = ah * (0.025 + bars_[i] * 0.46);
            p.drawRoundedRect(QRectF(area.left() + i * slot + slot * 0.28, cy - h / 2,
                                     std::max(1.0, slot * 0.44 * widthScale), h), 2, 2);
        }
    } else if (style == QLatin1String("music_beat_wall")) {
        for (int i = 0; i < n; ++i) {
            const double h = ah * (0.06 + bars_[i] * 0.76);
            QColor c = base;
            c.setAlpha(102 + int(bars_[i] * 55));
            p.setBrush(c);
            p.drawRoundedRect(QRectF(area.left() + i * slot + slot * 0.16, area.bottom() - h,
                                     std::max(1.0, slot * 0.64 * widthScale), h), 3, 3);
        }
    } else if (style == QLatin1String("enigmatic_echo_sound")) {
        const double bw = std::max(2.0, 8.0 * widthScale);
        p.setBrush(QColor(255, 255, 255, 205));
        for (int i = 0; i < n; ++i) {
            const double boost = (i < n * 0.22 || i > n * 0.78) ? 1.35 : 0.82;
            const double h = ah * (0.05 + bars_[i] * 0.74 * boost);
            p.drawRoundedRect(QRectF(area.left() + i * slot + slot * 0.5 - bw / 2, cy - h / 2, bw, h), 3, 3);
        }
    } else if (style == QLatin1String("space")) {
        for (int i = 0; i < 54; ++i) { // drifting starfield dots
            const double x = area.left() + std::fmod(i * 73 + int(now * 20), std::max(1.0, aw));
            const double y = area.top() + std::fmod(i * 41 + int(now * 8), std::max(1.0, ah));
            p.setBrush(QColor(150, 180, 255, 34 + (i % 4) * 14));
            p.drawEllipse(QPointF(x, y), 1.0, 1.0);
        }
        for (int i = 0; i < n; ++i) {
            const double h = ah * (0.03 + bars_[i] * 0.34);
            p.setBrush(QColor(255, 255, 255, 145 + int(bars_[i] * 80)));
            const double x = area.left() + i * slot + slot * 0.5;
            p.drawRoundedRect(QRectF(x - 0.8 * widthScale, cy - h, 1.6 * widthScale, h * 2), 2, 2);
        }
    } else if (style == QLatin1String("cyber")) {
        const double baseY = area.top() + ah * 0.28;
        QVector<double> lens(n);
        for (int i = 0; i < n; ++i) {
            const double v = bars_[i];
            const double len = ah * (0.04 + v * 0.55);
            lens[i] = len;
            QColor c = rainbowColor(i / double(n) * 0.85 + now * 0.035, 150 + int(v * 80));
            p.setBrush(c);
            const double bw = slot * 0.5;
            p.drawRoundedRect(QRectF(area.left() + i * slot + (slot - bw) / 2, baseY, bw, len), 2, 2);
        }
        for (int k = 0; k < 36; ++k) { // falling dots under the bar ends
            const int i = int(k * n / 36.0);
            const double age = std::fmod(now * 0.65 + i * 0.021, 1.0);
            const double x = area.left() + i * slot + slot / 2;
            const double y = baseY + lens[i] + age * ah * 0.30;
            QColor c = rainbowColor(i / double(n) * 0.85 + now * 0.035, int((1.0 - age) * 190));
            p.setBrush(c);
            p.drawEllipse(QPointF(x, y), 1.6, 1.6);
        }
    } else if (style == QLatin1String("rainbow")) {
        const double bw = std::max(1.0, slot * 0.38 * widthScale);
        QVector<float> tones(n);
        QVector<double> tops(n);
        for (int i = 0; i < n; ++i) {
            const double t = i / double(std::max(1, n - 1));
            auto clampIdx = [&](double f) { return std::max(0, std::min(n - 1, int(f * (n - 1)))); };
            const double lowV = bars_[clampIdx(0.05 + 0.11 * t)];
            const double midV = bars_[clampIdx(0.42 + 0.18 * t)];
            const double highV = bars_[clampIdx(0.74 + 0.10 * (1.0 - std::abs(t - 0.5) * 2.0))];
            const double lw = std::pow(1.0 - t, 8.55);
            const double mw = std::pow(t, 0.85);
            const double hw = std::pow(std::max(0.0, 1.0 - std::abs(t - 0.5) * 2.0), 1.80) * 1.65;
            const double tone = std::max(0.0, std::min(1.0,
                (lowV * lw + midV * mw + highV * hw + bars_[i] * 0.012 + avg_ * 0.0008) /
                std::max(0.001, lw + mw + hw + 0.012 + 0.0008)));
            tones[i] = float(tone);
            const double h = ah * (0.030 + tone * 0.340);
            tops[i] = cy - h;
            const double bx = area.left() + i * slot + slot * 0.31;
            p.setBrush(rainbowColor(t + now * 0.050, 230));
            p.drawRoundedRect(QRectF(bx, cy - h, bw, h), 3, 3);
            QColor ref = rainbowColor(t + now * 0.050, 14 + int(tone * 22));
            p.setBrush(ref);
            p.drawRoundedRect(QRectF(bx, cy + 2, bw, h * 0.12), 3, 3);
        }
        // Scale-powder: spawn flakes only on sharp peaks (Python rules).
        if (rainbowPrevTone_.size() != n) {
            rainbowPrevTone_ = QVector<float>(n, 0.0f);
            rainbowLastSpawn_ = QVector<double>(n, -9999.0);
        }
        const int step = std::max(1, n / 15);
        for (int i = 0; i < n; i += step) {
            const double rise = tones[i] - rainbowPrevTone_[i];
            if (tones[i] >= 0.120 && rise >= 0.055 && now - rainbowLastSpawn_[i] >= 0.140) {
                rainbowLastSpawn_[i] = now;
                for (int spark = 0; spark < 6; ++spark) {
                    Powder pw;
                    pw.birth = now;
                    pw.x = area.left() + i * slot + slot * 0.5;
                    pw.y = tops[i];
                    pw.tone = tones[i];
                    pw.spark = spark;
                    pw.seed = std::fmod(i * 0.017 + spark * 0.109 + now * 0.013, 1.0);
                    powder_.append(pw);
                }
            }
        }
        rainbowPrevTone_ = tones;
        QVector<Powder> alive;
        for (const Powder &pw : powder_) {
            const double age = (now - pw.birth) / 1.01;
            if (age < 0.0 || age >= 1.0) continue;
            alive.append(pw);
            double flutter = std::sin(now * 1.35 + pw.seed * 17.0 + pw.spark * 1.77) * slot * (0.85 + age * 2.35);
            flutter += std::sin(now * 2.55 + pw.seed * 11.0 + pw.spark * 2.63) * slot * (0.38 + age * 1.15);
            flutter += std::sin(age * M_PI * 2.5 + pw.spark * 0.73) * slot * 0.44;
            const double px = pw.x + flutter + (pw.spark - 4) * slot * 0.105;
            const double py = pw.y - age * ah * (0.42 + pw.tone * 0.30) - pw.spark * ah * 0.005;
            const double pr = (0.65 + pw.tone * 1.05 + (pw.spark % 3) * 0.885) * std::max(1.0, widthScale);
            const double fade = 1.0 - age;
            const int alpha = int(fade * fade * fade * 230);
            const double t = pw.x / std::max(1.0, aw); // hue by field position
            p.save();
            p.translate(px, py);
            p.rotate(std::sin(now * 1.65 + pw.seed * 13.0 + pw.spark * 0.97) * 58.0 + age * 155.0 + pw.spark * 23.0);
            p.setBrush(rainbowColor(std::fmod(t + pw.spark * 0.081 + now * 0.030 + age * 0.065, 1.0), alpha));
            p.drawRoundedRect(QRectF(-pr * 1.2, -pr * 0.5, pr * 2.4, pr), 1.2, 1.2);
            p.restore();
        }
        powder_ = alive.mid(std::max(qsizetype(0), alive.size() - 700));
    } else if (style == QLatin1String("luminance")) {
        QVector<double> sm(n);
        double run = avg_;
        for (int i = 0; i < n; ++i) { run = run * 0.45 + bars_[i] * 0.55; sm[i] = run; }
        for (int layer = 2; layer >= 0; --layer) { // ghosts first, core last
            const double s = layer == 0 ? 1.0 : 0.82;
            const double yOff = layer == 0 ? 0.0 : (layer == 1 ? -ah * 0.055 : ah * 0.055);
            QColor col;
            if (layer == 0) { col = QColor(246, 249, 255, 215); }
            else { col = base; col.setAlpha(layer == 1 ? 60 : 45); }
            QPainterPath path;
            for (int i = 0; i < n; ++i) {
                const double x = area.left() + aw * i / std::max(1, n - 1);
                const double thick = (ah * 0.04 + sm[i] * ah * 0.52) * s * (1.0 - 0.25 * i / n);
                const double y = cy + yOff - thick / 2;
                if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
            }
            for (int i = n - 1; i >= 0; --i) {
                const double x = area.left() + aw * i / std::max(1, n - 1);
                const double thick = (ah * 0.04 + sm[i] * ah * 0.52) * s * (1.0 - 0.25 * i / n);
                path.lineTo(x, cy + yOff + thick / 2);
            }
            path.closeSubpath();
            p.setBrush(col);
            p.drawPath(path);
        }
    } else if (style == QLatin1String("aurora")) {
        // The flutter version the author originally wanted (design notes):
        // the cloth's lower edge is modulated per column by the spectrum,
        // on top of the shipped "fast aurora" gradient curtain.
        const int cols = 56;
        for (int layer = 1; layer >= 0; --layer) {
            const double phase = layer * 1.7;
            const double alphaScale = layer == 0 ? 0.42 : 0.20;
            QPainterPath path;
            const double topBase = area.top() + ah * 0.105;
            path.moveTo(area.left(), topBase + std::sin(now * 0.8 + phase) * ah * 0.018);
            for (int j = 1; j <= cols; ++j) {
                const double x = area.left() + aw * j / cols;
                path.lineTo(x, topBase + std::sin(j * 0.35 + now * 0.8 + phase) * ah * 0.018);
            }
            for (int j = cols; j >= 0; --j) {
                const double x = area.left() + aw * j / cols;
                const double v = bars_[std::max(0, std::min(n - 1, int(j / double(cols) * (n - 1))))];
                const double y = area.top() + ah * 0.825 + std::sin(j * 0.30 - now * 1.1 + phase) * ah * 0.03
                                 + (v - avg_) * ah * 0.38;
                path.lineTo(x, y);
            }
            path.closeSubpath();
            QLinearGradient grad(area.left(), area.top(), area.left(), area.bottom());
            auto withAlpha = [&](const QColor &c, double a) {
                QColor r = c; r.setAlphaF(a * alphaScale); return r;
            };
            grad.setColorAt(0.0, withAlpha(QColor(80, 255, 170), 1.0));
            grad.setColorAt(0.35, withAlpha(QColor(60, 230, 220), 0.95));
            grad.setColorAt(0.65, withAlpha(QColor(80, 140, 255), 0.9));
            grad.setColorAt(1.0, withAlpha(QColor(170, 90, 255), 0.85));
            p.setBrush(grad);
            p.drawPath(path);
        }
        for (int i = 0; i < n; ++i) { // faint rainbow minimal spectrum on top
            const double h = ah * (0.025 + bars_[i] * 0.30);
            p.setBrush(rainbowColor(i / double(n), 90));
            p.drawRoundedRect(QRectF(area.left() + i * slot + slot * 0.42, cy - h / 2,
                                     std::max(1.0, 1.6 * widthScale), h), 2, 2);
        }
    }
    p.restore();
}

void VisualizerWidget::paintCircularScene(QPainter &p, const QRect &area, const QString &style) {
    const int n = AudioEngine::kBands;
    const double now = now_;
    const double aw = area.width(), ah = area.height();
    const QPointF c(area.center().x(), area.center().y());
    const double shortS = std::min(aw, ah);
    const double maxR = std::max(2.0, shortS * 0.43);
    const double radius = std::min(shortS * (0.23 + 0.12 * bass_), maxR * 0.72);
    const double widthScale = std::max(0.2, std::min(3.0, cfg_.vizDouble("visualizer_bar_width_scale", 1.0)));
    const QColor base = accent();
    p.save();
    if (cfg_.vizBool("visualizer_flip_vertical", false)) {
        p.translate(0.0, area.top() + area.bottom());
        p.scale(1.0, -1.0);
    }
    if (cfg_.vizBool("visualizer_glow_enabled", true)) {
        QRadialGradient g(c, maxR * 1.06);
        QColor gc = base; gc.setAlpha(24);
        g.setColorAt(0.0, gc);
        gc.setAlpha(0);
        g.setColorAt(1.0, gc);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, maxR * 1.06, maxR * 1.06);
    }
    auto ang = [](int i, int count) { return i / double(count) * 2.0 * M_PI - M_PI / 2.0; };
    auto val = [&](int i) { return double(bars_[std::max(0, std::min(n - 1, i))]); };

    if (style == QLatin1String("ellipse")) {
        const double rx = 1.58, ry = 0.54;
        const double core = std::min(radius, maxR * 0.48);
        const int step = std::max(1, n / 88);
        for (int i = 0; i < n; i += step) {
            const double v = val(i), a = ang(i, n);
            QColor col = base;
            col.setAlpha(130 + int(v * 110));
            p.save();
            p.translate(c.x() + std::cos(a) * core * rx, c.y() + std::sin(a) * core * ry);
            p.rotate(a * 180.0 / M_PI);
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            const double len = shortS * (0.035 + v * 0.16);
            p.drawRoundedRect(QRectF(0, -(1.5 + v * 3.0) * widthScale, len, (3.0 + v * 6.0) * widthScale), 4, 4);
            p.restore();
        }
    } else if (style == QLatin1String("turntable")) {
        // Side view (rotate -3 deg, squash Y to 0.38), per the Python painter.
        const double coverR = std::max(1.0, std::min(maxR * 0.62, shortS * 0.31) * 0.84 - 10.0);
        const double outerR = std::min(maxR * 0.98, coverR + 70.0);
        p.save();
        p.translate(c);
        p.rotate(-3.0);
        p.scale(1.0, 0.38);
        p.translate(-c.x(), -c.y());
        QRadialGradient grad(c, outerR);
        grad.setColorAt(0.0, QColor(245, 248, 255, 235));
        grad.setColorAt(0.22, QColor(base.red(), base.green(), base.blue(), 135));
        grad.setColorAt(0.56, QColor(18, 20, 26, 235));
        grad.setColorAt(1.0, QColor(4, 5, 8, 245));
        p.setPen(Qt::NoPen);
        p.setBrush(grad);
        p.drawEllipse(c, outerR, outerR);
        p.setBrush(Qt::NoBrush);
        for (int k = 0; k < 5; ++k) {
            p.setPen(QPen(QColor(220, 230, 245, 45 + k * 15), 1));
            p.drawEllipse(c, coverR * (0.34 + k * 0.13), coverR * (0.34 + k * 0.13));
        }
        drawCover(p, c, coverR, coverR, now * 0.55 * 180.0 / M_PI); // the disc spins
        const double barInner = std::min(outerR - 2.0, coverR + 8.0);
        const double room = std::max(1.0, outerR - barInner - 4.0);
        const int step = std::max(1, n / 74);
        for (int i = 0; i < n; i += step) {
            const double v = val(i);
            capsule(p, c, ang(i, n), barInner, std::min(room, shortS * (0.025 + v * 0.13)),
                    std::max(1.8, (2.0 + v * 4.0) * widthScale), QColor(230, 235, 245, 110 + int(v * 110)));
        }
        p.restore();
    } else if (style == QLatin1String("spotlight_beat")) {
        for (int ring = 0; ring < 2; ++ring) {
            const double pulse = 1.0 + bass_ * (0.025 + ring * 0.012);
            const double rx = maxR * (0.54 + ring * 0.22) * pulse;
            const double ry = rx * (0.45 + bass_ * 0.018);
            const double phase = now * (ring == 0 ? 0.36 : -0.32);
            for (int k = 0; k < 42; ++k) {
                const double v = val(k * n / 42);
                const double a = k / 42.0 * 2.0 * M_PI + phase + std::sin(now * 1.2 + k * 0.17) * v * 0.05;
                const QPointF pt(c.x() + std::cos(a) * rx, c.y() + std::sin(a) * ry * (ring ? -1 : 1));
                QColor col = ring ? base : QColor(245, 248, 255);
                QColor orb = col;
                orb.setAlpha(30 + int(v * 78));
                p.setPen(Qt::NoPen);
                p.setBrush(orb);
                p.drawEllipse(pt, 5 + v * 16, 5 + v * 16);
                col.setAlpha(72 + int(v * 175));
                p.setBrush(col);
                const double bw = std::max(3.8, 4.95 * widthScale);
                const double bh = bw * (1.0 + v * 4.85 + bass_ * 0.90);
                p.drawRoundedRect(QRectF(pt.x() - bw / 2, pt.y() - bh / 2, bw, bh), 3, 3);
            }
        }
    } else if (style == QLatin1String("flat_spectrum")) {
        // Observation-led composition (design notes): side-view cover disc,
        // rim capsules, and a separate outer ring modulated by the audio.
        // The ring uses the Python painter's focused mid-band values,
        // mirrored, with its temporal smoothing and energy gate.
        const int S = 104, half = S / 2;
        QVector<float> focused(S);
        for (int s = 0; s < half; ++s) {
            const double pos = s / double(std::max(1, half - 1));
            const int src = std::max(0, std::min(n - 1, int(n * 0.18 + pos * (n * 0.64 - n * 0.18))));
            const double cw = 1.0 - std::min(1.0, std::abs(pos - 0.45) / 0.45);
            focused[s] = float(std::max(0.0, std::min(1.0, (val(src) - bass_ * 0.18) * (0.36 + cw * 0.64) * 1.58)));
            focused[S - 1 - s] = focused[s];
        }
        double fPeak = 0.0, fSum = 0.0;
        for (float v : focused) { fPeak = std::max(fPeak, double(v)); fSum += v; }
        const double energy = fPeak * 0.68 + (fSum / S) * 0.32;
        double target = std::max(std::max(0.0, std::min(1.0, (energy - 0.08) / 0.57)),
                                 std::max(0.0, std::min(1.0, (energy - flatPrevEnergy_) / 0.18)) * 0.80);
        if (target < 0.055) target = 0.0;
        flatGate_ = target >= flatGate_ ? flatGate_ * 0.54 + target * 0.46 : flatGate_ * 0.88 + target * 0.12;
        if (flatGate_ < 0.035) flatGate_ = 0.0;
        flatPrevEnergy_ = energy;
        if (flatSmooth_.size() != S) flatSmooth_ = QVector<float>(S, 0.0f);
        QVector<float> sm(S);
        for (int s = 0; s < S; ++s) {
            const float cur = float(focused[s] * flatGate_);
            sm[s] = cur >= flatSmooth_[s] ? flatSmooth_[s] * 0.70f + cur * 0.30f : flatSmooth_[s] * 0.90f + cur * 0.10f;
        }
        flatSmooth_ = sm;
        p.save();
        p.translate(c);
        p.rotate(0.5);
        p.scale(1.0, 0.47);
        p.translate(-c.x(), -c.y());
        drawCover(p, c, maxR * 0.62, maxR * 0.62);
        const int step = std::max(1, n / 74);
        for (int i = 0; i < n; i += step) {
            const double v = val(i);
            QColor col = base;
            col.setAlpha(160 + int(v * 60));
            capsule(p, c, ang(i, n), maxR * 0.62, shortS * (0.02 + v * 0.10), std::max(1.6, (2.0 + v * 3.0) * widthScale), col);
        }
        for (int pass = 0; pass < 2; ++pass) {
            QPainterPath path;
            for (int s = 0; s <= S; ++s) {
                const int idx = s % S;
                const double blurred = sm[(idx + S - 2) % S] * 0.10 + sm[(idx + S - 1) % S] * 0.20 + sm[idx] * 0.40 +
                                       sm[(idx + 1) % S] * 0.20 + sm[(idx + 2) % S] * 0.10;
                const double r = std::max(maxR * 0.54, maxR * (0.70 + blurred * 0.22)) - pass * 2.0;
                const double a = ang(idx, S);
                const QPointF pt(c.x() + std::cos(a) * r, c.y() + std::sin(a) * r);
                if (s == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(QPen(pass == 0 ? QColor(base.red(), base.green(), base.blue(), 200) : QColor(255, 255, 255, 90),
                          pass == 0 ? 1.5 : 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        p.restore();
    } else if (style == QLatin1String("audio_ripple")) {
        const double r0 = maxR * 0.56;
        drawCover(p, c, r0, r0);
        const int S = 128;
        for (int pass = 0; pass < 2; ++pass) {
            QPainterPath path;
            for (int s = 0; s <= S; ++s) {
                const double v = val((s % S) * n / S);
                const double r = r0 + (pass == 0 ? 3.0 + v * shortS * 0.045 : 1.5 + v * shortS * 0.030);
                const double a = ang(s % S, S);
                const QPointF pt(c.x() + std::cos(a) * r, c.y() + std::sin(a) * r);
                if (s == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(QPen(pass == 0 ? QColor(base.red(), base.green(), base.blue(), 210) : QColor(255, 255, 255, 110), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    } else if (style == QLatin1String("hologram")) {
        const double r0 = maxR * 0.53;
        drawCover(p, c, r0, r0);
        p.save(); // white translucent scanlines inside the disc
        QPainterPath clip;
        clip.addEllipse(c, r0, r0);
        p.setClipPath(clip);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < n; i += 2) {
            const double x = c.x() - r0 + (i / double(n)) * 2.0 * r0;
            const double dx = x - c.x();
            const double chord = std::sqrt(std::max(0.0, r0 * r0 - dx * dx));
            const double h = chord * 2.0 * (0.25 + val(i) * 0.55);
            p.setBrush(QColor(255, 255, 255, 46 + int(val(i) * 50)));
            p.drawRect(QRectF(x - 0.75 * widthScale, c.y() - h / 2, 1.5 * widthScale, h));
        }
        p.restore();
        p.setPen(QPen(QColor(base.red(), base.green(), base.blue(), 90), 3.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r0 + 2, r0 + 2);
        const int step = std::max(1, n / 64);
        for (int i = 0; i < n; i += step) {
            const double v = val(i);
            QColor col = base;
            col.setAlpha(205);
            capsule(p, c, ang(i, n), r0 + 3, shortS * (0.05 + v * 0.20), std::max(2.5, (4.0 + v * 7.0) * widthScale), col);
        }
    } else if (style == QLatin1String("neon_soundwave")) {
        p.setBrush(Qt::NoBrush);
        for (int k = 0; k < 3; ++k) {
            const double rr = maxR * (0.26 + k * 0.15);
            p.setPen(QPen(QColor(base.red(), base.green(), base.blue(), 68), std::max(1.0, (2.0 - k * 0.25) * widthScale)));
            p.drawEllipse(c, rr, rr);
        }
        drawCover(p, c, maxR * 0.53, maxR * 0.53);
        const int step = std::max(1, n / 72);
        for (int i = 0; i < n; i += step) {
            const double v = val(i);
            const double a = ang(i, n) + now * 0.38; // the whole mapping rotates
            QColor col = base;
            col.setAlpha(100 + int(v * 70));
            capsule(p, c, a, maxR * 0.53, shortS * (0.035 + v * 0.17), std::max(1.6, (2.0 + v * 4.0) * widthScale), col);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(base.red(), base.green(), base.blue(), 70));
            p.drawEllipse(QPointF(c.x() + std::cos(a) * maxR * 0.73, c.y() + std::sin(a) * maxR * 0.73), 2.0, 2.0);
        }
    } else if (style == QLatin1String("glow_beat_music")) {
        const double coverR = maxR * 0.46;
        drawCover(p, c, coverR, coverR);
        p.setPen(Qt::NoPen);
        for (int k = 0; k < 60; ++k) { // golden-angle spiral particles over the cover
            const double ph = std::fmod(k * 0.618 + now * 0.9, 1.0);
            const double a = k * 2.399 + now * 0.3;
            const double rr = maxR * 0.08 + ph * maxR * 0.55;
            p.setBrush(QColor(255, 255, 255, std::max(0, int((1.0 - ph) * 140))));
            p.drawEllipse(QPointF(c.x() + std::cos(a) * rr, c.y() + std::sin(a) * rr), 1.1 + ph * 1.4, 1.1 + ph * 1.4);
        }
        const int step = std::max(1, n / 84);
        for (int i = 0; i < n; i += step) {
            const double v = val(i), a = ang(i, n);
            capsule(p, c, a, coverR, shortS * (0.04 + v * 0.23), std::max(1.8, (2.5 + v * 5.0) * widthScale),
                    QColor(255, 255, 255, 82 + int(v * 120)));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 70 + int(v * 80)));
            p.drawEllipse(QPointF(c.x() + std::cos(a) * (coverR - 5), c.y() + std::sin(a) * (coverR - 5)), 1.6, 1.6);
        }
    } else if (style == QLatin1String("reactive_lights")) {
        const double centerR = maxR * 0.25;
        QRadialGradient g(c, maxR * 0.28);
        g.setColorAt(0.0, QColor(255, 255, 255, 120));
        g.setColorAt(1.0, QColor(255, 255, 255, 20));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(c, centerR, centerR);
        drawCover(p, c, centerR, centerR);
        for (int k = 0; k < 17; ++k) {
            const double v = val(k * n / 17);
            QColor col = base;
            col.setAlpha(120 + int(v * 100));
            capsule(p, c, k / 17.0 * 2.0 * M_PI - M_PI / 2.0 + now * 0.08, maxR * 0.30,
                    shortS * (0.04 + v * 0.22), std::max(1.8, (2.0 + v * 4.0) * widthScale), col);
        }
        p.setBrush(Qt::NoBrush);
        for (int ri = 0; ri < 4; ++ri) {
            p.setPen(QPen(QColor(base.red(), base.green(), base.blue(), 52 - ri * 7), 1.0));
            p.drawEllipse(c, maxR * (0.86 + ri * 0.035), maxR * (0.86 + ri * 0.035));
        }
    } else if (style == QLatin1String("electro_dubstep")) {
        // Polar wireframe mesh: mid-band focused samples, heavy angular
        // smoothing, cardinals pinned, diagonals pulled by a spring burst.
        const int S = 176;
        QVector<float> raw(S);
        for (int s = 0; s < S; ++s) {
            const double pos = s / double(S - 1);
            const int src = std::max(0, std::min(n - 1, int(n * 0.20 + pos * (n * 0.66 - n * 0.20))));
            const double cw = 1.0 - std::min(1.0, std::abs(pos - 0.50) / 0.50);
            raw[s] = float(val(src) * (0.62 + cw * 0.38));
        }
        for (int pass = 0; pass < 3; ++pass) {
            QVector<float> next(S);
            for (int s = 0; s < S; ++s)
                next[s] = raw[(s + S - 3) % S] * 0.04f + raw[(s + S - 2) % S] * 0.09f + raw[(s + S - 1) % S] * 0.22f +
                          raw[s] * 0.30f + raw[(s + 1) % S] * 0.22f + raw[(s + 2) % S] * 0.09f + raw[(s + 3) % S] * 0.04f;
            raw = next;
        }
        if (electroSmooth_.size() != S) electroSmooth_ = raw;
        double peak = 0.0, sum = 0.0;
        for (int s = 0; s < S; ++s) {
            const float cur = raw[s];
            electroSmooth_[s] = electroSmooth_[s] * (cur >= electroSmooth_[s] ? 0.40f : 0.60f) + cur * (cur >= electroSmooth_[s] ? 0.60f : 0.40f);
            peak = std::max(peak, double(electroSmooth_[s]));
            sum += electroSmooth_[s];
        }
        const double energy = std::max(0.0, std::min(1.0, peak * 0.64 + (sum / S) * 0.56));
        const double onset = std::max(0.0, std::min(1.0, (energy - electroPrevEnergy_) / 0.13));
        electroPrevEnergy_ = energy;
        double target = std::max(0.0, std::min(1.0, onset * 1.82 + std::max(0.0, std::min(1.0, (energy - 0.07) / 0.55)) * 0.18));
        if (onset < 0.08) target *= 0.28;
        if (target < 0.035) target = 0.0;
        electroBurstVel_ += (target - electroBurst_) * 0.800;
        electroBurstVel_ *= 0.455;
        electroBurst_ += electroBurstVel_;
        if (target <= 0.02) { electroBurst_ *= 0.735; electroBurstVel_ *= 0.620; }
        electroBurst_ = std::max(0.0, std::min(1.0, electroBurst_));
        auto meshR = [&](int s, double layerScale) {
            const double a = ang(s, S);
            const double pin = std::abs(std::sin(2.0 * a)); // 0 at cardinals, 1 at diagonals
            return maxR * 0.86 * layerScale * (1.0 + electroSmooth_[s] * 0.30 * (0.30 + electroBurst_) * (0.25 + 0.75 * pin));
        };
        drawCover(p, c, maxR * 0.46 * (1.0 + electroBurst_ * 0.05), maxR * 0.46 * (1.0 + electroBurst_ * 0.05));
        for (int layer = 0; layer < 5; ++layer) {
            const double ls = 0.80 + layer * 0.05;
            QColor col = QColor::fromRgbF(base.redF() + (80 / 255.0 - base.redF()) * layer / 4.0,
                                          base.greenF() + (140 / 255.0 - base.greenF()) * layer / 4.0,
                                          base.blueF() + (1.0 - base.blueF()) * layer / 4.0, 170 / 255.0);
            QPainterPath path;
            for (int s = 0; s <= S; ++s) {
                const double r = meshR(s % S, ls), a = ang(s % S, S);
                const QPointF pt(c.x() + std::cos(a) * r, c.y() + std::sin(a) * r);
                if (s == 0) path.moveTo(pt); else path.lineTo(pt);
            }
            p.setPen(QPen(col, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
        p.setPen(QPen(QColor(base.red(), base.green(), base.blue(), 90), 1.0));
        for (int s = 0; s < S; s += 12) {
            const double a = ang(s, S);
            const double r1 = meshR(s, 0.80), r2 = meshR(s, 1.0);
            p.drawLine(QPointF(c.x() + std::cos(a) * r1, c.y() + std::sin(a) * r1),
                       QPointF(c.x() + std::cos(a) * r2, c.y() + std::sin(a) * r2));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 120));
        for (int s = 0; s < S; s += 6) {
            const double r = meshR(s, 1.0), a = ang(s, S);
            p.drawEllipse(QPointF(c.x() + std::cos(a) * r, c.y() + std::sin(a) * r), 1.3, 1.3);
        }
        if (electroBurst_ > 0.25) { // cover shockwave on strong hits
            p.setPen(QPen(QColor(255, 255, 255, int(electroBurst_ * 110)), 1.5));
            p.setBrush(Qt::NoBrush);
            const double rr = maxR * (0.52 + (1.0 - electroBurst_) * 0.45);
            p.drawEllipse(c, rr, rr);
        }
    } else if (style == QLatin1String("minimal_beat")) {
        const double outer = maxR * 0.82;
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(base.red(), base.green(), base.blue(), 90), 1.2 * widthScale));
        p.drawEllipse(c, outer, outer);
        p.drawEllipse(c, outer - 5, outer - 5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(base.red(), base.green(), base.blue(), 105));
        for (int s = 0; s < 96; ++s) {
            const double v = val(s * n / 96), a = ang(s, 96);
            const double rr = outer - 2.5 + ((s % 2) ? -1.0 : 1.0) * shortS * (0.006 + v * 0.025);
            p.drawEllipse(QPointF(c.x() + std::cos(a) * rr, c.y() + std::sin(a) * rr), 1.1, 1.1);
        }
        const double innerR = maxR * 0.32;
        p.setBrush(QColor(0, 0, 0, 82));
        p.drawEllipse(c, innerR, innerR);
        const double coverR = std::max(1.0, innerR - std::min(10.0, std::max(0.0, innerR - 2.0)));
        drawCover(p, c, coverR, coverR);
        const int step = std::max(1, n / 80);
        for (int i = 0; i < n; i += step) {
            const double v = val(i);
            capsule(p, c, ang(i, n), maxR * 0.38, shortS * (0.035 + v * 0.19),
                    std::max(1.5, (2.0 + v * 4.0) * widthScale), QColor(base.red(), base.green(), base.blue(), 205));
        }
    } else if (style == QLatin1String("cosmic_fusion")) {
        const double coverR = shortS * 0.245;
        const double ringInner = coverR + std::max(4.0, shortS * 0.026);
        const double outerLimit = maxR * 1.08;
        const double maxBarLen = std::max(8.0, outerLimit - ringInner);
        QRadialGradient halo(c, outerLimit * 1.02);
        halo.setColorAt(0.0, QColor(255, 255, 255, 26));
        halo.setColorAt(0.35, QColor(base.red(), base.green(), base.blue(), 20 + int(avg_ * 22)));
        halo.setColorAt(0.74, QColor(150, 64, 255, 10 + int(bass_ * 16)));
        halo.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(c, outerLimit * 1.02, outerLimit * 1.02);
        drawCover(p, c, coverR, coverR);
        // Right/thin arc: rainbow-gradient bars (drawn first, thick cluster on top).
        const int thinCount = std::max(34, std::min(72, int(shortS * 0.24)));
        const double thinW = std::max(1.25, shortS * 0.0105 * widthScale);
        for (int j = 0; j < thinCount; ++j) {
            const double t = j / double(std::max(1, thinCount - 1));
            const double deg = 225.0 + 230.0 * t;
            const double v = val(int(t * (n - 1)));
            const double len = maxBarLen * (0.08 + v * 0.85);
            if (len <= 0.5) continue;
            p.save();
            p.translate(c);
            p.rotate(deg);
            QLinearGradient grad(QPointF(ringInner, 0), QPointF(ringInner + len, 0));
            const double phase = now * 0.085 + t * 0.6;
            for (int stop = 0; stop < 7; ++stop) {
                QColor sc = rainbowColor(phase + stop / 7.0, 217);
                sc.setHsvF(sc.hueF(), 0.88, 1.0, 217 / 255.0);
                grad.setColorAt(stop / 6.0, sc);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawRoundedRect(QRectF(ringInner, -thinW / 2, len, thinW), thinW / 2, thinW / 2);
            p.restore();
        }
        // Left/thick arc: exactly 7 flat bevel bars, one solid rainbow color each.
        const double thickW = std::max(7.0, shortS * 0.055 * widthScale);
        const double stepDeg = (thickW + 14.0) / std::max(1.0, ringInner) * 180.0 / M_PI;
        const double clusterStart = (45.0 + 90.0 + 22.0) - stepDeg * 6.0 / 2.0;
        for (int j = 0; j < 7; ++j) {
            const double t = j / 6.0;
            const double deg = clusterStart + stepDeg * j;
            const double v = val(int(t * (n - 1)));
            const double len = maxBarLen * (0.10 + v * 0.90);
            if (len <= 0.5) continue;
            const QColor col = rainbowColor(t * 0.83, 217);
            p.save();
            p.translate(c);
            p.rotate(deg);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 40));
            p.drawRect(QRectF(ringInner + thickW * 0.14, -thickW / 2 + thickW * 0.16, len, thickW));
            QLinearGradient bevel(QPointF(0, -thickW / 2), QPointF(0, thickW / 2));
            QColor topC = col.lighter(128); topC.setAlpha(217);
            QColor midC = col; midC.setAlpha(217);
            QColor botC = col.darker(145); botC.setAlpha(217);
            bevel.setColorAt(0.0, topC);
            bevel.setColorAt(0.48, midC);
            bevel.setColorAt(1.0, botC);
            p.setBrush(bevel);
            p.drawRect(QRectF(ringInner, -thickW / 2, len, thickW));
            p.restore();
        }
    } else if (style == QLatin1String("hud_equalizer")) {
        // Layered HUD (design notes): dark halo, frosted ring band with arc
        // gaps, rotating inner rings, 112 accent ticks, center dots.
        const double rx = aw * 0.44, ry = ah * 0.44;
        QRadialGradient halo(c, maxR * 1.05);
        halo.setColorAt(0.0, QColor(0, 0, 0, 95));
        halo.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(c, rx * 1.05, ry * 1.05);
        p.setBrush(Qt::NoBrush);
        const double gapPhase = std::fmod(now * 7.0, 360.0);
        for (int seg = 0; seg < 4; ++seg) { // frosted band as 4 arcs with gaps
            QPainterPath arc;
            const double a0 = (seg * 90.0 + gapPhase) * M_PI / 180.0;
            const double a1 = ((seg * 90.0 + 68.0) + gapPhase) * M_PI / 180.0;
            for (int s = 0; s <= 16; ++s) {
                const double a = a0 + (a1 - a0) * s / 16.0;
                const QPointF pt(c.x() + std::cos(a) * rx * 0.86, c.y() + std::sin(a) * ry * 0.86);
                if (s == 0) arc.moveTo(pt); else arc.lineTo(pt);
            }
            p.setPen(QPen(QColor(150, 225, 255, 120), std::max(3.0, ah * 0.085), Qt::SolidLine, Qt::RoundCap));
            p.drawPath(arc);
        }
        static const QColor ringCols[3] = {QColor(80, 255, 230, 150), QColor(120, 255, 150, 130), QColor(190, 120, 255, 130)};
        for (int k = 0; k < 3; ++k) {
            QPen pen(ringCols[k], 1.4);
            pen.setDashPattern(QVector<qreal>{10.0, 6.0});
            pen.setDashOffset(now * 20.0 * (k % 2 ? -1 : 1) * (k + 1));
            p.setPen(pen);
            p.drawEllipse(c, rx * (0.72 - k * 0.13), ry * (0.72 - k * 0.13));
        }
        for (int i = 0; i < 112; ++i) { // outer ticks use the accent color
            const double v = val(i * n / 112), a = ang(i, 112);
            const QPointF p0(c.x() + std::cos(a) * rx, c.y() + std::sin(a) * ry);
            const double len = shortS * (0.02 + v * 0.11);
            const QPointF p1(c.x() + std::cos(a) * (rx + len), c.y() + std::sin(a) * (ry + len * (ry / std::max(1.0, rx))));
            p.setPen(QPen(base, std::max(1.2, 1.8 * widthScale), Qt::SolidLine, Qt::RoundCap));
            p.drawLine(p0, p1);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 180));
        for (int d = -2; d <= 2; ++d)
            p.drawEllipse(QPointF(c.x() + d * 9, c.y()), 1.6, 1.6);
    } else if (style == QLatin1String("round_base_audio")) {
        // Approximation (no full spec read): cover on a thick base ring
        // with capsules, in the circle-family geometry of the Python build.
        const double r0 = maxR * 0.56;
        p.setPen(QPen(base.darker(140), 7.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r0 + 4, r0 + 4);
        drawCover(p, c, maxR * 0.50, maxR * 0.50);
        const int step = std::max(1, n / 72);
        for (int i = 0; i < n; i += step) {
            const double v = val(i);
            capsule(p, c, ang(i, n), r0 + 6, shortS * (0.03 + v * 0.15),
                    std::max(1.8, (2.0 + v * 4.0) * widthScale), QColor(base.red(), base.green(), base.blue(), 190));
        }
        p.setPen(QPen(QColor(255, 255, 255, 60), 2.0));
        p.drawArc(QRectF(c.x() - r0 - 9, c.y() - r0 - 9, 2 * (r0 + 9), 2 * (r0 + 9)), 200 * 16, 140 * 16);
    }
    p.restore();
}

void VisualizerWidget::addContextActions(QMenu &menu) {
    QMenu *styleMenu = menu.addMenu(tr("ビジュアライザーのスタイル"));
    static const struct { const char *label; const char *key; } styles[] = {
        {"クラシック", "classic"}, {"楕円形", "ellipse"}, {"ターンテーブル風", "turntable"},
        {"スポットライト・ビート風", "spotlight_beat"}, {"虹風", "rainbow"}, {"ミニマル風", "minimal"},
        {"ミュージックビートウォール風", "music_beat_wall"}, {"Luminance風", "luminance"},
        {"HUDオーディオイコライザ風", "hud_equalizer"}, {"Space風", "space"},
        {"フラットスペクトラム風", "flat_spectrum"}, {"Cyber風", "cyber"}, {"オーロラ風", "aurora"},
        {"ホログラム風", "hologram"}, {"オーディオリップル風", "audio_ripple"},
        {"ネオン音波風", "neon_soundwave"}, {"光彩ビート風", "glow_beat_music"},
        {"エニグマティック風", "enigmatic_echo_sound"}, {"音声反応型ライト風", "reactive_lights"},
        {"エレクトロ・ダブステップ風", "electro_dubstep"}, {"ミニマルビート風", "minimal_beat"},
        {"コズミックフュージョン風", "cosmic_fusion"}, {"Music Lower Third風", "music_lower_third_audio"},
        {"Digital Base風", "digital_base_audio"}, {"Round Base風", "round_base_audio"},
    };
    const QString cur = cfg_.vizStyle();
    for (const auto &s : styles) {
        QAction *a = styleMenu->addAction(tr(s.label));
        a->setCheckable(true);
        a->setChecked(cur == QLatin1String(s.key));
        const QString key = QString::fromLatin1(s.key);
        connect(a, &QAction::triggered, this, [this, key] {
            cfg_.setVizStyle(key);
            syncArtworkNeed(); // start/stop the media helper as needed
            update();
            emit requestSave();
        });
    }
    menu.addSeparator();
    auto addToggle = [this, &menu](const QString &label, const char *key, bool def) {
        QAction *a = menu.addAction(label);
        a->setCheckable(true);
        a->setChecked(cfg_.vizBool(key, def));
        connect(a, &QAction::triggered, this, [this, key](bool on) {
            cfg_.setVizBool(key, on);
            update();
            emit requestSave();
        });
        return a;
    };
    addToggle(tr("上下反転"), "visualizer_flip_vertical", false);
    QAction *vert = menu.addAction(tr("縦向き"));
    vert->setCheckable(true);
    vert->setChecked(cfg_.extra.value(QStringLiteral("visualizer_orientation")).toString() == QLatin1String("vertical"));
    connect(vert, &QAction::triggered, this, [this](bool on) {
        cfg_.extra["visualizer_orientation"] = on ? QStringLiteral("vertical") : QStringLiteral("horizontal");
        update();
        emit requestSave();
    });
    addToggle(tr("ピーク表示"), "visualizer_peak_bar_enabled", true);
    addToggle(tr("グロー"), "visualizer_glow_enabled", true);
    addToggle(tr("影"), "visualizer_shadow_enabled", true);
}

// ---------- Volume ----------
void VolumeWidget::onTick(const SystemSnapshot &, const QDateTime &) {
    const int v = Platform::getVolumePercent();
    const bool m = Platform::getMute();
    if (v != volume_ || m != mute_) { volume_ = v; mute_ = m; update(); }
}
void VolumeWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    const int top = rect.top() + (cfg_.title.isEmpty() ? 12 : 38);
    contentFont(p, cfg_.fontSize + 6, true);
    p.setPen(QColor(240, 244, 248));
    p.drawText(QRect(rect.left() + 10, top, rect.width() - 20, 32), Qt::AlignLeft,
               volume_ < 0 ? QStringLiteral("-- %") : mute_ ? QStringLiteral("ミュート") : QStringLiteral("%1 %").arg(volume_));
    const QRect bar(rect.left() + 10, top + 40, rect.width() - 20, 14);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 28));
    p.drawRoundedRect(bar, 6, 6);
    if (volume_ > 0 && !mute_) {
        QRect fill = bar;
        fill.setWidth(int(bar.width() * volume_ / 100.0));
        p.setBrush(accent());
        p.drawRoundedRect(fill, 6, 6);
    }
    contentFont(p, cfg_.fontSize - 4);
    p.setPen(QColor(180, 188, 198));
    p.drawText(QRect(rect.left() + 10, bar.bottom() + 4, rect.width() - 20, 16), Qt::AlignLeft,
               tr("ホイール: 音量 / クリック: ミュート切替"));
}
void VolumeWidget::onClicked(const QPoint &) {
    Platform::setMute(!Platform::getMute());
    onTick(SystemSnapshot{}, QDateTime::currentDateTime());
}
void VolumeWidget::onWheel(int delta) {
    const int cur = Platform::getVolumePercent();
    if (cur < 0) return;
    const int next = std::max(0, std::min(100, cur + (delta > 0 ? 5 : -5)));
    Platform::setVolumePercent(next);
    volume_ = next;
    update();
}

// ---------- Media ----------
// Layout ported 1:1 from the Python MediaPlayerWidget (widgets.py):
// 76x76 cover at (+14, +44), title/artist/meta lines to its right, and
// four transport buttons (prev / play-pause / next / stop) at the bottom.
// Diagnostic log for the media buttons (v0.7.3): proves whether a press
// reached the widget, which button it hit, and whether the OS accepted
// the key events. Written next to the config file in the home folder.
static void mediaDebugLog(const QString &msg) {
    // Routed through the shared debug log (v0.13.3): same file, same
    // tray-controlled on/off gate and size rotation as debugLog().
    // (Before, this writer bypassed everything and always wrote.)
    debugLog(msg);
}

MediaWidget::MediaWidget(WidgetConfig cfg) : WidgetWindow(std::move(cfg)) {
    MediaHelperClient::instance()->acquire();
    connect(MediaHelperClient::instance(), &MediaHelperClient::mediaChanged, this, [this] { update(); });
    flashTimer_.setSingleShot(true);
    connect(&flashTimer_, &QTimer::timeout, this, [this] { pressedBtn_ = -1; update(); });
    mediaDebugLog(QStringLiteral("media widget created; helper present=%1")
                      .arg(MediaHelperClient::instance()->helperPresent() ? QStringLiteral("yes") : QStringLiteral("no")));
}
MediaWidget::~MediaWidget() {
    MediaHelperClient::instance()->release();
}

void MediaWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    const MediaInfo info = MediaHelperClient::instance()->info();
    const QColor titleColor(245, 248, 255);
    const QColor subColor(210, 218, 230, 160);
    const QColor mutedColor(210, 218, 230, 115);

    const QRectF coverRect(rect.left() + 14, rect.top() + 44, 76, 76);
    const double textLeft = coverRect.right() + 14;
    const double textW = rect.right() - textLeft - 14;

    // Cover: translucent frame, then the artwork cover-fitted inside.
    p.setBrush(QColor(255, 255, 255, 24));
    p.setPen(QPen(QColor(255, 255, 255, 45), 1));
    p.drawRoundedRect(coverRect, 14, 14);
    if (!info.artwork.isNull()) {
        p.save();
        QPainterPath clip;
        clip.addRoundedRect(coverRect, 14, 14);
        p.setClipPath(clip);
        const QImage &art = info.artwork;
        const double scale = std::max(coverRect.width() / art.width(), coverRect.height() / art.height());
        const QSizeF sz(art.width() * scale, art.height() * scale);
        p.drawImage(QRectF(coverRect.center().x() - sz.width() / 2, coverRect.center().y() - sz.height() / 2, sz.width(), sz.height()), art);
        p.restore();
    } else {
        p.setFont(QFont(QStringLiteral("Segoe UI Symbol"), 28, QFont::Bold));
        p.setPen(accent());
        p.drawText(coverRect, Qt::AlignCenter, QStringLiteral("♪"));
    }

    const QString title = !info.title.isEmpty() ? info.title : QStringLiteral("No media");
    const QString line2 = !info.artist.isEmpty() ? info.artist
                        : !info.album.isEmpty() ? info.album : info.app;
    QString metaLine = info.status;
    if (!info.album.isEmpty() && !info.artist.isEmpty())
        metaLine = info.status.isEmpty() ? info.album : info.status + QStringLiteral(" / ") + info.album;
    else if (!info.app.isEmpty() && info.artist.isEmpty())
        metaLine = info.status.isEmpty() ? info.app : info.status + QStringLiteral(" / ") + info.app;

    auto elide = [&p](const QString &s, int w) {
        return p.fontMetrics().elidedText(s, Qt::ElideRight, std::max(20, w));
    };
    p.setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::Bold));
    p.setPen(titleColor);
    p.drawText(QRectF(textLeft, rect.top() + 46, textW, 24), Qt::AlignLeft | Qt::AlignVCenter, elide(title, int(textW)));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    p.setPen(subColor);
    p.drawText(QRectF(textLeft, rect.top() + 72, textW, 20), Qt::AlignLeft | Qt::AlignVCenter, elide(line2, int(textW)));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    p.setPen(mutedColor);
    p.drawText(QRectF(textLeft, rect.top() + 94, textW, 20), Qt::AlignLeft | Qt::AlignVCenter, elide(metaLine, int(textW)));

    // Transport buttons: size from height, 4 across, centered.
    const int size = std::min(42, std::max(30, int(rect.height() * 0.22)));
    const int gap = 10;
    const int totalW = size * 4 + gap * 3;
    int x = rect.left() + (rect.width() - totalW) / 2;
    const int y = rect.bottom() - size - 18;
    QRect *rects[] = {&btnPrev_, &btnPlay_, &btnNext_, &btnStop_};
    static const char *glyphs[] = {"⏮", "⏯", "⏭", "⏹"};
    p.setFont(QFont(QStringLiteral("Segoe UI Symbol"), 15, QFont::Bold));
    for (int i = 0; i < 4; ++i) {
        *rects[i] = QRect(x, y, size, size);
        if (i == pressedBtn_) { // press feedback flash
            QColor hot = accent(); hot.setAlpha(120);
            p.setBrush(hot);
            p.setPen(QPen(QColor(255, 255, 255, 160), 1));
        } else {
            p.setBrush(QColor(255, 255, 255, 24));
            p.setPen(QPen(QColor(255, 255, 255, 45), 1));
        }
        p.drawRoundedRect(*rects[i], 14, 14);
        p.setPen(accent());
        p.drawText(*rects[i], Qt::AlignCenter, QString::fromUtf8(glyphs[i]));
        x += size + gap;
    }
}
bool MediaWidget::triggerAt(const QPoint &pos) {
    const QRect *rects[] = {&btnPrev_, &btnPlay_, &btnNext_, &btnStop_};
    static const char *names[] = {"prev", "play", "next", "stop"};
    for (int i = 0; i < 4; ++i) {
        if (!rects[i]->contains(pos)) continue;
        pressedBtn_ = i;
        flashTimer_.start(180);
        update();
        static const char *cmds[] = {"prev", "play_pause", "next", "stop"};
        if (MediaHelperClient::instance()->controlHelperPresent()) {
            // Primary path: GSMTC session control via the one-shot helper.
            MediaHelperClient::instance()->sendTransportCommand(QLatin1String(cmds[i]));
            mediaDebugLog(QStringLiteral("press (%1,%2) -> %3; via control helper")
                              .arg(pos.x()).arg(pos.y()).arg(QLatin1String(names[i])));
            return true;
        }
        bool ok = false; // fallback: synthesized media keys
        switch (i) {
            case 0: ok = Platform::mediaPrevious(); break;
            case 1: ok = Platform::mediaPlayPause(); break;
            case 2: ok = Platform::mediaNext(); break;
            case 3: ok = Platform::mediaStop(); break;
        }
        mediaDebugLog(QStringLiteral("press (%1,%2) -> %3; key delivered=%4 (control helper absent)")
                          .arg(pos.x()).arg(pos.y()).arg(QLatin1String(names[i]))
                          .arg(ok ? QStringLiteral("yes") : QStringLiteral("NO")));
        return true;
    }
    mediaDebugLog(QStringLiteral("press (%1,%2) -> no button; rects prev=%3 play=%4 next=%5 stop=%6")
                      .arg(pos.x()).arg(pos.y())
                      .arg(QStringLiteral("%1,%2 %3x%4").arg(btnPrev_.x()).arg(btnPrev_.y()).arg(btnPrev_.width()).arg(btnPrev_.height()))
                      .arg(QStringLiteral("%1,%2 %3x%4").arg(btnPlay_.x()).arg(btnPlay_.y()).arg(btnPlay_.width()).arg(btnPlay_.height()))
                      .arg(QStringLiteral("%1,%2 %3x%4").arg(btnNext_.x()).arg(btnNext_.y()).arg(btnNext_.width()).arg(btnNext_.height()))
                      .arg(QStringLiteral("%1,%2 %3x%4").arg(btnStop_.x()).arg(btnStop_.y()).arg(btnStop_.width()).arg(btnStop_.height())));
    return false;
}
bool MediaWidget::onPressed(const QPoint &pos) { return triggerAt(pos); }
void MediaWidget::onClicked(const QPoint &pos) { triggerAt(pos); }

// ---------- Weather ----------
// Layout ported 1:1 from the Python WeatherWidget (widgets.py): place
// line at +34, icon/temp/description/feels block, humidity and wind
// info rows at +122, forecast cards (only when height >= 220), and the
// "更新 HH:MM:SS" stamp at the bottom right.
WeatherWidget::WeatherWidget(WidgetConfig cfg)
    : WidgetWindow(std::move(cfg)), engine_(new WeatherEngine(this)) {
    engine_->setLocation(cfg_.weatherLocation);
    connect(engine_, &WeatherEngine::updated, this, [this] { update(); });
}
void WeatherWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    const WeatherData &d = engine_->data();
    const QColor titleColor(245, 248, 255);
    const QColor subColor(210, 218, 230, 170);
    const QColor textColor(235, 240, 250, 220);
    Q_UNUSED(titleColor)

    const QString place = (d.area.isEmpty() ? (cfg_.weatherLocation.isEmpty() ? tr("現在地") : cfg_.weatherLocation) : d.area)
                        + (d.country.isEmpty() ? QString() : QStringLiteral(", ") + d.country);
    p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    p.setPen(subColor);
    p.drawText(QRectF(rect.left() + 14, rect.top() + 34, rect.width() - 28, 18), Qt::AlignLeft | Qt::AlignVCenter, place);

    if (!d.error.isEmpty()) {
        p.setFont(QFont(QStringLiteral("Segoe UI"), 9));
        p.setPen(QColor(255, 180, 120));
        p.drawText(QRectF(rect.left() + 14, rect.top() + 62, rect.width() - 28, 40), Qt::AlignLeft | Qt::AlignVCenter, tr("天気の取得に失敗しました"));
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        p.setPen(QColor(255, 210, 180, 160));
        p.drawText(QRectF(rect.left() + 14, rect.top() + 98, rect.width() - 28, 50), Qt::AlignLeft | Qt::TextWordWrap, d.error);
        return;
    }

    const double w = rect.width();
    p.setFont(QFont(QStringLiteral("Segoe UI Emoji"), 36));
    p.setPen(accent());
    p.drawText(QRectF(rect.left() + 14, rect.top() + 56, w * 0.22, 62), Qt::AlignCenter, d.icon);
    p.setFont(QFont(QStringLiteral("Segoe UI"), 34, QFont::Bold));
    p.setPen(accent());
    p.drawText(QRectF(rect.left() + w * 0.25, rect.top() + 58, w * 0.23, 56), Qt::AlignLeft | Qt::AlignVCenter, d.temperature + QStringLiteral("°"));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::Bold));
    p.setPen(textColor);
    p.drawText(QRectF(rect.left() + w * 0.50, rect.top() + 62, w * 0.46, 24), Qt::AlignLeft | Qt::AlignVCenter, d.description);
    p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    p.setPen(subColor);
    p.drawText(QRectF(rect.left() + w * 0.50, rect.top() + 88, w * 0.46, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("体感 ") + d.feelsLike + QStringLiteral("°C"));

    auto infoRow = [&](double y, const QString &label, const QString &value) {
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        p.setPen(subColor);
        p.drawText(QRectF(rect.left() + 14, y, (w - 28) * 0.45, 18), Qt::AlignLeft | Qt::AlignVCenter, label);
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));
        p.setPen(textColor);
        p.drawText(QRectF(rect.left() + 14 + (w - 28) * 0.45, y, (w - 28) * 0.55, 18), Qt::AlignRight | Qt::AlignVCenter, value);
    };
    const double infoTop = rect.top() + 122;
    infoRow(infoTop, QStringLiteral("湿度"), d.humidity + QStringLiteral("%"));
    infoRow(infoTop + 22, QStringLiteral("風速"), d.windKmph + QStringLiteral(" km/h"));

    if (!d.forecast.isEmpty() && rect.height() >= 220) {
        const double forecastTop = infoTop + 52; // +44 rows +8 gap
        p.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));
        p.setPen(subColor);
        p.drawText(QRectF(rect.left() + 14, forecastTop, w - 28, 18), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("3日予報"));
        const double cardTop = forecastTop + 22;
        const double cardW = (w - 34) / 3.0;
        for (int i = 0; i < std::min(3, int(d.forecast.size())); ++i) {
            const WeatherForecastDay &f = d.forecast.at(i);
            const QRectF card(rect.left() + 14 + i * (cardW + 3), cardTop, cardW, 56);
            p.setBrush(QColor(255, 255, 255, 20));
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(card, 10, 10);
            p.setFont(QFont(QStringLiteral("Segoe UI"), 7));
            p.setPen(subColor);
            p.drawText(QRectF(card.left() + 5, card.top() + 3, card.width() - 10, 12), Qt::AlignCenter,
                       f.date.size() >= 5 ? f.date.mid(5) : f.date);
            p.setFont(QFont(QStringLiteral("Segoe UI Emoji"), 14));
            p.setPen(accent());
            p.drawText(QRectF(card.left() + 5, card.top() + 16, card.width() - 10, 18), Qt::AlignCenter, f.icon);
            p.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Bold));
            p.setPen(textColor);
            p.drawText(QRectF(card.left() + 5, card.top() + 35, card.width() - 10, 16), Qt::AlignCenter,
                       f.minTemp + QStringLiteral("°/") + f.maxTemp + QStringLiteral("°"));
        }
    }

    if (!d.updatedAt.isEmpty()) {
        p.setFont(QFont(QStringLiteral("Segoe UI"), 7));
        p.setPen(QColor(210, 218, 230, 120));
        p.drawText(QRectF(rect.left() + 12, rect.bottom() - 20, w - 24, 14), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("更新 ") + d.updatedAt);
    }
}

// ---------- Placeholder ----------
void PlaceholderWidget::paintContent(QPainter &p, const QRect &rect) {
    drawTitle(p, rect);
    contentFont(p, cfg_.fontSize - 2);
    p.setPen(QColor(200, 208, 216));
    p.drawText(rect.adjusted(10, 34, -10, -8), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
               tr("「%1」は Lite C++版 v0.1 では未実装です。\n右クリックから削除できます。").arg(cfg_.type));
}

// ---------- Factory ----------
WidgetWindow *createWidget(const WidgetConfig &cfg, AudioEngine *audio) {
    const QString &t = cfg.type;
    if (t == QLatin1String("clock")) return new ClockWidget(cfg);
    if (t == QLatin1String("system")) return new SystemWidget(cfg);
    if (t == QLatin1String("network")) return new NetworkWidget(cfg);
    if (t == QLatin1String("calendar")) return new CalendarWidget(cfg);
    if (t == QLatin1String("html")) return new TextWidget(cfg);
    if (t == QLatin1String("visualizer")) return new VisualizerWidget(cfg, audio);
    if (t == QLatin1String("volume")) return new VolumeWidget(cfg);
    if (t == QLatin1String("media")) return new MediaWidget(cfg);
    if (t == QLatin1String("weather")) return new WeatherWidget(cfg);
    if (t == QLatin1String("effects_overlay")) return new EffectsWidget(cfg);
    if (t == QLatin1String("html_js")) return new JsHtmlWidget(cfg, audio);
    return new PlaceholderWidget(cfg); // unknown types (unknown types are ignored)
}
