#pragma once
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QTimer;

// Port of the Python WeatherEngine (runtime.py): wttr.in format=j1&lang=ja,
// refreshed every 600s. One engine per weather widget (the Python build
// shared one engine and let widgets fight over the location; per-widget is
// both simpler and correct, and a 10-minute HTTP fetch costs nothing).
struct WeatherForecastDay {
    QString date, maxTemp, minTemp, desc, icon;
};
struct WeatherData {
    QString temperature = QStringLiteral("--");
    QString feelsLike = QStringLiteral("--");
    QString description = QStringLiteral("Loading...");
    QString humidity = QStringLiteral("--");
    QString windKmph = QStringLiteral("--");
    QString area, country, updatedAt, error;
    QString icon = QStringLiteral("☁");
    QVector<WeatherForecastDay> forecast;
    bool loaded = false;
};

class WeatherEngine : public QObject {
    Q_OBJECT
public:
    explicit WeatherEngine(QObject *parent = nullptr);
    void setLocation(const QString &location);
    const WeatherData &data() const { return data_; }
signals:
    void updated();
private:
    void fetch();
    void parse(const QByteArray &raw);
    static QString iconFor(const QString &code, const QString &descEn);
    static QString translateDesc(const QString &descEn);

    QNetworkAccessManager *nam_;
    QTimer *timer_;
    QString location_;
    WeatherData data_;
    bool fetchInFlight_ = false;
    bool forceFetch_ = true;
    qint64 lastFetchMs_ = 0;
    static constexpr qint64 kRefreshMs = 600 * 1000;
};
