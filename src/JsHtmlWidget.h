#pragma once
#include "WidgetWindow.h"
#include "SystemMonitor.h"
#include "JsHtmlPluginApi.h"
#include <QLibrary>
#include <QTimer>

// Host side of the JSHTML widget (v0.14.0). This class is always
// compiled and never touches WebEngine headers: it lazy-loads the
// LdsJsHtml plugin the first time an html_js widget is created and
// embeds the plugin's QWebEngineView as a child covering the whole
// window. Without the plugin (a build/machine without WebEngine) it
// degrades to an explanatory placeholder, like the Python build did
// when its WebEngine import failed.
//
// Host responsibilities (mirroring the Python JSHtmlViewManager):
// field defaults + config persistence, per-second state pushes
// (system snapshot + audio spectrum for the LDS API), config/package
// change detection -> plugin reload, edit-mode mouse transparency,
// right-click forwarding back into WidgetWindow::showContextMenu,
// and the package actions (ZIP import / open folder / back to
// inline) the Python build had in its Studio panel.
class JsHtmlWidget : public WidgetWindow {
    Q_OBJECT
public:
    JsHtmlWidget(WidgetConfig cfg, AudioEngine *audio);
    ~JsHtmlWidget() override;

    void onTick(const SystemSnapshot &s, const QDateTime &now) override;
    void onEditModeChanged() override;

    // Why the plugin is unavailable (QLibrary's error text or the
    // missing symbol names); empty when it loaded fine. Shown in
    // the placeholder so a failed load diagnoses itself (v0.14.3).
    static QString pluginStatus();

protected:
    void paintContent(QPainter &p, const QRect &rect) override;
    void resizeEvent(QResizeEvent *) override;
    void addContextActions(QMenu &menu) override;

private:
    struct Plugin {
        QLibrary lib;
        LdsJsCreateFn create = nullptr;
        LdsJsReloadFn reload = nullptr;
        LdsJsPushStateFn pushState = nullptr;
        LdsJsSetMouseTransparentFn setMouseTransparent = nullptr;
        bool ok = false;
        QString status;
    };
    static Plugin &plugin();
    static void contextMenuThunk(void *ctx, int globalX, int globalY);

    QByteArray cfgJson() const;
    void reloadPlugin();
    void pushState(bool audioOnly);
    QString widgetDirPath() const { return widgetDir_; }

    AudioEngine *audio_;
    QWidget *view_ = nullptr;
    // Fast audio push (v0.14.8): the 1 Hz onTick state push made
    // audio-reactive pages sluggish — the sample bars poll
    // LDS.getAudioSpectrum() every 50 ms, but that API answers from
    // the last pushed state, so they effectively got 1 fps data.
    // While the audio engine is running, push audio-only state at
    // 20 fps (the native visualizer's rate); the timer stops when
    // audio stops, so idle cost stays zero.
    QTimer audioTimer_;
    SystemSnapshot lastSnapshot_;
    QString widgetDir_;
    QByteArray lastCfgJson_;
    bool fieldsChanged_ = false;
};
