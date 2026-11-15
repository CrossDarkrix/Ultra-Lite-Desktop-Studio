#include "JsHtmlWidget.h"
#include "AudioEngine.h"
#include "JsHtmlPackage.h"
#include "Platform.h"
#include "SystemMonitor.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QStringList>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>
#include <QUrl>
#include <cmath>

namespace {
// contentFont lives in Widgets.cpp; replicate the tiny helper here
// to keep this translation unit self-contained.
void contentFont(QPainter &p, int px) {
    QFont f = p.font();
    f.setPixelSize(qMax(9, px));
    p.setFont(f);
}
} // namespace

JsHtmlWidget::Plugin &JsHtmlWidget::plugin() {
    static Plugin p;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const QString dir = QCoreApplication::applicationDirPath();
        p.lib.setFileName(dir + QStringLiteral("/LdsJsHtml"));
        if (!p.lib.load()) {
            // MinGW builds before v0.14.4 produced libLdsJsHtml.dll
            // (GNU "lib" prefix) while the host asked for
            // LdsJsHtml.dll; accept the prefixed name too so an
            // already-built plugin still loads.
            const QString firstError = p.lib.errorString();
            p.lib.setFileName(dir + QStringLiteral("/libLdsJsHtml"));
            if (!p.lib.load()) p.status = firstError;
        }
        if (p.lib.isLoaded()) {
            p.create = reinterpret_cast<LdsJsCreateFn>(p.lib.resolve(ldsJsCreateSymbol()));
            p.reload = reinterpret_cast<LdsJsReloadFn>(p.lib.resolve(ldsJsReloadSymbol()));
            p.pushState = reinterpret_cast<LdsJsPushStateFn>(p.lib.resolve(ldsJsPushStateSymbol()));
            p.setMouseTransparent = reinterpret_cast<LdsJsSetMouseTransparentFn>(
                p.lib.resolve(ldsJsSetMouseTransparentSymbol()));
            p.ok = p.create && p.reload && p.pushState && p.setMouseTransparent;
            if (!p.ok) {
                QStringList missing;
                if (!p.create) missing << QString::fromLatin1(ldsJsCreateSymbol());
                if (!p.reload) missing << QString::fromLatin1(ldsJsReloadSymbol());
                if (!p.pushState) missing << QString::fromLatin1(ldsJsPushStateSymbol());
                if (!p.setMouseTransparent) missing << QString::fromLatin1(ldsJsSetMouseTransparentSymbol());
                p.status = QStringLiteral("symbols missing: ") + missing.join(QStringLiteral(", "));
                p.lib.unload();
            }
        } else if (p.status.isEmpty()) {
            // The loader's own words (usually "Cannot load library
            // ...: The specified module could not be found" when a
            // dependency DLL is absent, or a path error when the
            // plugin DLL itself is not next to the exe).
            p.status = p.lib.errorString();
        }
        debugLog(QStringLiteral("jshtml: plugin %1")
                     .arg(p.ok ? QStringLiteral("loaded")
                               : QStringLiteral("unavailable: ") + p.status));
    }
    return p;
}

QString JsHtmlWidget::pluginStatus() {
    Plugin &p = plugin();
    return p.ok ? QString() : p.status;
}

void JsHtmlWidget::contextMenuThunk(void *ctx, int globalX, int globalY) {
    auto *self = static_cast<JsHtmlWidget *>(ctx);
    if (self) self->showContextMenu(QPoint(globalX, globalY));
}

JsHtmlWidget::JsHtmlWidget(WidgetConfig cfg, AudioEngine *audio)
    : WidgetWindow(std::move(cfg)), audio_(audio) {
    fieldsChanged_ = JsHtmlPackage::ensureFields(cfg_);
    widgetDir_ = JsHtmlPackage::widgetDir(cfg_);
    if (fieldsChanged_) {
        // attach() connects requestSave after construction; defer.
        QTimer::singleShot(0, this, [this] { emit requestSave(); });
    }
    Plugin &pl = plugin();
    if (pl.ok && !widgetDir_.isEmpty()) {
        lastCfgJson_ = cfgJson();
        view_ = static_cast<QWidget *>(pl.create(this, lastCfgJson_.constData(),
                                                  &JsHtmlWidget::contextMenuThunk, this));
        if (view_) {
            view_->setGeometry(rect());
            view_->show();
            pl.setMouseTransparent(view_, editMode() ? 1 : 0);
        }
    }
}

