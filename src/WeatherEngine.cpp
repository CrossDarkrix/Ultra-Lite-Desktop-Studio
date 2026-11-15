#include "WeatherEngine.h"
#include <QDateTime>
#include <QHash>
#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

WeatherEngine::WeatherEngine(QObject *parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)), timer_(new QTimer(this)) {
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!fetchInFlight_ && (forceFetch_ || now - lastFetchMs_ >= kRefreshMs)) fetch();
    });
    timer_->start();
}

void WeatherEngine::setLocation(const QString &location) {
    const QString loc = location.trimmed();
    if (loc == location_) return;
    location_ = loc;
    data_ = WeatherData{};
    forceFetch_ = true;
    emit updated();
}

void WeatherEngine::fetch() {
    forceFetch_ = false;
    fetchInFlight_ = true;
    lastFetchMs_ = QDateTime::currentMSecsSinceEpoch();
    QString url = QStringLiteral("https://wttr.in/");
    if (!location_.isEmpty())
        url += QString::fromUtf8(QUrl::toPercentEncoding(location_));
    url += QStringLiteral("?format=j1&lang=ja");
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("WeatherWidget"));
    req.setTransferTimeout(8000);
    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        fetchInFlight_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            data_.error = reply->errorString();
            data_.updatedAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
            reply->deleteLater();
            emit updated();
            return;
        }
        parse(reply->readAll());
        reply->deleteLater();
        emit updated();
    });
}

static QString firstValue(const QJsonObject &obj, const char *key) {
    const QJsonArray a = obj.value(QLatin1String(key)).toArray();
    if (a.isEmpty()) return {};
    return a.at(0).toObject().value(QStringLiteral("value")).toString();
}

void WeatherEngine::parse(const QByteArray &raw) {
    const QJsonObject root = QJsonDocument::fromJson(raw).object();
    auto firstObj = [&root](const char *key) {
        const QJsonArray a = root.value(QLatin1String(key)).toArray();
        return a.isEmpty() ? QJsonObject{} : a.at(0).toObject();
    };
    const QJsonObject current = firstObj("current_condition");
    const QJsonObject areaObj = firstObj("nearest_area");
    if (current.isEmpty()) {
        data_.error = QStringLiteral("empty response");
        data_.updatedAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
        return;
    }
    WeatherData d;
    d.temperature = current.value(QStringLiteral("temp_C")).toString(QStringLiteral("--"));
    d.feelsLike = current.value(QStringLiteral("FeelsLikeC")).toString(QStringLiteral("--"));
    d.humidity = current.value(QStringLiteral("humidity")).toString(QStringLiteral("--"));
    d.windKmph = current.value(QStringLiteral("windspeedKmph")).toString(QStringLiteral("--"));
    const QString code = current.value(QStringLiteral("weatherCode")).toString();
    const QString descEn = firstValue(current, "weatherDesc");
    d.icon = iconFor(code, descEn);
    d.description = translateDesc(descEn);
    d.area = firstValue(areaObj, "areaName");
    d.country = firstValue(areaObj, "country");
    const QJsonArray days = root.value(QStringLiteral("weather")).toArray();
    for (int i = 0; i < std::min(3, int(days.size())); ++i) {
        const QJsonObject day = days.at(i).toObject();
        WeatherForecastDay f;
        f.date = day.value(QStringLiteral("date")).toString();
        f.maxTemp = day.value(QStringLiteral("maxtempC")).toString(QStringLiteral("--"));
        f.minTemp = day.value(QStringLiteral("mintempC")).toString(QStringLiteral("--"));
        const QJsonArray hourly = day.value(QStringLiteral("hourly")).toArray();
        if (!hourly.isEmpty()) {
            const QJsonObject mid = hourly.at(hourly.size() / 2).toObject();
            const QString dcode = mid.value(QStringLiteral("weatherCode")).toString();
            const QString ddesc = firstValue(mid, "weatherDesc");
            f.icon = iconFor(dcode, ddesc);
            f.desc = translateDesc(ddesc);
        }
        d.forecast.append(f);
    }
    d.updatedAt = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    d.loaded = true;
    data_ = d;
}

QString WeatherEngine::iconFor(const QString &code, const QString &descEn) {
    // Same weatherCode -> icon mapping as the Python build (subset by group).
    static const QHash<QString, QString> map = {
        {"113", "☀"}, {"116", "⛅"}, {"119", "☁"}, {"122", "☁"}, {"143", "🌫"},
        {"176", "🌦"}, {"179", "🌨"}, {"182", "🌨"}, {"185", "🌧"}, {"200", "⛈"},
        {"227", "🌨"}, {"230", "❄"}, {"248", "🌫"}, {"260", "🌫"}, {"263", "🌦"},
        {"266", "🌧"}, {"281", "🌧"}, {"284", "🌧"}, {"293", "🌦"}, {"296", "🌧"},
        {"299", "🌧"}, {"302", "🌧"}, {"305", "🌧"}, {"308", "🌧"}, {"311", "🌧"},
        {"314", "🌧"}, {"317", "🌨"}, {"320", "🌨"}, {"323", "🌨"}, {"326", "🌨"},
        {"329", "❄"}, {"332", "❄"}, {"335", "❄"}, {"338", "❄"}, {"350", "🌨"},
        {"353", "🌦"}, {"356", "🌧"}, {"359", "🌧"}, {"362", "🌨"}, {"365", "🌨"},
        {"368", "🌨"}, {"371", "❄"}, {"374", "🌨"}, {"377", "🌨"}, {"386", "⛈"},
        {"389", "⛈"}, {"392", "⛈"}, {"395", "⛈"},
    };
    const auto it = map.constFind(code);
    if (it != map.constEnd()) return it.value();
    const QString t = descEn.toLower();
    if (t.contains(QLatin1String("thunder"))) return QStringLiteral("⛈");
    if (t.contains(QLatin1String("snow"))) return QStringLiteral("❄");
    if (t.contains(QLatin1String("rain")) || t.contains(QLatin1String("drizzle"))) return QStringLiteral("🌧");
    if (t.contains(QLatin1String("fog")) || t.contains(QLatin1String("mist"))) return QStringLiteral("🌫");
    if (t.contains(QLatin1String("sunny")) || t.contains(QLatin1String("clear"))) return QStringLiteral("☀");
    if (t.contains(QLatin1String("partly"))) return QStringLiteral("⛅");
    return QStringLiteral("☁");
}

QString WeatherEngine::translateDesc(const QString &descEn) {
    static const QHash<QString, QString> table = {
        {"Sunny", "晴れ"}, {"Clear", "快晴"}, {"Partly cloudy", "一部曇り"},
        {"Cloudy", "曇り"}, {"Overcast", "厚い曇り"}, {"Mist", "霧"}, {"Fog", "霧"},
        {"Patchy rain nearby", "所により雨"}, {"Light rain", "小雨"},
        {"Moderate rain", "雨"}, {"Heavy rain", "強い雨"}, {"Light drizzle", "霧雨"},
        {"Patchy light drizzle", "所により霧雨"}, {"Patchy light rain", "所により小雨"},
        {"Light rain shower", "弱いにわか雨"}, {"Moderate or heavy rain shower", "強いにわか雨"},
        {"Thunderstorm", "雷雨"}, {"Patchy snow nearby", "所により雪"},
        {"Light snow", "小雪"}, {"Moderate snow", "雪"}, {"Heavy snow", "大雪"},
        {"Blizzard", "吹雪"},
    };
    return table.value(descEn, descEn.isEmpty() ? QStringLiteral("--") : descEn);
}
