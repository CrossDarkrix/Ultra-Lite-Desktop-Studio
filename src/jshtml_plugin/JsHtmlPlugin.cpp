// LdsJsHtml — the optional JSHTML plugin (v0.14.0).
//
// This is the ONLY component that links Qt WebEngine. The main
// executable loads it with QLibrary the first time an html_js
// widget appears (see JsHtmlWidget.cpp), so setups without JSHTML
// widgets never start Chromium and never even map these DLLs.
//
// Faithful port of the Python build's litedesktopstudio/jshtml.py
// view side: same config keys, same widget-dir layout, same LDS API
// (window.LDS via QWebChannel object "LDSApi", all methods returning
// JSON strings), same System.Gadget compatibility shim, same
// package-mode source key (path + mtime + size) and the same
// traversal-safe path rules. Deliberate deviations, per
// Design principle 4 ("no keep-running tricks"):
//   - no QTWEBENGINE_CHROMIUM_FLAGS manipulation (the Python build
//     disabled frame-rate limits / background throttling; we don't)
//   - no realtime-guard rAF loop in the bootstrap, no keepalive
//     timer, no memory watchdog/recycle — Chromium's default page
//     lifecycle applies
//   - one shared off-the-record profile for all views (the plan's
//     "複数ウィジェットでプロファイルを共有"), configured like the
//     Python build's per-view profiles: no HTTP cache, no
//     persistent cookies, spellcheck off.
#include "../JsHtmlPluginApi.h"
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QWebChannel>
#include <QtWebEngineCore/QWebEnginePage>
#include <QtWebEngineCore/QWebEngineProfile>
#include <QtWebEngineCore/QWebEngineSettings>
#include <QtWebEngineWidgets/QWebEngineView>

