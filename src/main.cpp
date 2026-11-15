// LiteDesktopStudio C++ (Lite edition) v0.1
// Design rules for low CPU / low memory:
//  - Qt Widgets only. No WebEngine (Chromium), no OpenGL, no 3rd-party libs.
//  - No full-screen canvas: each widget is its own small window and repaints
//    only its own rectangle, only when its content changed.
//  - One shared 1-second timer drives clock/system/network/calendar/volume.
//    The visualizer is the only widget with its own fast timer, and it skips
//    repaints while the audio is silent.
#include "AudioEngine.h"
#include "EffectsOverlay.h"
#include "Platform.h"
#include "Preview3D.h"
#include "SystemMonitor.h"
#include "WidgetConfig.h"
#include "Widgets.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QLineEdit>
#include <QLockFile>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QSpinBox>
#include <QStyle>
#include <QTextEdit>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#include <QAbstractNativeEventFilter>

// Global key watch for the edit-mode shortcuts (v0.15.0). The
// widget windows never take keyboard focus (WA_ShowWithoutActivating),
// so ordinary Qt key events never reach them; a WH_KEYBOARD_LL hook
// is the only way to see "E" / Delete. The hook is observe-only —
// it never swallows a key — and it is deliberately picky about the
// context (see AppController::onShortcutKey): a low-level hook sees
// EVERY keystroke on the machine, so the guards are the feature.
// Installed once, for the process lifetime; costs one comparison
// per keystroke.
class ShortcutKeyFilter : public QAbstractNativeEventFilter {
public:
    // A plain function pointer (set in main) keeps the hook free of
    // any dependency on AppController's full definition.
    static inline void (*onKey)(int vk) = nullptr;
    static inline HHOOK hook = nullptr;

    static LRESULT CALLBACK proc(int code, WPARAM wParam, LPARAM lParam) {
        if (code == HC_ACTION && onKey && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            const auto *kb = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lParam);
            if (kb->vkCode == 'E' || kb->vkCode == VK_DELETE) onKey(int(kb->vkCode));
        }
        return CallNextHookEx(hook, code, wParam, lParam);
    }

    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override { return false; }

    static void install() {
        hook = SetWindowsHookExW(WH_KEYBOARD_LL, &ShortcutKeyFilter::proc,
                                 GetModuleHandleW(nullptr), 0);
    }
};
#endif

class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr) : QObject(parent) {
        configPath_ = QDir::homePath() + QStringLiteral("/LiteDesktopStudio_config.json");
        loadConfig();
        // The audio engine feeds the visualizer and the JSHTML
        // Audio API (getAudioSpectrum); start it for either consumer.
        const bool needsAudio = std::any_of(widgets_.begin(), widgets_.end(),
            [](WidgetWindow *w) {
                return w->type() == QLatin1String("visualizer") ||
                       w->type() == QLatin1String("html_js");
            });
        if (needsAudio) audio_.start();

