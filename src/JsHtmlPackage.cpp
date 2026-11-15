#include "JsHtmlPackage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#ifdef LDS_HAVE_ZLIB
#include <zlib.h>
#endif

namespace JsHtmlPackage {

namespace {

QString safeIdentifier(QString value) {
    value = value.trimmed();
    QString out;
    for (const QChar ch : value) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
            out += ch;
    }
    return out.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : out;
}

quint16 u16(const QByteArray &b, qsizetype off) {
    return quint16(quint8(b[off])) | (quint16(quint8(b[off + 1])) << 8);
}
quint32 u32(const QByteArray &b, qsizetype off) {
    return quint32(quint8(b[off])) | (quint32(quint8(b[off + 1])) << 8) |
           (quint32(quint8(b[off + 2])) << 16) | (quint32(quint8(b[off + 3])) << 24);
}

bool isSafeZipMemberName(const QString &name) {
    if (name.isEmpty()) return false;
    const QString normalized = QString(name).replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (normalized.startsWith(QLatin1Char('/')) || normalized.contains(QLatin1Char(':')))
        return false;
    for (const QString &part : normalized.split(QLatin1Char('/')))
        if (part == QLatin1String("..")) return false;
    return true;
}

#ifdef LDS_HAVE_ZLIB
bool inflateRaw(const QByteArray &in, QByteArray *out, quint32 expectedSize) {
    out->clear();
    out->resize(qsizetype(expectedSize));
    z_stream zs{};
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.constData()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef *>(out->data());
    zs.avail_out = uInt(out->size());
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    const int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK) return false;
    out->resize(qsizetype(zs.total_out));
    return zs.total_out == expectedSize;
}
#endif

} // namespace

