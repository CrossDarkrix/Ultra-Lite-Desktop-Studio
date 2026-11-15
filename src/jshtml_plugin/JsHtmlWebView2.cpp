// LdsJsHtml — the JSHTML plugin, WebView2 backend (v0.14.1, Windows).
//
// Why WebView2: MSYS2 ships no Qt WebEngine for the GCC environments
// (MINGW64 / UCRT64) — Chromium cannot be built with MinGW GCC, so
// `pacman -S mingw-w64-ucrt-x86_64-qt6-webengine` does not exist.
// The user chose the WebView2 route instead: this plugin drives the
// Microsoft Edge WebView2 runtime (preinstalled and evergreen on
// Windows 11) directly through its COM API. The main executable
// still links no browser engine at all; it QLibrary-loads this
// plugin on the first html_js widget, exactly like the Qt WebEngine
// backend used on other platforms (JsHtmlPlugin.cpp). Same C ABI
// (JsHtmlPluginApi.h), same config JSON, same state pushes.
//
// Differences from the WebEngine backend, all forced by the API:
//  - The JS bridge is message passing, not QWebChannel:
//    window.chrome.webview.postMessage carries {id, method, args}
//    and responses come back as {__lds_response, id, result} where
//    result is the same JSON string the WebEngine slots returned.
//    The injected bootstrap (AddScriptToExecuteOnDocumentCreated)
//    exposes the identical window.LDS / LDSReady / System.Gadget
//    surface, so widget HTML works unchanged on both backends.
//  - Pages load through a virtual host: the widget dir is mapped to
//    https://lds.local/ (ICoreWebView2_3), inline documents are
//    written to data/lds_inline.html first (NavigateToString has a
//    2 MB cap; a file URL has none).
//  - Right-click: default context menus are disabled and a capture
//    'contextmenu' listener in the bootstrap posts the screen
//    coordinates to the host, which opens the widget's own menu.
//  - Edit-mode mouse transparency hides the controller: a native
//    child window cannot be click-through the way an alien QWidget
//    can, so while editing, the page is hidden and the host's panel
//    shows instead.
//  - WebView2Loader.dll is loaded dynamically (its MSVC import lib
//    is unusable from MinGW); the DLL ships next to this plugin.
//  - MinGW quirk: WebView2.h includes "EventToken.h" (Windows SDK
//    casing) — third_party/webview2/include/EventToken.h forwards
//    to MinGW's eventtoken.h.

#include "../JsHtmlPluginApi.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QWidget>

#include <windows.h>
#include <unknwn.h>
#include <WebView2.h>

// MinGW's __uuidof<T>() is declared at global scope in guiddef.h, so
// its specializations must be declared before first use and live at
// global scope. MSVC gets the GUIDs from WebView2.h's
// __declspec(uuid); MinGW needs these explicit specializations
// (values = the SDK's IID_ constants in WebView2.h).
#define LDS_UUIDOF(T, iid) \
    template <> inline const GUID &__mingw_uuidof<T>() { \
        static const GUID g = iid; \
        return g; \
    }
LDS_UUIDOF(ICoreWebView2Controller2,
           (GUID{0xC979903E, 0xD4CA, 0x4228, {0x92, 0xEB, 0x47, 0xEE, 0x3F, 0xA9, 0x6E, 0xAB}}))
LDS_UUIDOF(ICoreWebView2_3,
           (GUID{0xA0D6DF20, 0x3B92, 0x416D, {0xAA, 0x0C, 0x43, 0x7A, 0x9C, 0x72, 0x78, 0x57}}))
LDS_UUIDOF(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
           (GUID{0x4E8A3389, 0xC9D8, 0x4BD2, {0xB6, 0xB5, 0x12, 0x4F, 0xEE, 0x6C, 0xC1, 0x4D}}))
LDS_UUIDOF(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
           (GUID{0x6C4819F3, 0xC9B7, 0x4260, {0x81, 0x27, 0xC9, 0xF5, 0xBD, 0xE7, 0xF6, 0x8C}}))
LDS_UUIDOF(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
           (GUID{0xB99369F3, 0x9B11, 0x47B5, {0xBC, 0x6F, 0x8E, 0x78, 0x95, 0xFC, 0xEA, 0x17}}))