namespace {

// ------------------------------------------------------------------
// Document building (port of build_js_html_document /
// inject_jshtml_api_bootstrap; the realtime guard is omitted, see
// the header comment).
// ------------------------------------------------------------------

const char *kBootstrap = R"BOOTSTRAP(<!-- LiteDesktopStudio JSHTML API bootstrap -->
<script src="qrc:///qtwebchannel/qwebchannel.js"></script>
<script>
(function () {
  if (window.__LDS_JSHTML_API_BOOTSTRAPPED__) return;
  window.__LDS_JSHTML_API_BOOTSTRAPPED__ = true;

  function parseJson(text) {
    try { return JSON.parse(text); }
    catch (error) { return { ok: false, error: String(error), raw: text }; }
  }

  window.LDSReady = new Promise(function (resolve, reject) {
    function fail(message) { reject(new Error(message)); }
    if (typeof QWebChannel === "undefined") { fail("QWebChannel is not available"); return; }
    if (typeof qt === "undefined" || !qt.webChannelTransport) { fail("qt.webChannelTransport is not available"); return; }

    new QWebChannel(qt.webChannelTransport, function (channel) {
      var raw = channel.objects.LDSApi;
      if (!raw) { fail("LDSApi is not available"); return; }

      window.LDS = {
        ping: async function () { return parseJson(await raw.ping()); },
        getWidgetInfo: async function () { return parseJson(await raw.getWidgetInfo()); },
        getWidgetRect: async function () { return parseJson(await raw.getWidgetRect()); },
        getSystemInfo: async function () { return parseJson(await raw.getSystemInfo()); },
        readConfig: async function (key) { return parseJson(await raw.readConfig(String(key))); },
        writeConfig: async function (key, value) { return parseJson(await raw.writeConfig(String(key), JSON.stringify(value))); },
        openUrl: async function (url) { return parseJson(await raw.openUrl(String(url))); },
        getLocalAssetUrl: async function (path) { return parseJson(await raw.getLocalAssetUrl(String(path))); },
        listAssets: async function (path) { return parseJson(await raw.listAssets(String(path || "assets"))); },
        readTextFile: async function (path) { return parseJson(await raw.readTextFile(String(path))); },
        getAudioInfo: async function () { return parseJson(await raw.getAudioInfo()); },
        getAudioLevel: async function () { return parseJson(await raw.getAudioLevel()); },
        getAudioSpectrum: async function () { return parseJson(await raw.getAudioSpectrum()); }
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
      resolve(window.LDS);
    });
  });
})();
</script>
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

QString injectBootstrap(const QString &html) {
    if (html.contains(QStringLiteral("__LDS_JSHTML_API_BOOTSTRAPPED__"))) return html;
    const QString bootstrap = QString::fromLatin1(kBootstrap);
    const QString lower = html.toLower();
    const qsizetype headPos = lower.indexOf(QStringLiteral("</head>"));
    if (headPos >= 0) return html.left(headPos) + bootstrap + html.mid(headPos);
    const qsizetype bodyPos = lower.indexOf(QStringLiteral("<body"));
    if (bodyPos >= 0) {
        const qsizetype bodyEnd = html.indexOf(QLatin1Char('>'), bodyPos);
        if (bodyEnd >= 0) return html.left(bodyEnd + 1) + bootstrap + html.mid(bodyEnd + 1);
    }
    return bootstrap + html;
}

// Keep the charset declaration inside the first bytes of the
// document: a <meta charset> past the prescan window (~1024 bytes)
// is ignored when the page arrives without an HTTP charset, and
// UTF-8 text then falls back to the system legacy encoding. Strip
// any existing declaration and re-insert it at the start of <head>.
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
        return injectBootstrap(ensureCharset(html));
    const QString doc = QStringLiteral(
        "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<style>\n"
        "html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; "
        "background: transparent; font-family: \"Segoe UI\", sans-serif; color: white; }\n"
        "body { box-sizing: border-box; }\n</style>\n</head>\n<body>\n") +
        html + QStringLiteral("\n</body>\n</html>");
    return injectBootstrap(doc);
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

// ------------------------------------------------------------------
// LDS API object (port of JSHtmlWidgetApi). One per view. System and
// audio answers come from the host's per-second state pushes.
// ------------------------------------------------------------------
class LdsApi : public QObject {
    Q_OBJECT
public:
    explicit LdsApi(QObject *parent = nullptr) : QObject(parent) {}

    void bind(const QJsonObject &cfg, QWebEngineView *view) {
        cfg_ = cfg;
        view_ = view;
        widgetDir_ = cfg.value(QStringLiteral("widgetDir")).toString();
        storePath_ = widgetDir_ + QStringLiteral("/data/config.json");
        store_ = QJsonObject();
        QFile f(storePath_);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) store_ = doc.object();
        }
        QDir().mkpath(widgetDir_ + QStringLiteral("/data"));
    }
    // Merge, not replace (v0.14.8): the host's 20 fps audio pushes
    // carry only the audio object; the 1 Hz full push carries the
    // rest. Replacing would wipe cpu/memory between full pushes.
    void setState(const QJsonObject &state) {
        for (auto it = state.begin(); it != state.end(); ++it) state_[it.key()] = it.value();
    }