        tick_.setInterval(1000);
        connect(&tick_, &QTimer::timeout, this, [this] {
            monitor_.sample();
            const QDateTime now = QDateTime::currentDateTime();
            for (WidgetWindow *w : widgets_) w->onTick(monitor_.snapshot(), now);
            enforceLayers();
        });
        tick_.start();
        // Layer enforcement (v0.8.2): the effects overlay belongs beneath
        // every widget; without enforcement, activating a widget reshuffles
        // WindowStaysOnBottomHint windows and the overlay can end up on top,
        // making the other widgets look "gone".
        layerTimer_.setInterval(750);
        connect(&layerTimer_, &QTimer::timeout, this, [this] { enforceLayers(); });
        layerTimer_.start();
        // Activation is what reshuffles bottom windows on Windows; restack
        // immediately when focus moves instead of waiting for the timer.
        connect(qApp, &QGuiApplication::focusWindowChanged, this,
                [this](QWindow *) { enforceLayers(); });
        for (QScreen *s : QGuiApplication::screens())
            connect(s, &QScreen::geometryChanged, this, [this] { syncHostedGeometry(); reclampEffects(); });
        connect(qApp, &QGuiApplication::screenAdded, this, [this] { syncHostedGeometry(); reclampEffects(); });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] { syncHostedGeometry(); reclampEffects(); });
        // Prime immediately so widgets do not sit blank for the first second.
        monitor_.sample();
        const QDateTime now = QDateTime::currentDateTime();
        for (WidgetWindow *w : widgets_) w->onTick(monitor_.snapshot(), now);
    }
    ~AppController() override { audio_.stop(); }

    // 3D preview (v0.11.0): one on-demand window, created lazily.
    void openPreview3D() {
        if (!preview3d_) {
            preview3d_ = new Preview3DWindow(&widgets_);
            preview3d_->setAttribute(Qt::WA_DeleteOnClose);
            connect(preview3d_, &Preview3DWindow::requestSave, this, [this] { saveConfig(); });
            connect(preview3d_, &Preview3DWindow::requestAddWidget, this,
                    [this](const QString &type) { addWidget(type); });
            connect(preview3d_, &Preview3DWindow::requestRemoveWidget, this,
                    [this](WidgetWindow *target) {
                        widgets_.removeOne(target);
                        hosted_.remove(target);
                        safeClamped_.remove(target);
                        target->deleteLater();
                        saveConfig();
                    });
            connect(preview3d_, &QObject::destroyed, this, [this] { preview3d_ = nullptr; });
        }
        preview3d_->show();
        preview3d_->raise();
        preview3d_->activateWindow();
    }

    // Dev hook (offscreen verification): open the effects editor.
    void debugOpenEffectsEditor() {
        for (WidgetWindow *w : widgets_)
            if (w->type() == QLatin1String("effects_overlay")) {
                static_cast<EffectsWidget *>(w)->openEffectsEditor();
                return;
            }
    }

    void setupTray() {
        tray_.setIcon(QIcon(QStringLiteral(":/icon.png")));
        tray_.setToolTip(QStringLiteral("Ultra Lite Desktop Studio"));
        auto *menu = new QMenu;
        editAct_ = menu->addAction(tr("編集モード (移動/リサイズ)"));
        editAct_->setCheckable(true);
        connect(editAct_, &QAction::toggled, this, [this](bool on) { setEditMode(on); });
        // Desktop hosting is experimental and OFF by default (v0.8.4):
        // v0.8.2/v0.8.3 enabled it unconditionally and it misbehaved on
        // the user's machine. The toggle persists as "desktop_host".
        hostAct_ = menu->addAction(tr("デスクトップに埋め込む (実験的)"));
        hostAct_->setCheckable(true);
        hostAct_->setChecked(hostEnabled_);
        connect(hostAct_, &QAction::toggled, this, [this](bool on) {
            hostEnabled_ = on;
            hostGaveUp_ = false;
            hostTries_ = 0;
            hostCycles_ = 0;
            debugLog(QStringLiteral("host: toggled %1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
            refreshHosting();
            saveConfig();
        });
        // Widgets in the topmost band (v0.9.10): the user's proposal to
        // end the z-order flicker for good. Default ON; the trade-off is
        // that widgets also float above application windows.
        onTopAct_ = menu->addAction(tr("ウィジェットを最前面に表示"));
        onTopAct_->setCheckable(true);
        onTopAct_->setChecked(widgetsOnTop_);
        connect(onTopAct_, &QAction::toggled, this, [this](bool on) {
            widgetsOnTop_ = on;
            for (WidgetWindow *w : widgets_) w->applyLayerFlags(on);
            saveConfig();
        });
        // Debug log toggle (v0.13.3, the user's request): the debug
        // log is OFF unless this is checked — some writers are chatty
        // (the icon layer logs a line per 1.5 s cache fill), so an
        // always-on log had already reached ~1 MB and would keep
        // growing for as long as the app runs. Persisted as the
        // config root key "debug_log".
        debugLogAct_ = menu->addAction(tr("デバッグログをオン"));
        debugLogAct_->setCheckable(true);
        debugLogAct_->setChecked(debugLogEnabled_);
        connect(debugLogAct_, &QAction::toggled, this, [this](bool on) {
            if (!on) debugLog(QStringLiteral("log: debug log disabled from the tray"));
            debugLogEnabled_ = on;
            setDebugLogEnabled(on);
            if (on) debugLog(QStringLiteral("log: debug log enabled from the tray"));
            saveConfig();
        });
        auto *addMenu = menu->addMenu(tr("ウィジェットを追加"));
        static const struct { const char *type; const char *label; } kinds[] = {
            {"clock", "時計"}, {"system", "システム"}, {"network", "通信"},
            {"calendar", "カレンダー"}, {"visualizer", "音楽Spectrum"},
            {"volume", "音量"}, {"media", "メディア"}, {"html", "テキスト"},
            {"weather", "天気"}, {"effects_overlay", "エフェクト"},
            {"html_js", "JSHTML"},
        };
        for (const auto &k : kinds) {
            QAction *a = addMenu->addAction(QString::fromUtf8(k.label));
            const QString type = QString::fromLatin1(k.type);
            connect(a, &QAction::triggered, this, [this, type] { addWidget(type); });
        }
        menu->addSeparator();
        connect(menu->addAction(tr("3Dプレビュー")), &QAction::triggered, this, [this] { openPreview3D(); });
        connect(menu->addAction(tr("設定を保存")), &QAction::triggered, this, [this] { saveConfig(); });
        connect(menu->addAction(tr("終了")), &QAction::triggered, this, [] { QApplication::quit(); });
        tray_.setContextMenu(menu);
        if (QSystemTrayIcon::isSystemTrayAvailable()) tray_.show();
    }

    // ---- Keyboard shortcuts (v0.15.0, the user's request) ----
    // "E" toggles edit mode; in edit mode Delete removes the
    // selected widget (the one last pressed; its frame turns solid).
    // Plain E is also a typing key, so the hook only acts when the
    // key press belongs to the desktop context: no modifier held,
    // the foreground window is one of ours / the desktop / the
    // taskbar, and no text input is focused there. A press that
    // fails any check is left completely alone.
    Q_INVOKABLE void shortcutKeyFromHook(int vk) {
#ifdef Q_OS_WIN
        // No modifiers (Ctrl+E, Alt+E, Win+E belong to other apps).
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000 || GetAsyncKeyState(VK_MENU) & 0x8000 ||
            GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000)
            return;
        HWND fg = GetForegroundWindow();
        if (fg) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid != GetCurrentProcessId()) {
                // Another process is foreground: only the desktop
                // shell and the taskbar count as "our" context.
                wchar_t cls[64] = {};
                GetClassNameW(fg, cls, 64);
                const QString name = QString::fromWCharArray(cls);
                if (name != QStringLiteral("Progman") && name != QStringLiteral("WorkerW") &&
                    name != QStringLiteral("Shell_TrayWnd") &&
                    name != QStringLiteral("Shell_SecondaryTrayWnd"))
                    return;
            } else {
                // One of our own windows is foreground: if a text
                // input has focus there, the key is typing, not a
                // shortcut (config dialogs, the effects editor...).
                QWidget *fw = QApplication::focusWidget();
                if (fw && (qobject_cast<QLineEdit *>(fw) || qobject_cast<QTextEdit *>(fw) ||
                           qobject_cast<QPlainTextEdit *>(fw) || qobject_cast<QSpinBox *>(fw) ||
                           qobject_cast<QComboBox *>(fw)))
                    return;
            }
        }
