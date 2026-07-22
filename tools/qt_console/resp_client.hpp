#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QChar>
#include <optional>

class RespClient : public QObject {
    Q_OBJECT

public:
    explicit RespClient(QObject* parent = nullptr);

    bool isConnected() const;
    QString lastError() const;

public slots:
    void connectToServer(const QString& host, quint16 port);
    void disconnectFromServer();
    void sendCommand(const QStringList& parts);

signals:
    void connected();
    void disconnected();
    void responseReceived(const QString& response);
    void errorOccurred(const QString& error);

private slots:
    void onReadyRead();

private:
    QByteArray encodeCommand(const QStringList& parts) const;
    std::optional<QString> tryParseResponse();
    QString parseValue(int& pos, bool& ok, int depth = 0, bool in_array = false) const;
    QString parseLineValue(int& pos, QChar prefix, const QString& label, bool& ok) const;

    QTcpSocket socket_;
    QByteArray buffer_;
    QString last_error_;
};
