#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonObject>

class StatsClient : public QObject {
    Q_OBJECT

public:
    explicit StatsClient(QObject* parent = nullptr);

public slots:
    void fetchStats(const QString& host, quint16 port);
    void fetchMetrics(const QString& host, quint16 port);

signals:
    void statsReceived(const QJsonObject& stats);
    void metricsReceived(const QString& metrics);
    void errorOccurred(const QString& error);

private:
    QNetworkAccessManager manager_;
};