LDS_UUIDOF(ICoreWebView2WebMessageReceivedEventHandler,
           (GUID{0x57213F19, 0x00E6, 0x49FA, {0x8E, 0x07, 0x89, 0x8E, 0xA0, 0x1E, 0xCB, 0xD2}}))

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------------------
// Small COM helpers (no WRL / ATL: MinGW ships neither reliably).
// ------------------------------------------------------------------
template <typename T> class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ComPtr(ComPtr &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr &operator=(ComPtr &&o) noexcept {
        if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    void attach(T *p) { reset(); p_ = p; }
    T *get() const { return p_; }
    T **put() { reset(); return &p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
private:
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T *p_ = nullptr;
};

template <typename Iface>
class HandlerBase : public Iface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Iface)) {
            *ppv = static_cast<Iface *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
private:
    std::atomic<ULONG> refs_{1};
};

using CreateEnvFn = HRESULT(STDMETHODCALLTYPE *)(
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);

CreateEnvFn loaderEntry() {
    static CreateEnvFn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    // Search next to this DLL first, then the exe directory, then PATH.
    wchar_t selfPath[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&loaderEntry), &self);
    if (self && GetModuleFileNameW(self, selfPath, MAX_PATH)) {
        std::wstring dir(selfPath);
        const auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring candidate = dir.substr(0, slash + 1) + L"WebView2Loader.dll";
            HMODULE h = LoadLibraryW(candidate.c_str());
            if (h) fn = reinterpret_cast<CreateEnvFn>(GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions"));
            if (fn) return fn;
        }
    }
    HMODULE h = LoadLibraryW(L"WebView2Loader.dll");
    if (h) fn = reinterpret_cast<CreateEnvFn>(GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions"));
    return fn;
}

// ------------------------------------------------------------------
// Document building: identical to the WebEngine backend except the
// bootstrap itself (message-passing instead of QWebChannel) and no
// bootstrap injection into the HTML — WebView2 injects it with
// AddScriptToExecuteOnDocumentCreated, so stored HTML stays clean.
// ------------------------------------------------------------------
const char *kBootstrap = R"BOOTSTRAP((function () {
  if (window.__LDS_JSHTML_API_BOOTSTRAPPED__) return;
  window.__LDS_JSHTML_API_BOOTSTRAPPED__ = true;

  // Right-clicks go to the host, which opens the widget's own menu.
  window.addEventListener("contextmenu", function (e) {
    try {
      e.preventDefault();
      window.chrome.webview.postMessage(JSON.stringify({
        __lds_contextmenu: true, x: e.screenX, y: e.screenY
      }));
    } catch (err) {}
  }, true);

  var seq = 0;
  var pending = {};
  window.chrome.webview.addEventListener("message", function (e) {
    var msg = e.data;
    if (typeof msg === "string") { try { msg = JSON.parse(msg); } catch (err) { return; } }
    if (!msg || !msg.__lds_response) return;
    var entry = pending[msg.id];
    if (!entry) return;
    delete pending[msg.id];
    entry(msg.result);
  });

  function callRaw(method, args) {
    return new Promise(function (resolve) {
      var id = ++seq;
      pending[id] = resolve;
      window.chrome.webview.postMessage(JSON.stringify({
        __lds_call: true, id: id, method: method, args: args || []
      }));
    });
  }
  function parseJson(text) {
    try { return JSON.parse(text); }
    catch (error) { return { ok: false, error: String(error), raw: text }; }
  }
  async function call(method, args) { return parseJson(await callRaw(method, args)); }

  window.LDS = {
    ping: function () { return call("ping"); },
    getWidgetInfo: function () { return call("getWidgetInfo"); },
    getWidgetRect: function () { return call("getWidgetRect"); },
    getSystemInfo: function () { return call("getSystemInfo"); },
    readConfig: function (key) { return call("readConfig", [String(key)]); },
    writeConfig: function (key, value) { return call("writeConfig", [String(key), JSON.stringify(value)]); },
    openUrl: function (url) { return call("openUrl", [String(url)]); },
    getLocalAssetUrl: function (path) { return call("getLocalAssetUrl", [String(path)]); },
    listAssets: function (path) { return call("listAssets", [String(path || "assets")]); },
    readTextFile: function (path) { return call("readTextFile", [String(path)]); },
    getAudioInfo: function () { return call("getAudioInfo"); },
    getAudioLevel: function () { return call("getAudioLevel"); },
    getAudioSpectrum: function () { return call("getAudioSpectrum"); }
  };

  window.System = window.System || {};
  window.System.Gadget = window.System.Gadget || {};
  window.System.Gadget.version = "LiteDesktopStudio-compat";
  window.System.Gadget.settings = window.System.Gadget.settings || {};
  window.System.Gadget.settings.read = async function (key) {
    var result = await window.LDS.readConfig(key);
    return result ? result.value : null;
  };
  window.System.Gadget.settings.write = async function (key, value) {
    return await window.LDS.writeConfig(key, value);
  };
  window.LDSReady = Promise.resolve(window.LDS);
})();
)BOOTSTRAP";