    Q_INVOKABLE QString ping() {
        return result(true, {{QStringLiteral("message"), QStringLiteral("pong")},
                             {QStringLiteral("runtime"), QStringLiteral("LiteDesktopStudio.JSHTML")}});
    }
    Q_INVOKABLE QString getWidgetInfo() {
        QJsonObject o;
        o[QStringLiteral("title")] = cfg_.value(QStringLiteral("title")).toString();
        o[QStringLiteral("mode")] = cfg_.value(QStringLiteral("mode")).toString(QStringLiteral("inline"));
        o[QStringLiteral("instanceId")] = cfg_.value(QStringLiteral("instanceId")).toString();
        o[QStringLiteral("entry")] = cfg_.value(QStringLiteral("entry")).toString(QStringLiteral("index.html"));
        o[QStringLiteral("packageName")] = cfg_.value(QStringLiteral("packageName")).toString();
        o[QStringLiteral("packageVersion")] = cfg_.value(QStringLiteral("packageVersion")).toString();
        o[QStringLiteral("widgetDir")] = widgetDir_;
        return result(true, o);
    }
    Q_INVOKABLE QString getWidgetRect() {
        if (!view_ || !view_->parentWidget())
            return result(false, {{QStringLiteral("error"), QStringLiteral("widget is not bound")}});
        const QRect r = view_->parentWidget()->geometry();
        return result(true, {{QStringLiteral("x"), r.x()}, {QStringLiteral("y"), r.y()},
                             {QStringLiteral("width"), r.width()}, {QStringLiteral("height"), r.height()}});
    }
    Q_INVOKABLE QString getSystemInfo() {
        QJsonObject o;
        o[QStringLiteral("cpuPercent")] = state_.value(QStringLiteral("cpuPercent")).toDouble(0.0);
        o[QStringLiteral("memoryPercent")] = state_.value(QStringLiteral("memoryPercent")).toDouble(0.0);
        o[QStringLiteral("diskPercent")] = state_.value(QStringLiteral("diskPercent")).toDouble(0.0);
        o[QStringLiteral("battery")] = QJsonValue::Null; // no battery source in the C++ build
        o[QStringLiteral("platform")] = state_.value(QStringLiteral("platform")).toString(QStringLiteral("win32"));
        return result(true, o);
    }
    Q_INVOKABLE QString readConfig(const QString &key) {
        QJsonObject o;
        o[QStringLiteral("key")] = key;
        o[QStringLiteral("value")] = store_.value(key);
        return result(true, o);
    }
    Q_INVOKABLE QString writeConfig(const QString &key, const QString &valueJson) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(valueJson.toUtf8(), &err);
        const QJsonValue value = (err.error == QJsonParseError::NoError)
                                     ? QJsonValue::fromVariant(doc.toVariant())
                                     : QJsonValue(valueJson);
        store_.insert(key, value);
        bool saved = false;
        QDir().mkpath(widgetDir_ + QStringLiteral("/data"));
        QFile f(storePath_);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            saved = f.write(QJsonDocument(store_).toJson(QJsonDocument::Indented)) > 0;
        }
        QJsonObject o;
        o[QStringLiteral("key")] = key;
        o[QStringLiteral("value")] = value;
        return result(saved, o);
    }
    Q_INVOKABLE QString openUrl(const QString &url) {
        if (!url.startsWith(QStringLiteral("https://")) && !url.startsWith(QStringLiteral("http://")))
            return result(false, {{QStringLiteral("error"),
                                   QStringLiteral("Only http:// and https:// URLs are allowed")}});
        const bool ok = QDesktopServices::openUrl(QUrl(url));
        return result(ok, {});
    }
    Q_INVOKABLE QString getLocalAssetUrl(const QString &path) {
        const QString target = safePath(widgetDir_, path);
        if (target.isEmpty())
            return result(false, {{QStringLiteral("error"), QStringLiteral("Path is outside widget directory")}});
        if (!QFileInfo::exists(target))
            return result(false, {{QStringLiteral("error"), QStringLiteral("Asset not found")}});
        return result(true, {{QStringLiteral("path"), target},
                             {QStringLiteral("url"), QUrl::fromLocalFile(target).toString()}});
    }
    Q_INVOKABLE QString listAssets(const QString &relativeDir) {
        const QString target = safePath(widgetDir_, relativeDir.isEmpty() ? QStringLiteral("assets") : relativeDir);
        if (target.isEmpty())
            return result(false, {{QStringLiteral("error"), QStringLiteral("Path is outside widget directory")}});
        const QFileInfo fi(target);
        if (!fi.exists() || !fi.isDir())
            return result(false, {{QStringLiteral("error"), QStringLiteral("Directory not found")},
                                  {QStringLiteral("items"), QJsonArray()}});
        QJsonArray items;
        const QDir d(target);
        QFileInfoList files = d.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo &child : files) {
            QJsonObject item;
            item[QStringLiteral("name")] = child.fileName();
            item[QStringLiteral("relativePath")] =
                QDir(widgetDir_).relativeFilePath(child.absoluteFilePath());
            item[QStringLiteral("url")] = QUrl::fromLocalFile(child.absoluteFilePath()).toString();
            item[QStringLiteral("size")] = double(child.size());
            items.append(item);
        }
        return result(true, {{QStringLiteral("items"), items}});
    }
    Q_INVOKABLE QString readTextFile(const QString &path) {
        const QString target = safePath(widgetDir_, path);
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
    Q_INVOKABLE QString getAudioInfo() { return audioResult(false); }
    Q_INVOKABLE QString getAudioLevel() { return audioResult(true); }
    Q_INVOKABLE QString getAudioSpectrum() { return audioResult(true); }

private:
    QString result(bool ok, const QJsonObject &extra) const {
        QJsonObject o = extra;
        o[QStringLiteral("ok")] = ok;
        return jsonString(o);
    }
    QString audioResult(bool withBands) const {
        const QJsonObject a = state_.value(QStringLiteral("audio")).toObject();
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

    QJsonObject cfg_;
    QJsonObject state_;
    QJsonObject store_;
    QString widgetDir_;
    QString storePath_;
    QWebEngineView *view_ = nullptr;
};

// ------------------------------------------------------------------
// View: transparent QWebEngineView whose right-clicks are forwarded
// to the host (the Python build swallowed WebEngine's own menu the
// same way, via event filters on the view's children).
// ------------------------------------------------------------------
class JsView : public QWebEngineView {
    Q_OBJECT
public:
    JsView(QWidget *parent, LdsJsContextMenuFn menuFn, void *menuCtx)
        : QWebEngineView(parent), menuFn_(menuFn), menuCtx_(menuCtx) {
        setContextMenuPolicy(Qt::NoContextMenu);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setStyleSheet(QStringLiteral("background: transparent;"));
        installFilters();
        for (int delay : {0, 100, 500, 1200})
            QTimer::singleShot(delay, this, [this] { installFilters(); });
        connect(this, &QWebEngineView::loadFinished, this, [this] { installFilters(); });
    }

    void installFilters() {
        QSet<QObject *> targets;
        targets.insert(this);
        if (focusProxy()) targets.insert(focusProxy());
        for (QObject *child : findChildren<QObject *>()) targets.insert(child);
        for (QObject *obj : targets) {
            if (!filtered_.contains(obj)) {
                obj->installEventFilter(this);
                filtered_.insert(obj);
            }
        }
    }

protected:
    void contextMenuEvent(QContextMenuEvent *e) override {
        e->accept();
        forwardMenu(e->globalPos());
    }
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == QEvent::ContextMenu) {
            event->accept();
            if (auto *ce = static_cast<QContextMenuEvent *>(event)) forwardMenu(ce->globalPos());
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::RightButton) {
                forwardMenu(me->globalPosition().toPoint());
                return true;
            }
        }
        return QWebEngineView::eventFilter(obj, event);
    }

