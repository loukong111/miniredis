#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QString>

class StatsClient : public QObject {
    Q_OBJECT

public:
    explicit StatsClient(QObject* parent = nullptr);

public slots:
    void fetchStats(const QString& host, quint16 port);
    void fetchStatsText(const QString& host, quint16 port);
    void fetchMetrics(const QString& host, quint16 port);
    void fetchHealthz(const QString& host, quint16 port);
    void fetchReadyz(const QString& host, quint16 port);

signals:
    void statsReceived(const QJsonObject& stats);
    void statsTextReceived(const QString& body);
    void metricsReceived(const QString& metrics);
    void healthReceived(const QString& body);
    void readinessReceived(const QString& body);
    void errorOccurred(const QString& error);

private:
    QNetworkAccessManager manager_;
};