const char *kDefaultHtml = R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<style>
html, body {
    margin: 0;
    padding: 0;
    width: 100%;
    height: 100%;
    overflow: hidden;
    background: transparent;
    font-family: "Segoe UI", sans-serif;
    color: white;
}
.card {
    box-sizing: border-box;
    width: 100%;
    height: 100%;
    border-radius: 16px;
    padding: 16px;
    background: rgba(16, 20, 28, 0.62);
    border: 1px solid rgba(255, 255, 255, 0.08);
}
.title {
    font-size: 20px;
    font-weight: 700;
    color: #80FF9F;
}
button {
    margin-top: 12px;
    padding: 8px 12px;
    border-radius: 8px;
    border: 1px solid rgba(128, 255, 159, 0.55);
    background: rgba(128, 255, 159, 0.12);
    color: white;
}
</style>
</head>
<body>
<div class="card">
  <div class="title">JavaScript Widget</div>
  <p id="text">この内容は WidgetConfig.text として config.json に保存されます。</p>
  <button onclick="document.getElementById('text').textContent = 'Clicked: ' + new Date().toLocaleTimeString();">
    Click me
  </button>
</div>
</body>
</html>
)HTML";

// The page is served over the virtual host with no charset in the
// HTTP headers, so Chromium falls back to the system legacy encoding
// (Shift_JIS on Japanese Windows) and UTF-8 Japanese text turns into
// mojibake — seen on the user's machine with the default document,
// which DOES declare <meta charset>. The meta tag is only honored
// within the first 1024 bytes, and our default document's <style>
// pushes it past that. So: strip any existing charset declaration
// and put a fresh one at the very start of <head> (inserting a head
// when the document has none). Idempotent.
QString ensureCharset(QString doc) {
    static const QRegularExpression charsetRe(
        QStringLiteral("<meta[^>]*charset[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    doc.remove(charsetRe);
    const QString meta = QStringLiteral("<meta charset=\"utf-8\">");
    const QRegularExpression headRe(QStringLiteral("<head[^>]*>"),
                                    QRegularExpression::CaseInsensitiveOption);
    const auto hm = headRe.match(doc);
    if (hm.hasMatch()) {
        doc.insert(int(hm.capturedEnd()), meta);
        return doc;
    }
    const QRegularExpression htmlRe(QStringLiteral("<html[^>]*>"),
                                    QRegularExpression::CaseInsensitiveOption);
    const auto xm = htmlRe.match(doc);
    if (xm.hasMatch()) {
        doc.insert(int(xm.capturedEnd()), QStringLiteral("<head>") + meta + QStringLiteral("</head>"));
        return doc;
    }
    return meta + doc;
}

QString buildDocument(const QString &html) {
    const QString lower = html.toLower();
    if (lower.contains(QStringLiteral("<html")) || lower.contains(QStringLiteral("<!doctype")))
        return ensureCharset(html);
    return QStringLiteral(
               "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<style>\n"
               "html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; "
               "background: transparent; font-family: \"Segoe UI\", sans-serif; color: white; }\n"
               "body { box-sizing: border-box; }\n</style>\n</head>\n<body>\n") +
           html + QStringLiteral("\n</body>\n</html>");
}

// Same rules as the host's JsHtmlPackage::safePath.
QString safePath(const QString &root, const QString &relative) {
    const QString rootClean = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    QString rel = relative;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
    if (rel.contains(QLatin1Char(':'))) return QString();
    const QString target = QDir::cleanPath(rootClean + QLatin1Char('/') + rel);
    if (target != rootClean && !target.startsWith(rootClean + QLatin1Char('/')))
        return QString();
    const QString canonical = QFileInfo(target).canonicalFilePath();
    if (!canonical.isEmpty() && canonical != rootClean &&
        !canonical.startsWith(rootClean + QLatin1Char('/')))
        return QString();
    return target;
}

QString jsonString(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// URL the page (running on https://lds.local, the virtual host
// mapped to the widget folder) can actually load for a file inside
// the widget directory: same origin, percent-encoded (v0.14.9).
// The relative path is computed on cleaned absolute paths:
// QDir::relativeFilePath mangles Windows drive-letter paths.
QString localUrl(const QString &hostName, const QString &widgetDir, const QString &absolutePath) {
    const QString base = QDir::cleanPath(QFileInfo(widgetDir).absoluteFilePath());
    const QString target = QDir::cleanPath(QFileInfo(absolutePath).absoluteFilePath());
    const QString rel =
        target.startsWith(base + QLatin1Char('/')) ? target.mid(base.size() + 1) : target;
    return QStringLiteral("https://") + hostName + QLatin1Char('/') +
           QString::fromUtf8(QUrl::toPercentEncoding(rel, "/"));
}

// ------------------------------------------------------------------
// Per-view state + the LDS API dispatch (same semantics and result
// shapes as the WebEngine backend's LdsApi).
// ------------------------------------------------------------------
struct ViewState {
    QWidget *holder = nullptr;
    HWND hwnd = nullptr;
    LdsJsContextMenuFn menuFn = nullptr;
    void *menuCtx = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    QJsonObject cfg;
    QJsonObject state;
    QJsonObject store;
    QString widgetDir;
    QString storePath;
    QString lastSourceKey;
    QString hostName; // this view's own virtual host (v0.15.2)
    bool ready = false;
    bool mouseTransparent = false;
    bool visibleWanted = true;

    void bind(const QJsonObject &cfgNew) {
        cfg = cfgNew;
        widgetDir = cfg.value(QStringLiteral("widgetDir")).toString();
        // "lds-<instanceId>.local": unique per widget instance, so
        // two views can never serve each other's folders (v0.15.2).
        QString id = cfg.value(QStringLiteral("instanceId")).toString();
        id.remove(QLatin1Char('{')).remove(QLatin1Char('}'));
        hostName = id.isEmpty() ? QStringLiteral("lds.local")
                                : QStringLiteral("lds-") + id.toLower() + QStringLiteral(".local");
        storePath = widgetDir + QStringLiteral("/data/config.json");
        store = QJsonObject();
        QFile f(storePath);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) store = doc.object();
        }
        QDir().mkpath(widgetDir + QStringLiteral("/data"));
    }

    QString result(bool ok, const QJsonObject &extra) const {
        QJsonObject o = extra;
        o[QStringLiteral("ok")] = ok;
        return jsonString(o);
    }

    QString dispatch(const QString &method, const QJsonArray &args) {
        auto arg = [&](int i) { return i < args.size() ? args.at(i).toString() : QString(); };
        if (method == QLatin1String("ping")) {
            return result(true, {{QStringLiteral("message"), QStringLiteral("pong")},
                                 {QStringLiteral("runtime"), QStringLiteral("LiteDesktopStudio.JSHTML")}});
        }
        if (method == QLatin1String("getWidgetInfo")) {
            QJsonObject o;
            o[QStringLiteral("title")] = cfg.value(QStringLiteral("title")).toString();
            o[QStringLiteral("mode")] = cfg.value(QStringLiteral("mode")).toString(QStringLiteral("inline"));
            o[QStringLiteral("instanceId")] = cfg.value(QStringLiteral("instanceId")).toString();
            o[QStringLiteral("entry")] = cfg.value(QStringLiteral("entry")).toString(QStringLiteral("index.html"));
            o[QStringLiteral("packageName")] = cfg.value(QStringLiteral("packageName")).toString();
            o[QStringLiteral("packageVersion")] = cfg.value(QStringLiteral("packageVersion")).toString();
            o[QStringLiteral("widgetDir")] = widgetDir;
            return result(true, o);
        }
        if (method == QLatin1String("getWidgetRect")) {
            QWidget *top = holder ? holder->parentWidget() : nullptr;
            if (!top) return result(false, {{QStringLiteral("error"), QStringLiteral("widget is not bound")}});
            const QRect r = top->geometry();
            return result(true, {{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()},
                                 {QStringLiteral("width"), r.width()}, {QStringLiteral("height"), r.height()}});
        }
        if (method == QLatin1String("getSystemInfo")) {
            QJsonObject o;
            o[QStringLiteral("cpuPercent")] = state.value(QStringLiteral("cpuPercent")).toDouble(0.0);
            o[QStringLiteral("memoryPercent")] = state.value(QStringLiteral("memoryPercent")).toDouble(0.0);
            o[QStringLiteral("diskPercent")] = state.value(QStringLiteral("diskPercent")).toDouble(0.0);
            o[QStringLiteral("battery")] = QJsonValue::Null;
            o[QStringLiteral("platform")] = state.value(QStringLiteral("platform")).toString(QStringLiteral("win32"));
            return result(true, o);
        }
        if (method == QLatin1String("readConfig")) {
            QJsonObject o;
            o[QStringLiteral("key")] = arg(0);
            o[QStringLiteral("value")] = store.value(arg(0));
            return result(true, o);
        }
        if (method == QLatin1String("writeConfig")) {
            const QString key = arg(0);
            const QString valueJson = arg(1);
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(valueJson.toUtf8(), &err);
            const QJsonValue value = (err.error == QJsonParseError::NoError)
                                         ? QJsonValue::fromVariant(doc.toVariant())
                                         : QJsonValue(valueJson);
            store.insert(key, value);
            bool saved = false;
            QDir().mkpath(widgetDir + QStringLiteral("/data"));
            QFile f(storePath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                saved = f.write(QJsonDocument(store).toJson(QJsonDocument::Indented)) > 0;
            QJsonObject o;
            o[QStringLiteral("key")] = key;
            o[QStringLiteral("value")] = value;
            return result(saved, o);
        }
        if (method == QLatin1String("openUrl")) {
            const QString url = arg(0);
            if (!url.startsWith(QStringLiteral("https://")) && !url.startsWith(QStringLiteral("http://")))
                return result(false, {{QStringLiteral("error"),
                                       QStringLiteral("Only http:// and https:// URLs are allowed")}});
            return result(QDesktopServices::openUrl(QUrl(url)), {});
        }
        if (method == QLatin1String("getLocalAssetUrl")) {
            const QString target = safePath(widgetDir, arg(0));
            if (target.isEmpty())
                return result(false, {{QStringLiteral("error"), QStringLiteral("Path is outside widget directory")}});
            if (!QFileInfo::exists(target))
                return result(false, {{QStringLiteral("error"), QStringLiteral("Asset not found")}});
            // Virtual-host URL, NOT file:// (v0.14.9): the page runs
            // on https://lds.local and Chromium blocks file://
            // subresources from an https page, so the Python-shaped
            // file:// URL left <img> broken on the user's machine.
            return result(true, {{QStringLiteral("path"), target},
                                 {QStringLiteral("url"), localUrl(hostName, widgetDir, target)}});
        }
        if (method == QLatin1String("listAssets")) {
            const QString target = safePath(widgetDir, arg(0).isEmpty() ? QStringLiteral("assets") : arg(0));
            if (target.isEmpty())
                return result(false, {{QStringLiteral("error"), QStringLiteral("Path is outside widget directory")}});
            const QFileInfo fi(target);
            if (!fi.exists() || !fi.isDir())
                return result(false, {{QStringLiteral("error"), QStringLiteral("Directory not found")},
                                      {QStringLiteral("items"), QJsonArray()}});
            QJsonArray items;
            const QDir d(target);
            for (const QFileInfo &child : d.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase)) {
                QJsonObject item;
                item[QStringLiteral("name")] = child.fileName();
                item[QStringLiteral("relativePath")] = QDir(widgetDir).relativeFilePath(child.absoluteFilePath());
                item[QStringLiteral("url")] = localUrl(hostName, widgetDir, child.absoluteFilePath());
                item[QStringLiteral("size")] = double(child.size());
                items.append(item);
            }
            return result(true, {{QStringLiteral("items"), items}});
        }
        if (method == QLatin1String("readTextFile")) {
            const QString target = safePath(widgetDir, arg(0));
            if (target.isEmpty())
                return result(false, {{QStringLiteral("error"), QStringLiteral("Path is outside widget directory")}});
            const QFileInfo fi(target);
            if (!fi.exists() || !fi.isFile())
                return result(false, {{QStringLiteral("error"), QStringLiteral("File not found")}});
            if (fi.size() > 2 * 1024 * 1024)
                return result(false, {{QStringLiteral("error"), QStringLiteral("File is too large")}});
            QFile f(target);
            if (!f.open(QIODevice::ReadOnly))
                return result(false, {{QStringLiteral("error"), QStringLiteral("File not found")}});
            return result(true, {{QStringLiteral("text"), QString::fromUtf8(f.readAll())}});
        }
        if (method == QLatin1String("getAudioInfo") || method == QLatin1String("getAudioLevel") ||
            method == QLatin1String("getAudioSpectrum")) {
            const bool withBands = method != QLatin1String("getAudioInfo");
            const QJsonObject a = state.value(QStringLiteral("audio")).toObject();
            const bool avail = a.value(QStringLiteral("available")).toBool(false);
            QJsonObject o;
            o[QStringLiteral("available")] = avail;
            if (withBands) {
                o[QStringLiteral("bands")] = a.value(QStringLiteral("bands")).toArray();
                o[QStringLiteral("level")] = a.value(QStringLiteral("level")).toDouble(0.0);
                o[QStringLiteral("peak")] = a.value(QStringLiteral("peak")).toDouble(0.0);
                o[QStringLiteral("rms")] = a.value(QStringLiteral("rms")).toDouble(0.0);
            }
            o[QStringLiteral("bandCount")] = a.value(QStringLiteral("bandCount")).toInt(0);
            o[QStringLiteral("bars")] = a.value(QStringLiteral("bandCount")).toInt(0);
            o[QStringLiteral("backend")] = a.value(QStringLiteral("backend")).toString(QStringLiteral("unavailable"));
            o[QStringLiteral("useFake")] = a.value(QStringLiteral("useFake")).toBool(false);
            o[QStringLiteral("running")] = a.value(QStringLiteral("running")).toBool(false);
            o[QStringLiteral("timestamp")] = a.value(QStringLiteral("timestamp")).toDouble(0.0);
            o[QStringLiteral("error")] = avail ? QString() : QStringLiteral("audio engine is not available");
            return result(avail, o);
        }
        return result(false, {{QStringLiteral("error"), QStringLiteral("unknown method")}});
    }
};

QHash<void *, ViewState *> g_views;

// Shared environment (one per process), created on first use.
ComPtr<ICoreWebView2Environment> g_env;
bool g_envPending = false;
std::vector<ViewState *> g_waiting;

void loadContent(ViewState *st, bool force) {
    if (!st->webview) return;
    const QJsonObject &cfg = st->cfg;
    const QString mode = cfg.value(QStringLiteral("mode")).toString(QStringLiteral("inline"));
    QString sourceKey, url;
    if (mode == QLatin1String("package")) {
        const QString entry = cfg.value(QStringLiteral("entry")).toString(QStringLiteral("index.html"));
        const QString entryPath = safePath(st->widgetDir, entry);
        if (entryPath.isEmpty() || !QFileInfo(entryPath).isFile()) {
            const QString errDoc = ensureCharset(buildDocument(QStringLiteral(
                "<div style='padding:14px;border:1px solid rgba(255,255,255,.2);border-radius:14px;"
                "background:rgba(20,24,32,.72);color:white;font-family:Segoe UI,sans-serif;'>"
                "<h3 style='margin:0 0 8px;color:#FFCC66;'>JSHTML Package Error</h3>"
                "<div>entry HTML が見つかりません: ") + entry + QStringLiteral("</div></div>")));
            QFile ef(st->widgetDir + QStringLiteral("/data/lds_inline.html"));
            if (ef.open(QIODevice::WriteOnly | QIODevice::Truncate)) ef.write(errDoc.toUtf8());
            url = QStringLiteral("https://") + st->hostName + QStringLiteral("/data/lds_inline.html");
            sourceKey = QStringLiteral("package-error");
        } else {
            const QFileInfo fi(entryPath);
            sourceKey = QStringLiteral("package:%1:%2:%3")
                            .arg(entryPath)
                            .arg(double(fi.lastModified().toMSecsSinceEpoch()))
                            .arg(double(fi.size()));
            url = QStringLiteral("https://") + st->hostName + QLatin1Char('/') +
                  QDir(st->widgetDir).relativeFilePath(entryPath);
        }
    } else {
        QString raw = cfg.value(QStringLiteral("text")).toString();
        if (raw.isEmpty()) raw = QString::fromUtf8(kDefaultHtml);
        const QString doc = ensureCharset(buildDocument(raw));
        sourceKey = QStringLiteral("inline:") + doc;
        if (force || st->lastSourceKey != sourceKey) {
            QFile f(st->widgetDir + QStringLiteral("/data/lds_inline.html"));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(doc.toUtf8());
        }
        url = QStringLiteral("https://") + st->hostName + QStringLiteral("/data/lds_inline.html");
    }
    if (!force && st->lastSourceKey == sourceKey) return;
    st->lastSourceKey = sourceKey;
    st->webview->Navigate(url.toStdWString().c_str());
}

void syncBounds(ViewState *st) {
    if (!st->controller || !st->holder) return;
    const QSize s = st->holder->size();
    RECT rc{0, 0, s.width(), s.height()};
    st->controller->put_Bounds(rc);
}

void applyVisibility(ViewState *st) {
    if (!st->controller) return;
    const BOOL vis = (st->visibleWanted && !st->mouseTransparent) ? TRUE : FALSE;
    st->controller->put_IsVisible(vis);
}

// Holder: the QWidget the host embeds. WA_NativeWindow gives it the
// HWND the WebView2 controller attaches to; the event filter keeps
// bounds/visibility in sync with the holder.
class Holder : public QWidget {
public:
    explicit Holder(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_NativeWindow, true);
        setAttribute(Qt::WA_DontCreateNativeAncestors, true);
        installEventFilter(this);
    }
    ViewState *st = nullptr;
protected:
    bool eventFilter(QObject *, QEvent *e) override {
        if (!st) return false;
        if (e->type() == QEvent::Resize) syncBounds(st);
        else if (e->type() == QEvent::Show) { st->visibleWanted = true; applyVisibility(st); }
        else if (e->type() == QEvent::Hide) { st->visibleWanted = false; applyVisibility(st); }
        return false;
    }
};

