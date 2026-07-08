#include "stats_client.hpp"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

StatsClient::StatsClient(QObject* parent) : QObject(parent) {}

void StatsClient::fetchStats(const QString& host, quint16 port) {
    QUrl url(QString("http://%1:%2/stats").arg(host).arg(port));
    QNetworkReply* reply = manager_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            reply->deleteLater();
            return;
        }
        QJsonParseError error{};
        QJsonDocument doc = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            emit errorOccurred("invalid /stats JSON response");
            reply->deleteLater();
            return;
        }
        emit statsReceived(doc.object());
        reply->deleteLater();
    });
}

void StatsClient::fetchStatsText(const QString& host, quint16 port) {
    QUrl url(QString("http://%1:%2/stats").arg(host).arg(port));
    QNetworkReply* reply = manager_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            reply->deleteLater();
            return;
        }
        emit statsTextReceived(QString::fromUtf8(body));
        reply->deleteLater();
    });
}

void StatsClient::fetchMetrics(const QString& host, quint16 port) {
    QUrl url(QString("http://%1:%2/metrics").arg(host).arg(port));
    QNetworkReply* reply = manager_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            reply->deleteLater();
            return;
        }
        emit metricsReceived(QString::fromUtf8(body));
        reply->deleteLater();
    });
}

void StatsClient::fetchHealthz(const QString& host, quint16 port) {
    QUrl url(QString("http://%1:%2/healthz").arg(host).arg(port));
    QNetworkReply* reply = manager_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            reply->deleteLater();
            return;
        }
        emit healthReceived(QString::fromUtf8(body));
        reply->deleteLater();
    });
}

void StatsClient::fetchReadyz(const QString& host, quint16 port) {
    QUrl url(QString("http://%1:%2/readyz").arg(host).arg(port));
    QNetworkReply* reply = manager_.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(reply->errorString());
            reply->deleteLater();
            return;
        }
        emit readinessReceived(QString::fromUtf8(body));
        reply->deleteLater();
    });
}