private:
    void forwardMenu(const QPoint &globalPos) {
        if (menuFn_) menuFn_(menuCtx_, globalPos.x(), globalPos.y());
    }
    LdsJsContextMenuFn menuFn_;
    void *menuCtx_;
    QSet<QObject *> filtered_;
};

// One shared off-the-record profile for all views.
QWebEngineProfile *sharedProfile() {
    static QWebEngineProfile *profile = nullptr;
    if (!profile) {
        profile = new QWebEngineProfile; // no storage name: off-the-record
        profile->setHttpCacheType(QWebEngineProfile::NoCache);
        profile->setHttpCacheMaximumSize(0);
        profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
        profile->setSpellCheckEnabled(false);
    }
    return profile;
}

struct ViewState {
    JsView *view = nullptr;
    LdsApi *api = nullptr;
    QJsonObject cfg;
    QString lastSourceKey;
};
QHash<void *, ViewState *> g_views;

void loadContent(ViewState *st, bool force) {
    const QJsonObject &cfg = st->cfg;
    const QString widgetDir = cfg.value(QStringLiteral("widgetDir")).toString();
    const QString mode = cfg.value(QStringLiteral("mode")).toString(QStringLiteral("inline"));
    QString html, sourceKey;
    QUrl baseUrl = QUrl::fromLocalFile(widgetDir.endsWith(QLatin1Char('/')) ? widgetDir
                                                                            : widgetDir + QLatin1Char('/'));
    if (mode == QLatin1String("package")) {
        const QString entry = cfg.value(QStringLiteral("entry")).toString(QStringLiteral("index.html"));
        const QString entryPath = safePath(widgetDir, entry);
        QFile f(entryPath);
        if (entryPath.isEmpty() || !f.exists()) {
            html = buildDocument(QStringLiteral(
                "<div style='padding:14px;border:1px solid rgba(255,255,255,.2);border-radius:14px;"
                "background:rgba(20,24,32,.72);color:white;font-family:Segoe UI,sans-serif;'>"
                "<h3 style='margin:0 0 8px;color:#FFCC66;'>JSHTML Package Error</h3>"
                "<div>entry HTML が見つかりません: ") + entry + QStringLiteral("</div></div>"));
            sourceKey = QStringLiteral("package-error");
        } else {
            if (f.open(QIODevice::ReadOnly)) html = buildDocument(QString::fromUtf8(f.readAll()));
            const QFileInfo fi(entryPath);
            sourceKey = QStringLiteral("package:%1:%2:%3")
                            .arg(entryPath)
                            .arg(double(fi.lastModified().toMSecsSinceEpoch()))
                            .arg(double(fi.size()));
        }
    } else {
        QString raw = cfg.value(QStringLiteral("text")).toString();
        if (raw.isEmpty()) raw = QString::fromUtf8(kDefaultHtml);
        html = buildDocument(raw);
        sourceKey = QStringLiteral("inline:") + html;
    }
    if (!force && st->lastSourceKey == sourceKey) return;
    st->lastSourceKey = sourceKey;
    st->view->setHtml(html, baseUrl);
}