class MessageHandler : public HandlerBase<ICoreWebView2WebMessageReceivedEventHandler> {
public:
    explicit MessageHandler(ViewState *st) : st_(st) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *sender,
                                     ICoreWebView2WebMessageReceivedEventArgs *args) override {
        LPWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
        const QString text = QString::fromWCharArray(raw);
        CoTaskMemFree(raw);
        const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
        if (msg.value(QStringLiteral("__lds_contextmenu")).toBool()) {
            if (st_->menuFn) {
                st_->menuFn(st_->menuCtx,
                            int(msg.value(QStringLiteral("x")).toDouble()),
                            int(msg.value(QStringLiteral("y")).toDouble()));
            }
            return S_OK;
        }
        if (msg.value(QStringLiteral("__lds_call")).toBool()) {
            const QString res = st_->dispatch(msg.value(QStringLiteral("method")).toString(),
                                              msg.value(QStringLiteral("args")).toArray());
            QJsonObject resp;
            resp[QStringLiteral("__lds_response")] = true;
            resp[QStringLiteral("id")] = msg.value(QStringLiteral("id"));
            resp[QStringLiteral("result")] = res;
            sender->PostWebMessageAsJson(jsonString(resp).toStdWString().c_str());
        }
        return S_OK;
    }
private:
    ViewState *st_;
};

