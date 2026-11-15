#pragma once
#include "WidgetConfig.h"
#include <QWidget>

struct SystemSnapshot;
class AudioEngine;

// One widget = one small top-level window. This replaces the original
// full-screen canvas repainted at 45fps: a widget only repaints its own
// rectangle, and only when its displayed content actually changed.
class WidgetWindow : public QWidget {
    Q_OBJECT
public:
    explicit WidgetWindow(WidgetConfig cfg, QWidget *parent = nullptr);
    ~WidgetWindow() override = default;

    const WidgetConfig &config() const { return cfg_; }
    WidgetConfig &config() { return cfg_; }
    QString type() const { return cfg_.type; }

    void setEditMode(bool on);
    bool editMode() const { return editMode_; }
    // Selection for keyboard actions (v0.15.0): in edit mode the
    // last-pressed widget is "selected" (accent frame instead of the
    // dashed one) and is what Delete removes. Selection is a purely
    // visual state; AppController owns which widget is selected.
    void setSelected(bool on) { selected_ = on; update(); }
    bool selected() const { return selected_; }
    // Layer band for this widget (v0.9.10, the user's proposal): normal
    // widgets can live in the TOPMOST band so nothing — shell, apps, or
    // z-order reshuffles — can ever cover or blink them; the effects
    // overlay always stays in the bottom band. Changing flags on a
    // shown window recreates the native window, so this re-shows it.
    void applyLayerFlags(bool widgetsOnTop);
    // Background-only transparency (v0.13.1): repaints with the
    // current cfg_.bgAlpha (the background color's alpha) and
    // optionally persists it. The native window is never faded —
    // content stays fully opaque.
    void applyBgAlpha(bool save);
    // Shared transparency UI (used by the normal right-click menu and
    // the 3D preview's): a preset submenu plus a slider dialog. Both
    // drive bgAlpha only. Widgets without a background panel (the
    // effects overlay, whose scene IS its content) get no menu.
    void addOpacityMenu(class QMenu &menu);
    void openOpacityDialog();
    // The widget's right-click menu, callable from outside the widget
    // itself: the JSHTML plugin view swallows native context-menu
    // events inside Chromium, so it forwards right-clicks here to get
    // exactly the same menu as every other widget (v0.14.0).
    void showContextMenu(const QPoint &globalPos);

    // Called once per second from the shared central tick.
    virtual void onTick(const SystemSnapshot &, const class QDateTime &) {}
    // Called when edit mode toggles (to refresh borders etc).
    virtual void onEditModeChanged() { update(); }

signals:
    void requestSave();
    void requestRemove(WidgetWindow *w);
    void requestToggleEdit();
    void requestQuit();
    // Emitted on any left press so AppController can track the
    // selected widget for the Delete key (v0.15.0).
    void requestSelect(WidgetWindow *w);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    // Subclasses draw their content inside the rounded panel.
    virtual void paintContent(class QPainter &p, const QRect &rect) = 0;
    // Subclasses may handle clicks outside edit mode (media / volume).
    virtual void onClicked(const QPoint &) {}
    // Press-time hook, checked BEFORE edit-mode drag handling. Return
    // true when the press hit an interactive control: the action fires
    // immediately (like the Python build, whose media buttons work on
    // mousePress even in edit mode) and no drag/click follows.
    virtual bool onPressed(const QPoint &) { return false; }
    virtual void onWheel(int) {}
    // Subclasses may add their own entries to the right-click menu.
    virtual void addContextActions(class QMenu &) {}

    void wheelEvent(class QWheelEvent *) override;
    void drawPanel(QPainter &p, const QRect &rect) const;
    void drawTitle(QPainter &p, const QRect &rect) const;
    QColor accent() const { return QColor(cfg_.color); }

    WidgetConfig cfg_;
private:
    bool editMode_ = false;
    bool selected_ = false;
    bool dragging_ = false;
    bool resizing_ = false;
    QPoint dragOffset_;
    QRect startGeom_;
    QPoint pressPos_;
    bool moved_ = false;
    bool pressHandled_ = false;
};