#endif
        onShortcutKey(vk);
    }

    void onShortcutKey(int vk) {
        if (vk == 'E') {
            setEditMode(!editMode_);
        } else if (vk == 0x2E /* VK_DELETE */ && editMode_ && selectedWidget_) {
            WidgetWindow *target = selectedWidget_;
            selectedWidget_ = nullptr;
            widgets_.removeOne(target);
            hosted_.remove(target);
            safeClamped_.remove(target);
            target->deleteLater();
            saveConfig();
        }
    }

    void setEditMode(bool on) {
        editMode_ = on;
        if (!on && selectedWidget_) {
            selectedWidget_->setSelected(false);
            selectedWidget_ = nullptr;
        }
        for (WidgetWindow *w : widgets_) {
            if (on && safeClamped_.contains(w)) {
                // Editing works on the user's real (full) geometry; the
                // work-area clamp is a display-time constraint only.
                safeClamped_.remove(w);
                const WidgetConfig &c = w->config();
                w->setGeometry(c.x, c.y, c.w, c.h);
            }
            w->setEditMode(on);
            if (!on) applySafeGeometry(w);
        }
        refreshHosting();
        if (editAct_ && editAct_->isChecked() != on) editAct_->setChecked(on);
        if (!on) saveConfig();
    }

    // Python's lds_safe_desktop_geometry, ported (v0.9.7): the
    // fullscreen effects canvas is drawn inside the screen's WORK AREA
    // and never covers the taskbar — in the Python era the taskbar kept
    // vanishing under the canvas until it was excluded, and the same
    // overlap is what makes Shell_TrayWnd flicker here when the desktop
    // is clicked after an application. The clamped geometry is
    // runtime-only: saveConfig() skips it (like hosted geometry), so
    // the config keeps the user's full-screen size. Opt out with
    // LDS_ALLOW_TASKBAR_COVER=1 (Python's env of the same name).
    void applySafeGeometry(WidgetWindow *w) {
        if (!w || w->type() != QLatin1String("effects_overlay")) return;
        if (qEnvironmentVariableIntValue("LDS_ALLOW_TASKBAR_COVER") == 1) return;
        QScreen *s = QGuiApplication::screenAt(w->geometry().center());
        if (!s) s = QGuiApplication::primaryScreen();
        if (!s) return;
        const QRect clamped = w->geometry().intersected(s->availableGeometry());
        if (clamped != w->geometry() && clamped.width() > 50 && clamped.height() > 50) {
            w->setGeometry(clamped);
            safeClamped_.insert(w);
        }
    }

    void addWidget(const QString &type) {
        WidgetConfig cfg;
        cfg.type = type;
        cfg.title = type;
        cfg.x = 140; cfg.y = 140;
        if (type == QLatin1String("clock") || type == QLatin1String("calendar")) { cfg.w = 240; cfg.h = 240; }
        if (type == QLatin1String("clock")) cfg.bg = QStringLiteral("#1a1a1a"); // dial default (v0.13.0)
        else if (type == QLatin1String("visualizer")) { cfg.w = 560; cfg.h = 190; }
        else if (type == QLatin1String("html_js")) { cfg.w = 340; cfg.h = 220; }
        else if (type == QLatin1String("effects_overlay")) {
            cfg.w = 1280; cfg.h = 720;
            // Seed preset: rain + ripples (Python's rain_ripples) with moon.
            // Other keep-systems are enabled by editing effects_json in the
            // config file; see README.
            cfg.extra[QStringLiteral("effects_json")] = QString::fromLatin1(
                "{\"rain_enabled\":true,\"rain_ripple_enabled\":true,"
                "\"moon_body_enabled\":true,\"moonlight_enabled\":true}");
        }
        else { cfg.w = 320; cfg.h = 150; }
        attach(createWidget(cfg, &audio_));
        widgets_.back()->show();
        if (type == QLatin1String("visualizer") || type == QLatin1String("html_js"))
            audio_.start();
        saveConfig();
    }

    void saveConfig() {
        // Never persist an empty widget list over a real config: an empty
        // list means internal state is incomplete, not that the user
        // deleted everything (deleting the last widget is not offered).
        if (widgets_.isEmpty()) return;
        QJsonObject root = rootExtra_;
        root[QStringLiteral("desktop_host")] = hostEnabled_;
        root[QStringLiteral("widgets_on_top")] = widgetsOnTop_;
        root[QStringLiteral("debug_log")] = debugLogEnabled_;
        QJsonArray arr;
        for (WidgetWindow *w : widgets_) {
            WidgetConfig &c = w->config();
            if (!hosted_.contains(w) && !safeClamped_.contains(w)) {
                // hosted geometry is the virtual screen and clamped
                // geometry is the work area; neither is the user's size.
                c.x = w->x(); c.y = w->y(); c.w = w->width(); c.h = w->height();
            }
            arr.append(c.toJson());
        }
        root[QStringLiteral("widgets")] = arr;
        // QSaveFile writes to a temp file and atomically replaces the
        // config on commit, so a crash mid-save cannot truncate it.
        QSaveFile f(configPath_);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            f.commit();
        }
    }