class ScriptHandler : public HandlerBase<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT, LPCWSTR) override { return S_OK; }
};

class ControllerHandler : public HandlerBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    explicit ControllerHandler(ViewState *st) : st_(st) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller *controller) override {
        if (FAILED(hr) || !controller) return S_OK;
        controller->AddRef(); // the callback hands us a borrowed ref
        st_->controller.attach(controller);
        ComPtr<ICoreWebView2> wv;
        controller->get_CoreWebView2(wv.put());
        st_->webview = std::move(wv);
        if (!st_->webview) return S_OK;

        ComPtr<ICoreWebView2Settings> settings;
        st_->webview->get_Settings(settings.put());
        if (settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(TRUE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Controller2> c2;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(c2.put())))) {
            COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
            c2->put_DefaultBackgroundColor(transparent);
        }
        ComPtr<ICoreWebView2_3> wv3;
        if (SUCCEEDED(st_->webview->QueryInterface(IID_PPV_ARGS(wv3.put())))) {
            // Per-view host (v0.15.2): every view gets its own host
            // name. A shared "lds.local" was hijacked by whichever
            // view registered last, and after an overwrite import the
            // stale view's reload tick fetched the OTHER widget's
            // files under the same URL — old and new packages
            // alternated on screen (user report).
            const QString nativeDir = QDir::toNativeSeparators(st_->widgetDir);
            wv3->SetVirtualHostNameToFolderMapping(
                st_->hostName.toStdWString().c_str(), nativeDir.toStdWString().c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
        EventRegistrationToken token{};
        st_->webview->add_WebMessageReceived(new MessageHandler(st_), &token);
        st_->webview->AddScriptToExecuteOnDocumentCreated(
            QString::fromLatin1(kBootstrap).toStdWString().c_str(), new ScriptHandler());
        st_->ready = true;
        syncBounds(st_);
        applyVisibility(st_);
        loadContent(st_, true);
        return S_OK;
    }
private:
    ViewState *st_;
};