bool ensureFields(WidgetConfig &cfg) {
    bool changed = false;
    auto fill = [&](const char *key, const QString &def) {
        if (field(cfg, key).isEmpty()) { setField(cfg, key, def); changed = true; }
    };
    fill("jshtml_mode", QStringLiteral("inline"));
    fill("jshtml_entry", QStringLiteral("index.html"));
    fill("jshtml_permissions_json", QStringLiteral("{}"));
    fill("jshtml_instance_id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString sanitized = safeIdentifier(field(cfg, "jshtml_instance_id"));
    if (sanitized != field(cfg, "jshtml_instance_id")) {
        setField(cfg, "jshtml_instance_id", sanitized);
        changed = true;
    }
    return changed;
}

QString field(const WidgetConfig &cfg, const char *key) {
    return cfg.extra.value(QLatin1String(key)).toString();
}

void setField(WidgetConfig &cfg, const char *key, const QString &value) {
    cfg.extra.insert(QLatin1String(key), value);
}

QString baseDir() {
    const QString dir = QDir::homePath() + QStringLiteral("/LiteDesktopStudio_jshtml_widgets");
    QDir().mkpath(dir);
    return QDir::cleanPath(dir);
}

QString widgetDir(const WidgetConfig &cfg) {
    const QString base = baseDir();
    const QString id = safeIdentifier(field(cfg, "jshtml_instance_id"));
    const QString dir = QDir::cleanPath(base + QLatin1Char('/') + id);
    if (dir != base && !dir.startsWith(base + QLatin1Char('/'))) return QString();
    QDir().mkpath(dir + QStringLiteral("/assets"));
    QDir().mkpath(dir + QStringLiteral("/data"));
    return dir;
}

QString safePath(const QString &root, const QString &relative) {
    const QString rootClean = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    QString rel = relative;
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
    if (rel.contains(QLatin1Char(':'))) return QString();
    const QString target = QDir::cleanPath(rootClean + QLatin1Char('/') + rel);
    if (target != rootClean && !target.startsWith(rootClean + QLatin1Char('/')))
        return QString();
    // When the target exists, also verify through symlinks.
    const QString canonical = QFileInfo(target).canonicalFilePath();
    if (!canonical.isEmpty() && canonical != rootClean &&
        !canonical.startsWith(rootClean + QLatin1Char('/')))
        return QString();
    return target;
}

bool extractZip(const QString &zipPath, const QString &destDir, QString *error) {
    auto fail = [&](const QString &msg) {
        if (error) *error = msg;
        return false;
    };
    QFile f(zipPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return fail(QObject::tr("ZIPファイルが存在しません。"));
    const QByteArray data = f.readAll();
    f.close();

    // End of Central Directory: scan the tail for PK\x05\x06.
    qsizetype eocd = -1;
    const qsizetype tail = qMin<qsizetype>(data.size(), 65557);
    for (qsizetype i = data.size() - 22; i >= data.size() - tail && i >= 0; --i) {
        if (u32(data, i) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd < 0) return fail(QObject::tr("ZIPの終端レコードが見つかりません。"));
    const quint32 count = u16(data, eocd + 10);
    const quint32 cdSize = u32(data, eocd + 12);
    const quint32 cdOff = u32(data, eocd + 16);
    if (count > 1000) return fail(QObject::tr("ZIP内のファイル数が多すぎます。"));
    if (qsizetype(cdOff) + cdSize > data.size())
        return fail(QObject::tr("ZIPの中央ディレクトリが壊れています。"));

    struct Entry { QString name; quint16 method; quint32 comp, uncomp, localOff; };
    QVector<Entry> entries;
    quint64 total = 0;
    qsizetype p = cdOff;
    for (quint32 i = 0; i < count; ++i) {
        if (p + 46 > data.size() || u32(data, p) != 0x02014b50u)
            return fail(QObject::tr("ZIPの中央ディレクトリが壊れています。"));
        Entry e;
        e.method = u16(data, p + 10);
        e.comp = u32(data, p + 20);
        e.uncomp = u32(data, p + 24);
        const quint16 nameLen = u16(data, p + 28);
        const quint16 extraLen = u16(data, p + 30);
        const quint16 commentLen = u16(data, p + 32);
        e.localOff = u32(data, p + 42);
        e.name = QString::fromUtf8(data.mid(p + 46, nameLen));
        if (!isSafeZipMemberName(e.name))
            return fail(QObject::tr("危険なパスが含まれています: %1").arg(e.name));
        if (e.uncomp > 20u * 1024 * 1024)
            return fail(QObject::tr("ファイルサイズが大きすぎます: %1").arg(e.name));
        total += e.uncomp;
        if (total > 50ull * 1024 * 1024)
            return fail(QObject::tr("ZIPの総展開サイズが大きすぎます。"));
        entries.push_back(e);
        p += 46 + nameLen + extraLen + commentLen;
    }

    const QString destClean = QDir::cleanPath(QFileInfo(destDir).absoluteFilePath());
    QDir().mkpath(destClean);
    for (const Entry &e : entries) {
        const QString rel = QString(e.name).replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString target = safePath(destClean, rel);
        if (target.isEmpty())
            return fail(QObject::tr("展開先が範囲外です: %1").arg(e.name));
        if (rel.endsWith(QLatin1Char('/'))) { QDir().mkpath(target); continue; }
        if (qsizetype(e.localOff) + 30 > data.size() || u32(data, e.localOff) != 0x04034b50u)
            return fail(QObject::tr("ZIPのローカルヘッダが壊れています: %1").arg(e.name));
        const quint16 lName = u16(data, e.localOff + 26);
        const quint16 lExtra = u16(data, e.localOff + 28);
        const qsizetype dataOff = qsizetype(e.localOff) + 30 + lName + lExtra;
        if (dataOff + e.comp > data.size())
            return fail(QObject::tr("ZIPのデータが壊れています: %1").arg(e.name));
        const QByteArray packed = data.mid(dataOff, e.comp);
        QByteArray raw;
        if (e.method == 0) {
            raw = packed;
        } else if (e.method == 8) {
#ifdef LDS_HAVE_ZLIB
            if (!inflateRaw(packed, &raw, e.uncomp))
                return fail(QObject::tr("展開に失敗しました: %1").arg(e.name));
#else
            return fail(QObject::tr("このビルドでは deflate 圧縮の ZIP を展開できません: %1").arg(e.name));
#endif
        } else {
            return fail(QObject::tr("未対応の圧縮形式です: %1").arg(e.name));
        }
        if (quint32(raw.size()) != e.uncomp)
            return fail(QObject::tr("展開サイズが一致しません: %1").arg(e.name));
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly) || out.write(raw) != raw.size())
            return fail(QObject::tr("書き込みに失敗しました: %1").arg(e.name));
    }
    return true;
}

bool importPackage(WidgetConfig &cfg, const QString &zipPath, QString *error) {
    ensureFields(cfg);
    const QString dir = widgetDir(cfg);
    if (dir.isEmpty()) {
        if (error) *error = QObject::tr("JSHTML ウィジェットのディレクトリが不正です。");
        return false;
    }
    // Clear the previous package, keeping data/ (the API config store).
    QDir d(dir);
    for (const QFileInfo &child : d.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        if (child.fileName() == QLatin1String("data")) continue;
        if (child.isDir()) QDir(child.absoluteFilePath()).removeRecursively();
        else QFile::remove(child.absoluteFilePath());
    }
    if (!extractZip(zipPath, dir, error)) return false;

    const QString manifestPath = dir + QStringLiteral("/widget.json");
    if (QFile::exists(manifestPath)) {
        QFile mf(manifestPath);
        if (mf.open(QIODevice::ReadOnly)) {
            QJsonParseError perr;
            const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll(), &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
                if (error) *error = QObject::tr("widget.json を読み込めません: %1").arg(perr.errorString());
                return false;
            }
            const QJsonObject m = doc.object();
            const QString entry = m.value(QStringLiteral("entry")).toString(QStringLiteral("index.html"));
            if (!safePath(dir, entry).isEmpty()) setField(cfg, "jshtml_entry", entry);
            const QString name = m.value(QStringLiteral("name")).toString(
                m.value(QStringLiteral("id")).toString(field(cfg, "jshtml_package_name")));
            if (!name.isEmpty()) setField(cfg, "jshtml_package_name", name);
            const QString version = m.value(QStringLiteral("version")).toString();
            if (!version.isEmpty()) setField(cfg, "jshtml_package_version", version);
            if (m.value(QStringLiteral("permissions")).isObject()) {
                setField(cfg, "jshtml_permissions_json",
                         QString::fromUtf8(QJsonDocument(m.value(QStringLiteral("permissions")).toObject())
                                               .toJson(QJsonDocument::Compact)));
            }
        }
    }

    QString entryPath = safePath(dir, field(cfg, "jshtml_entry"));
    if (entryPath.isEmpty() || !QFileInfo(entryPath).isFile()) {
        const QString fallback = dir + QStringLiteral("/index.html");
        if (QFileInfo(fallback).isFile()) {
            setField(cfg, "jshtml_entry", QStringLiteral("index.html"));
            entryPath = fallback;
        } else {
            if (error) *error = QObject::tr("index.html または widget.json の entry が見つかりません。");
            return false;
        }
    }
    setField(cfg, "jshtml_mode", QStringLiteral("package"));
    return true;
}

} // namespace JsHtmlPackage
