#include "MediaHelperClient.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

static QString helperExePath() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("MediaHelper/LiteDesktopStudio-MediaHelper.exe"));
}

MediaHelperClient *MediaHelperClient::instance() {
    static MediaHelperClient *inst = new MediaHelperClient(qApp);
    return inst;
}

MediaHelperClient::MediaHelperClient(QObject *parent) : QObject(parent) {}
MediaHelperClient::~MediaHelperClient() { stopHelper(); }

bool MediaHelperClient::helperPresent() const {
    return QFileInfo::exists(helperExePath());
}

static QString controlHelperExePath() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("MediaControlHelper/LiteDesktopStudio-MediaControlHelper.exe"));
}

bool MediaHelperClient::controlHelperPresent() const {
    return QFileInfo::exists(controlHelperExePath());
}

void MediaHelperClient::sendTransportCommand(const QString &cmd) {
    const QString exe = controlHelperExePath();
    if (!QFileInfo::exists(exe)) return;
    auto *proc = new QProcess(this);
#ifdef Q_OS_WIN
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            proc, [proc](int code, QProcess::ExitStatus) {
        qDebug() << "MediaControlHelper:" << QString::fromUtf8(proc->readAllStandardOutput()).trimmed()
                 << "exit" << code;
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, proc, [proc](QProcess::ProcessError e) {
        qDebug() << "MediaControlHelper: failed to start:" << e;
        proc->deleteLater();
    });
    proc->start(exe, {cmd});
}

void MediaHelperClient::acquire() {
    if (++refs_ == 1) startHelper();
}

void MediaHelperClient::release() {
    if (refs_ > 0 && --refs_ == 0) stopHelper();
}

void MediaHelperClient::startHelper() {
    if (proc_) return;
    const QString exe = helperExePath();
    if (!QFileInfo::exists(exe)) {
        if (!announced_) {
            announced_ = true;
            qDebug() << "MediaHelperClient: helper not found at" << exe
                     << "- media metadata disabled (widget controls still work)";
        }
        return;
    }
    proc_ = new QProcess(this);
#ifdef Q_OS_WIN
    // The helper is a console-subsystem exe; without this flag a console
    // window would flash up every time it starts.
    proc_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
        buffer_ += proc_->readAllStandardOutput();
        int nl;
        while ((nl = buffer_.indexOf('\n')) >= 0) {
            const QByteArray line = buffer_.left(nl).trimmed();
            buffer_.remove(0, nl + 1);
            if (!line.isEmpty()) handleLine(line);
        }
    });
    connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus st) {
        qDebug() << "MediaHelperClient: helper exited" << code << st;
        proc_->deleteLater();
        proc_ = nullptr;
        if (info_.available) {
            info_.available = false;
            emit mediaChanged();
        }
    });
    proc_->start(exe, QStringList());
    if (!proc_->waitForStarted(3000))
        qDebug() << "MediaHelperClient: failed to start helper:" << proc_->errorString();
    else
        qDebug() << "MediaHelperClient: helper started";
}

void MediaHelperClient::stopHelper() {
    if (!proc_) return;
    QProcess *proc = proc_;
    proc_ = nullptr;
    disconnect(proc, nullptr, this, nullptr);
    proc->closeWriteChannel(); // helper exits by itself on stdin EOF
    if (!proc->waitForFinished(900)) {
        proc->kill();
        proc->waitForFinished(500);
    }
    proc->deleteLater();
    if (info_.available || info_.session) {
        info_ = MediaInfo();
        emit mediaChanged();
    }
}

void MediaHelperClient::handleLine(const QByteArray &line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) return;
    const QJsonObject o = doc.object();
    const QString type = o.value(QStringLiteral("type")).toString();
    bool changed = false;
    if (type == QLatin1String("ready")) {
        changed = !info_.available;
        info_.available = true;
    } else if (type == QLatin1String("error")) {
        qDebug() << "MediaHelperClient: helper reports" << o.value(QStringLiteral("error")).toString();
        changed = info_.available;
        info_.available = false;
    } else if (type == QLatin1String("timeline")) {
        const qint64 pos = o.value(QStringLiteral("position_ms")).toVariant().toLongLong();
        const qint64 dur = o.value(QStringLiteral("duration_ms")).toVariant().toLongLong();
        const QString st = o.value(QStringLiteral("status")).toString();
        changed = pos != info_.positionMs || dur != info_.durationMs || st != info_.status;
        info_.positionMs = pos;
        info_.durationMs = dur;
        if (!st.isEmpty()) info_.status = st;
    } else if (type == QLatin1String("media")) {
        MediaInfo next;
        next.available = true;
        next.session = o.value(QStringLiteral("session")).toBool();
        next.title = o.value(QStringLiteral("title")).toString();
        next.artist = o.value(QStringLiteral("artist")).toString();
        next.album = o.value(QStringLiteral("album")).toString();
        next.app = o.value(QStringLiteral("app")).toString();
        next.status = o.value(QStringLiteral("status")).toString();
        next.positionMs = o.value(QStringLiteral("position_ms")).toVariant().toLongLong();
        next.durationMs = o.value(QStringLiteral("duration_ms")).toVariant().toLongLong();
        const QString b64 = o.value(QStringLiteral("thumbnail_b64")).toString();
        if (!b64.isEmpty()) {
            const QByteArray bytes = QByteArray::fromBase64(b64.toLatin1());
            next.artwork = QImage::fromData(bytes);
        }
        changed = next.session != info_.session || next.title != info_.title ||
                  next.artist != info_.artist || next.album != info_.album ||
                  next.status != info_.status ||
                  next.artwork.cacheKey() != info_.artwork.cacheKey();
        if (!next.title.isEmpty() && next.title != info_.title)
            qDebug() << "MediaHelperClient: now playing:" << next.title << "-" << next.artist;
        info_ = next;
    }
    if (changed) emit mediaChanged();
}