class EnvHandler : public HandlerBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment *env) override {
        g_envPending = false;
        if (SUCCEEDED(hr) && env) {
            env->AddRef(); // borrowed callback ref -> owned by g_env
            g_env.attach(env);
        }
        auto waiting = std::move(g_waiting);
        g_waiting.clear();
        if (!g_env) return S_OK;
        for (ViewState *st : waiting) {
            if (st->hwnd)
                g_env->CreateCoreWebView2Controller(st->hwnd, new ControllerHandler(st));
        }
        return S_OK;
    }
};

void ensureController(ViewState *st) {
    if (g_env) {
        g_env->CreateCoreWebView2Controller(st->hwnd, new ControllerHandler(st));
        return;
    }
    g_waiting.push_back(st);
    if (g_envPending) return;
    CreateEnvFn create = loaderEntry();
    if (!create) return;
    g_envPending = true;
    // User data folder: per-app, under LocalAppData (the Python
    // backend used an off-the-record profile; WebView2 always needs
    // a folder, so it gets a dedicated, app-owned one).
    wchar_t localApp[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH);
    const std::wstring udf = std::wstring(localApp) + L"\\UltraLiteDesktopStudio\\WebView2";
    create(nullptr, udf.c_str(), nullptr, new EnvHandler());
}