private:
    void attach(WidgetWindow *w) {
        connect(w, &WidgetWindow::requestSave, this, [this] { refreshHosting(); saveConfig(); });
        if (w->type() == QLatin1String("effects_overlay"))
            connect(static_cast<EffectsWidget *>(w), &EffectsWidget::requestOpenPreview3D,
                    this, [this] { openPreview3D(); });
        connect(w, &WidgetWindow::requestToggleEdit, this, [this] { setEditMode(!editMode_); });
        connect(w, &WidgetWindow::requestSelect, this, [this](WidgetWindow *target) {
            // The effects overlay never becomes the selection
            // (v0.15.1): it covers the whole screen, so an ordinary
            // desktop click would select it and a following Delete
            // would remove it. Clicking it just clears the selection.
            if (target && target->type() == QLatin1String("effects_overlay")) target = nullptr;
            if (selectedWidget_ && selectedWidget_ != target) selectedWidget_->setSelected(false);
            selectedWidget_ = target;
            if (target) target->setSelected(editMode_);
        });
        connect(w, &WidgetWindow::requestQuit, this, [] { QApplication::quit(); });
        connect(w, &WidgetWindow::requestRemove, this, [this](WidgetWindow *target) {
            if (selectedWidget_ == target) selectedWidget_ = nullptr;
            widgets_.removeOne(target);
            hosted_.remove(target);
            target->deleteLater();
            saveConfig();
        });
        // Append BEFORE setEditMode: mode hooks may emit signals, and a
        // save triggered from here must already see the full widget list.
        // (v0.8.2 regression: a save during attach wrote a truncated
        // config, which the next launch then "repaired" with defaults.)
        widgets_.append(w);
        w->applyLayerFlags(widgetsOnTop_);
        w->setEditMode(editMode_);
        if (!editMode_) applySafeGeometry(w);
    }

    void reclampEffects() {
        if (editMode_) return;
        for (WidgetWindow *w : widgets_) {
            if (!safeClamped_.contains(w)) continue;
            // Restore the config geometry first so the clamp is computed
            // from the user's size, not from the previous clamp.
            const WidgetConfig &c = w->config();
            w->setGeometry(c.x, c.y, c.w, c.h);
            safeClamped_.remove(w);
            applySafeGeometry(w);
        }
    }

    // ---- Layer management (v0.8.2) ----
    // Keeps the effects overlay beneath all widgets. Scenic overlays are
    // hosted by the desktop WorkerW (part of the wallpaper, below the
    // desktop icons); everything else is restacked in config order with
    // the overlay pinned to the very bottom of the window stack.
    void enforceLayers() {
        for (WidgetWindow *w : widgets_) {
            const bool isEffects = w->type() == QLatin1String("effects_overlay");
            if (!isEffects || editMode_) continue;
            auto *fx = static_cast<EffectsWidget *>(w);
            if (!fx->wantsDesktopHost() || !hostEnabled_ || hostGaveUp_) continue;
            if (hosted_.contains(w)) {
                // If Explorer or Qt dropped the parenting, fall back to a
                // normal window instead of staying invisible. Losing the
                // parent twice means something keeps undoing the hosting:
                // give up for this session rather than cycling attach /
                // detach forever (v0.8.3 froze the desktop that way — the
                // cycle burned the UI thread in reparenting calls).
                if (DesktopHost::parentOf(reinterpret_cast<quintptr>(w->winId())) == 0) {
                    hosted_.remove(w);
                    ++hostCycles_;
                    debugLog(QStringLiteral("host: parent lost (cycle %1)").arg(hostCycles_));
                    if (hostCycles_ >= 2) {
                        hostGaveUp_ = true;
                        debugLog(QStringLiteral("host: gave up for this session"));
                    }
                    w->show();
                }
                continue;
            }
            // Pace attempts: at most one per 5 s, at most 6 per session.
            if (hostTries_ >= 6) continue;
            if (!hostLastTry_.isNull() && hostLastTry_.msecsTo(QDateTime::currentDateTime()) < 5000) continue;
            hostLastTry_ = QDateTime::currentDateTime();
            ++hostTries_;
            w->winId(); // realize the native handle before reparenting
            if (DesktopHost::attach(reinterpret_cast<quintptr>(w->winId()))) {
                hosted_.insert(w);
                syncHostedGeometry();
            } else {
                w->show(); // attach() leaves the window parentless on failure
            }
        }
        if (editMode_) return;
#ifdef Q_OS_WIN
        // Enforce ONLY the invariant that matters (v0.9.8): every
        // effects overlay stays behind every normal widget. Earlier
        // versions restacked the whole set into the config's layer
        // order, but touching a widget shuffles it within the bottom
        // band, and re-seating it with SetWindowPos blanked the touched
        // widget for a frame (user report on v0.9.7: widgets blink out
        // and back when touched). Widget-vs-widget order is now left
        // wherever the system puts it — the gadgets rarely overlap and
        // the user arranges them in edit mode. Only the overlay is ever
        // moved, and only when it actually sits above some widget; its
        // constant repaint hides the one-frame cost.
        QList<HWND> normals, effects;
        for (WidgetWindow *w : widgets_) {
            const HWND h = reinterpret_cast<HWND>(w->winId());
            if (w->type() == QLatin1String("effects_overlay")) {
                if (!hosted_.contains(w)) effects.append(h);
            } else {
                normals.append(h);
            }
        }
        if (!normals.isEmpty() && !effects.isEmpty()) {
            const QSet<HWND> normalSet(normals.begin(), normals.end());
            const QSet<HWND> effectSet(effects.begin(), effects.end());
            HWND lowestNormal = nullptr;
            int seenNormals = 0;
            QList<HWND> violating;
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
                if (normalSet.contains(h)) {
                    ++seenNormals;
                    lowestNormal = h;
                } else if (effectSet.contains(h) && seenNormals < normals.size()) {
                    violating.append(h); // a widget still sits below this overlay
                }
            }
            for (HWND fx : violating) {
                if (SetWindowPos(fx, lowestNormal, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
                    debugLog(QStringLiteral("layers: effects overlay re-seated behind widgets"));
                else
                    debugLog(QStringLiteral("layers: SetWindowPos failed err=%1").arg(GetLastError()));
            }
        }
        // Gadget-layer repair (v0.9.11): normal widgets are plain
        // windows meant to sit directly above the desktop shell and
        // below application windows. That arrangement maintains itself
        // (apps open above, clicks never move a no-activate window),
        // EXCEPT when Explorer restarts and the new shell window lands
        // above a widget. Only then: lift the widget back to directly
        // above the shell — a rare one-shot repair, not a restack.
        if (HWND shell = GetShellWindow()) {
            const QSet<HWND> normalSet(normals.begin(), normals.end());
            HWND aboveShell = nullptr;
            bool pastShell = false;
            QList<HWND> belowShell;
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
                if (h == shell) { pastShell = true; continue; }
                if (!pastShell) aboveShell = h; // last window seen above the shell
                else if (normalSet.contains(h)) belowShell.append(h);
            }
            for (int i = belowShell.size() - 1; i >= 0; --i) {
                const HWND anchor = aboveShell ? aboveShell : HWND_TOP;
                if (SetWindowPos(belowShell[i], anchor, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
                    debugLog(QStringLiteral("layers: widget lifted above the shell"));
                    aboveShell = belowShell[i];
                }
            }
        }
        // One-shot measurement after each restack round (v0.9.2): the
        // v0.9.1 log taught us the restack itself was INVERTING the order
        // — DeferWindowPos insertAfter semantics put each next window
        // behind the previous one, ending with effects at the FRONT —
        // and that Qt re-raises a StaysOnBottom window the moment it is
        // placed at HWND_BOTTOM below the desktop shell. The truth is in
        // the log line written by the delayed check below.
        QTimer::singleShot(120, this, [this] {
            QStringList order;
            for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
                for (WidgetWindow *w : widgets_) {
                    if (reinterpret_cast<HWND>(w->winId()) == h) {
                        order.append(w->type() == QLatin1String("effects_overlay")
                                         ? QStringLiteral("effects")
                                         : w->type());
                        break;
                    }
                }
            }
            const QString sig = order.join(QStringLiteral(">"));
            if (sig != lastZOrder_) {
                lastZOrder_ = sig;
                debugLog(QStringLiteral("layers: z-order front-to-back: %1").arg(sig));
            }
        });
#else
        // Qt-only approximation elsewhere: lower the overlay, then raise
        // the rest in config order (raise() restacks, it does not focus).
        for (WidgetWindow *w : widgets_) {
            if (w->type() == QLatin1String("effects_overlay")) { w->lower(); break; }
        }
#endif
    }

    // Attach/detach the scenic overlay when entering/leaving edit mode:
    // editing needs a normal draggable window, scenic mode wants the host.
    void refreshHosting() {
        for (WidgetWindow *w : widgets_) {
            if (w->type() != QLatin1String("effects_overlay")) continue;
            if ((editMode_ || !hostEnabled_) && hosted_.contains(w)) {
                DesktopHost::detach(reinterpret_cast<quintptr>(w->winId()));
                hosted_.remove(w);
                // Back to the config geometry as a normal window.
                const WidgetConfig &c = w->config();
                w->setGeometry(c.x, c.y, std::max(40, c.w), std::max(30, c.h));
                w->show();
                w->lower();
            }
        }
        enforceLayers();
    }

    // While hosted, the overlay must cover the whole virtual screen.
    // Use native calls on Windows: Qt geometry calls on an externally
    // reparented window make Qt re-assert top-level placement.
    void syncHostedGeometry() {
        QRect v;
        for (QScreen *s : QGuiApplication::screens()) v = v.united(s->geometry());
        if (v.isEmpty()) return;
        for (WidgetWindow *w : hosted_) {
#ifdef Q_OS_WIN
            SetWindowPos(reinterpret_cast<HWND>(w->winId()), HWND_TOP,
                         v.x(), v.y(), v.width(), v.height(),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
#else
            w->setGeometry(v);
#endif
        }
    }

    void loadConfig() {
        QFile f(configPath_);
        QJsonObject root;
        if (f.open(QIODevice::ReadOnly)) {
            root = QJsonDocument::fromJson(f.readAll()).object();
        }
        rootExtra_ = root;
        rootExtra_.remove(QStringLiteral("widgets"));
        // Hosting (v0.8.x experiment) is a dead end — the Python build had
        // already abandoned WorkerW hosting — so it is NOT auto-applied
        // from the config any more (v0.9.1): a persisted desktop_host=true
        // from the v0.8.4 tests must not silently re-enable it. The tray
        // toggle still turns it on for the current session only.
        hostEnabled_ = false;
        // Topmost band for widgets (v0.9.10) is opt-in since v0.9.11:
        // the default is now the plain gadget layer just below app
        // windows, which the user asked for once the flicker was gone.
        widgetsOnTop_ = root.value(QStringLiteral("widgets_on_top")).toBool(false);
        // Debug log gate (v0.13.3): applied BEFORE any widget is
        // created below, so construction-time writers (media widget,
        // icon layer) already honor it. Default off.
        debugLogEnabled_ = root.value(QStringLiteral("debug_log")).toBool(false);
        setDebugLogEnabled(debugLogEnabled_);
        const QJsonArray arr = root.value(QStringLiteral("widgets")).toArray();
        if (arr.isEmpty()) {
            // Same default layout as the Python build's first run.
            auto def = [](const char *type, int x, int y, int w, int h, const QString &title) {
                WidgetConfig c; c.type = QString::fromLatin1(type);
                c.x = x; c.y = y; c.w = w; c.h = h; c.title = title;
                c.bg = QStringLiteral("#10141C");
                return c;
            };
            attach(createWidget(def("visualizer", 120, 120, 560, 190, QStringLiteral("音楽Spectrum")), &audio_));
            attach(createWidget(def("clock", 820, 120, 240, 240, QStringLiteral("時計")), &audio_));
            attach(createWidget(def("network", 820, 390, 320, 150, QStringLiteral("通信")), &audio_));
            attach(createWidget(def("system", 120, 340, 340, 150, QStringLiteral("システム")), &audio_));
            attach(createWidget(def("volume", 500, 340, 250, 150, QStringLiteral("音量")), &audio_));
            attach(createWidget(def("media", 1180, 450, 320, 160, QStringLiteral("メディアコントローラー")), &audio_));
            WidgetConfig html = def("html", 120, 520, 460, 180, QStringLiteral("HTML/CSS"));
            html.text = QStringLiteral("<h2 style=\"color:#5BE7FF;\">LiteDesktopStudio C++</h2>"
                                       "<p style=\"color:white;\">右クリックでメニューを開けます。</p>"
                                       "<p style=\"color:#80FF9F;\">編集モードで移動/リサイズ、変更は自動保存されます。</p>");
            attach(createWidget(html, &audio_));
            for (WidgetWindow *w : widgets_) w->show();
            saveConfig();
            return;
        }
        for (const QJsonValue &v : arr) {
            attach(createWidget(WidgetConfig::fromJson(v.toObject()), &audio_));
        }
        for (WidgetWindow *w : widgets_) w->show();
    }

    QString configPath_;
    QJsonObject rootExtra_;
    QList<WidgetWindow *> widgets_;
    Preview3DWindow *preview3d_ = nullptr;
    QSet<WidgetWindow *> hosted_;
    QString lastZOrder_;
    QSet<WidgetWindow *> safeClamped_; // effects widgets clamped to the work area
    bool hostEnabled_ = false; // opt-in via tray; persisted as config root key
    bool widgetsOnTop_ = false; // tray toggle; persisted as "widgets_on_top"
    bool hostGaveUp_ = false;
    int hostTries_ = 0;
    int hostCycles_ = 0;
    QDateTime hostLastTry_;
    QAction *hostAct_ = nullptr;
    QAction *onTopAct_ = nullptr;
    QAction *debugLogAct_ = nullptr;
    bool debugLogEnabled_ = false; // tray toggle; persisted as "debug_log"
    SystemMonitor monitor_;
    AudioEngine audio_;
    QTimer tick_;
    QTimer layerTimer_;
    QSystemTrayIcon tray_;
    QAction *editAct_ = nullptr;
    bool editMode_ = false;
    WidgetWindow *selectedWidget_ = nullptr; // Delete-key target (v0.15.0)
};