JsHtmlWidget::~JsHtmlWidget() = default; // view_ is a child; Qt deletes it

QByteArray JsHtmlWidget::cfgJson() const {
    QJsonObject o;
    o[QStringLiteral("title")] = cfg_.title;
    o[QStringLiteral("text")] = cfg_.text;
    o[QStringLiteral("mode")] = JsHtmlPackage::field(cfg_, "jshtml_mode");
    o[QStringLiteral("entry")] = JsHtmlPackage::field(cfg_, "jshtml_entry");
    o[QStringLiteral("packageName")] = JsHtmlPackage::field(cfg_, "jshtml_package_name");
    o[QStringLiteral("packageVersion")] = JsHtmlPackage::field(cfg_, "jshtml_package_version");
    o[QStringLiteral("instanceId")] = JsHtmlPackage::field(cfg_, "jshtml_instance_id");
    o[QStringLiteral("widgetDir")] = widgetDir_;
    if (o[QStringLiteral("mode")].toString() == QLatin1String("package")) {
        // Fold the entry file's stat into the config snapshot so a
        // package edited on disk reloads on the next tick, like the
        // Python manager's source key (path + mtime + size).
        const QString entryPath = JsHtmlPackage::safePath(
            widgetDir_, JsHtmlPackage::field(cfg_, "jshtml_entry"));
        const QFileInfo fi(entryPath);
        o[QStringLiteral("entryMtimeMs")] = fi.exists() ? double(fi.lastModified().toMSecsSinceEpoch()) : 0.0;
        o[QStringLiteral("entrySize")] = double(fi.size());
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void JsHtmlWidget::reloadPlugin() {
    Plugin &pl = plugin();
    if (!pl.ok || !view_) return;
    lastCfgJson_ = cfgJson();
    pl.reload(view_, lastCfgJson_.constData());
}

void JsHtmlWidget::onTick(const SystemSnapshot &s, const QDateTime &) {
    Plugin &pl = plugin();
    if (!pl.ok || !view_) return;
    // Config or package-file change -> reload (checked once a second).
    if (cfgJson() != lastCfgJson_) reloadPlugin();
    lastSnapshot_ = s;
    // Keep the fast audio timer in sync with the engine (v0.14.8).
    const bool running = audio_ && audio_->active();
    if (running && !audioTimer_.isActive()) {
        audioTimer_.setInterval(50);
        connect(&audioTimer_, &QTimer::timeout, this, [this] { pushState(true); });
        audioTimer_.start();
    } else if (!running && audioTimer_.isActive()) {
        audioTimer_.stop();
    }
    pushState(false);
}

void JsHtmlWidget::pushState(bool audioOnly) {
    Plugin &pl = plugin();
    if (!pl.ok || !view_) return;
    const SystemSnapshot &s = lastSnapshot_;

    QJsonObject audio;
    QVector<float> bands;
    if (audio_) bands = audio_->spectrum();
    QJsonArray arr;
    double sum = 0.0, peak = 0.0, sq = 0.0;
    for (const float v : bands) {
        const double d = qBound(0.0, double(v), 1.0);
        arr.append(d);
        sum += d;
        sq += d * d;
        peak = qMax(peak, d);
    }
    const bool running = audio_ && audio_->active();
    audio[QStringLiteral("available")] = running;
    audio[QStringLiteral("bands")] = arr;
    audio[QStringLiteral("bandCount")] = arr.size();
    audio[QStringLiteral("level")] = arr.isEmpty() ? 0.0 : sum / arr.size();
    audio[QStringLiteral("peak")] = peak;
    audio[QStringLiteral("rms")] = arr.isEmpty() ? 0.0 : std::sqrt(sq / arr.size());
#ifdef Q_OS_WIN
    audio[QStringLiteral("backend")] = QStringLiteral("wasapi-loopback");
#else
    audio[QStringLiteral("backend")] = running ? QStringLiteral("fake") : QStringLiteral("unavailable");
#endif
    audio[QStringLiteral("useFake")] = false;
    audio[QStringLiteral("running")] = running;
    audio[QStringLiteral("timestamp")] = QDateTime::currentMSecsSinceEpoch() / 1000.0;

    QJsonObject state;
    if (!audioOnly) {
        state[QStringLiteral("cpuPercent")] = s.cpuPercent < 0 ? 0.0 : s.cpuPercent;
        state[QStringLiteral("memoryPercent")] = s.memPercent;
        state[QStringLiteral("diskPercent")] = s.diskPercent;
#ifdef Q_OS_WIN
        state[QStringLiteral("platform")] = QStringLiteral("win32");
#else
        state[QStringLiteral("platform")] = QStringLiteral("linux");
#endif
    }
    state[QStringLiteral("audio")] = audio;
    pl.pushState(view_, QJsonDocument(state).toJson(QJsonDocument::Compact).constData());
}

void JsHtmlWidget::onEditModeChanged() {
    WidgetWindow::onEditModeChanged();
    Plugin &pl = plugin();
    if (pl.ok && view_) pl.setMouseTransparent(view_, editMode() ? 1 : 0);
}

void JsHtmlWidget::resizeEvent(QResizeEvent *e) {
    WidgetWindow::resizeEvent(e);
    if (view_) view_->setGeometry(rect());
}

void JsHtmlWidget::paintContent(QPainter &p, const QRect &rect) {
    if (view_) return; // the plugin view covers the whole window
    drawTitle(p, rect);
    contentFont(p, cfg_.fontSize - 2);
    p.setPen(QColor(200, 208, 216));
    const QString mode = JsHtmlPackage::field(cfg_, "jshtml_mode");
    QString msg = tr("JSHTML (%1) は任意コンポーネント「LdsJsHtml」プラグインが無いため表示できません。\n"
                     "プラグイン付きでビルドすると有効になります (パッケージのインポート等の管理は動作します)。")
                      .arg(mode.isEmpty() ? QStringLiteral("inline") : mode);
    const QString status = pluginStatus();
    if (!status.isEmpty()) msg += tr("\n詳細: %1").arg(status);
    p.drawText(rect.adjusted(10, 34, -10, -8), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, msg);
}

void JsHtmlWidget::addContextActions(QMenu &menu) {
    QAction *reloadAct = menu.addAction(tr("JSHTMLを再読み込み"));
    connect(reloadAct, &QAction::triggered, this, [this] { reloadPlugin(); });
    QAction *importAct = menu.addAction(tr("JSHTML ZIPをインポート…"));
    connect(importAct, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("JSHTML パッケージ ZIP"), QString(), tr("ZIP ファイル (*.zip)"));
        if (path.isEmpty()) return;
        QString error;
        if (JsHtmlPackage::importPackage(cfg_, path, &error)) {
            emit requestSave();
            reloadPlugin();
            QMessageBox::information(this, tr("JSHTML"),
                                     tr("パッケージをインポートしました: %1")
                                         .arg(JsHtmlPackage::field(cfg_, "jshtml_package_name")));
        } else {
            QMessageBox::warning(this, tr("JSHTML"), error);
        }
    });
    QAction *folderAct = menu.addAction(tr("JSHTMLフォルダを開く"));
    connect(folderAct, &QAction::triggered, this, [this] {
        if (!widgetDir_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(widgetDir_));
    });
    if (JsHtmlPackage::field(cfg_, "jshtml_mode") == QLatin1String("package")) {
        QAction *inlineAct = menu.addAction(tr("inlineモードに戻す"));
        connect(inlineAct, &QAction::triggered, this, [this] {
            JsHtmlPackage::setField(cfg_, "jshtml_mode", QStringLiteral("inline"));
            emit requestSave();
            reloadPlugin();
        });
    }
}