QJsonObject parseCfg(const char *cfgJsonUtf8) {
    return QJsonDocument::fromJson(QByteArray(cfgJsonUtf8 ? cfgJsonUtf8 : "")).object();
}

} // namespace

extern "C" {

Q_DECL_EXPORT void *lds_js_create(QWidget *parent, const char *cfgJsonUtf8,
                                  LdsJsContextMenuFn menuFn, void *menuCtx) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto *holder = new Holder(parent);
    auto *st = new ViewState;
    st->holder = holder;
    st->menuFn = menuFn;
    st->menuCtx = menuCtx;
    st->bind(parseCfg(cfgJsonUtf8));
    holder->st = st;
    g_views.insert(holder, st);
    QObject::connect(holder, &QObject::destroyed, [holder, st] {
        g_views.remove(holder);
        auto it = std::find(g_waiting.begin(), g_waiting.end(), st);
        if (it != g_waiting.end()) g_waiting.erase(it);
        if (st->controller) st->controller->Close();
        delete st;
    });
    // The HWND exists once the holder is shown as a native child;
    // defer controller creation to the first show/polish.
    QTimer::singleShot(0, holder, [st, holder] {
        st->hwnd = reinterpret_cast<HWND>(holder->winId());
        if (st->hwnd) ensureController(st);
    });
    return holder;
}

Q_DECL_EXPORT void lds_js_reload(void *view, const char *cfgJsonUtf8) {
    ViewState *st = g_views.value(static_cast<QWidget *>(view));
    if (!st) return;
    st->bind(parseCfg(cfgJsonUtf8));
    loadContent(st, true);
}

Q_DECL_EXPORT void lds_js_push_state(void *view, const char *stateJsonUtf8) {
    ViewState *st = g_views.value(static_cast<QWidget *>(view));
    if (!st) return;
    // Merge, not replace (v0.14.8): the host's 20 fps audio pushes
    // carry only the audio object; the 1 Hz full push carries the
    // rest. Replacing would wipe cpu/memory between full pushes.
    const QJsonObject incoming =
        QJsonDocument::fromJson(QByteArray(stateJsonUtf8 ? stateJsonUtf8 : "")).object();
    for (auto it = incoming.begin(); it != incoming.end(); ++it) st->state[it.key()] = it.value();
}

Q_DECL_EXPORT void lds_js_set_mouse_transparent(void *view, int on) {
    ViewState *st = g_views.value(static_cast<QWidget *>(view));
    if (!st) return;
    st->mouseTransparent = on != 0;
    applyVisibility(st);
}

} // extern "C"