QJsonObject parseCfg(const char *cfgJsonUtf8) {
    return QJsonDocument::fromJson(QByteArray(cfgJsonUtf8 ? cfgJsonUtf8 : "")).object();
}

} // namespace

extern "C" {

Q_DECL_EXPORT void *lds_js_create(QWidget *parent, const char *cfgJsonUtf8,
                                  LdsJsContextMenuFn menuFn, void *menuCtx) {
    auto *view = new JsView(parent, menuFn, menuCtx);
    auto *page = new QWebEnginePage(sharedProfile(), view);
    page->setBackgroundColor(Qt::transparent);
    view->setPage(page);
    QWebEngineSettings *settings = page->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

    auto *api = new LdsApi(view);
    auto *channel = new QWebChannel(view);
    channel->registerObject(QStringLiteral("LDSApi"), api);
    page->setWebChannel(channel);

    auto *st = new ViewState;
    st->view = view;
    st->api = api;
    st->cfg = parseCfg(cfgJsonUtf8);
    api->bind(st->cfg, view);
    g_views.insert(view, st);
    QObject::connect(view, &QObject::destroyed, [view] {
        delete g_views.take(view);
    });
    loadContent(st, true);
    return view;
}

Q_DECL_EXPORT void lds_js_reload(void *view, const char *cfgJsonUtf8) {
    ViewState *st = g_views.value(view);
    if (!st) return;
    st->cfg = parseCfg(cfgJsonUtf8);
    st->api->bind(st->cfg, st->view);
    loadContent(st, true);
}

Q_DECL_EXPORT void lds_js_push_state(void *view, const char *stateJsonUtf8) {
    ViewState *st = g_views.value(view);
    if (!st) return;
    st->api->setState(QJsonDocument::fromJson(QByteArray(stateJsonUtf8 ? stateJsonUtf8 : "")).object());
}

Q_DECL_EXPORT void lds_js_set_mouse_transparent(void *view, int on) {
    ViewState *st = g_views.value(view);
    if (!st) return;
    st->view->setAttribute(Qt::WA_TransparentForMouseEvents, on != 0);
}

} // extern "C"

#include "JsHtmlPlugin.moc"