int main(int argc, char *argv[]) {
    // Required by Qt WebEngine when it is initialized from a plugin
    // (v0.14.0): the JSHTML plugin loads WebEngine lazily, long after
    // the QApplication exists, and Chromium's GL context sharing must
    // be opted into before the application object is constructed.
    // Harmless when no html_js widget ever appears.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("UltraLiteDesktopStudio"));
    app.setApplicationDisplayName(QStringLiteral("Ultra Lite Desktop Studio"));
    app.setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
    app.setQuitOnLastWindowClosed(false);
    // Single-instance guard (v0.13.4, the user's request): two
    // instances both load/save the same config, fight over the widget
    // layer order and double-start the audio engine — a reliable bug
    // source. The lock file lives next to the config; while a live
    // instance holds it, a new launch exits silently right here,
    // before any widget, timer or file write exists. staleLockTime
    // is short (2 s) so a crashed / task-manager-killed instance does
    // not block relaunching: once the holder's PID is gone and the
    // lock is older than that, tryLock (given up to 3 s) steals it.
    // A live holder's PID keeps the lock valid indefinitely.
    QLockFile singleLock(QDir::homePath() + QStringLiteral("/LiteDesktopStudio_singleton.lock"));
    singleLock.setStaleLockTime(2000);
    if (!singleLock.tryLock(3000)) return 0;
    AppController controller;
    controller.setupTray();
#ifdef Q_OS_WIN
    // Global "E" / Delete shortcuts (v0.15.0): see ShortcutKeyFilter.
    // The hook callback hops to the UI thread before touching state.
    ShortcutKeyFilter::onKey = [](int vk) {
        QMetaObject::invokeMethod(qApp, [vk] {
            if (auto *c = qApp->property("ldsController").value<AppController *>())
                c->shortcutKeyFromHook(vk);
        }, Qt::QueuedConnection);
    };
    app.setProperty("ldsController", QVariant::fromValue<AppController *>(&controller));
    ShortcutKeyFilter::install();
#endif
    if (!qEnvironmentVariable("LDS_TEST_EDITOR").isEmpty())
        QTimer::singleShot(300, &controller, [&controller] { controller.debugOpenEffectsEditor(); });
    if (!qEnvironmentVariable("LDS_TEST_PREVIEW").isEmpty())
        QTimer::singleShot(300, &controller, [&controller] { controller.openPreview3D(); });
    // Dev hook: with LDS_BURST=<dir>, re-grab the preview window on a
    // 100ms cadence (drag-burst repaint verification).
    const QString burstDir = qEnvironmentVariable("LDS_BURST");
    if (!burstDir.isEmpty()) {
        QDir().mkpath(burstDir);
        auto *burstState = new int(0);
        auto *burstTimer = new QTimer(&app);
        burstTimer->setInterval(100);
        QObject::connect(burstTimer, &QTimer::timeout, &app, [&app, burstDir, burstState] {
            for (QWidget *w : app.topLevelWidgets()) {
                if (QString::fromLatin1(w->metaObject()->className()) != QLatin1String("Preview3DWindow")) continue;
                w->grab().save(QStringLiteral("%1/burst_%2.png").arg(burstDir).arg((*burstState)++, 3, 10, QLatin1Char('0')));
            }
        });
        burstTimer->start();
    }
    // Dev smoke-test hook: with LDS_SHOT_DIR=<dir>, grab every visible
    // top-level window to a PNG after a short delay, then quit. Used by
    // the Linux offscreen verification of the visualizer painters.
    const QString shotDir = qEnvironmentVariable("LDS_SHOT_DIR");
    if (!shotDir.isEmpty()) {
        QTimer::singleShot(2200, &app, [&app, shotDir] {
            QDir().mkpath(shotDir);
            int i = 0;
            for (QWidget *w : app.topLevelWidgets()) {
                if (!w->isVisible() || w->width() < 10) continue;
                w->grab().save(QStringLiteral("%1/%2_%3_%4x%5.png")
                                   .arg(shotDir).arg(i++).arg(QString::fromLatin1(w->metaObject()->className()))
                                   .arg(w->width()).arg(w->height()));
            }
            app.quit();
        });
    }
    return app.exec();
}

#include "main.moc"
