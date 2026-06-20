#include "resp_client.hpp"

#include <QHostAddress>

RespClient::RespClient(QObject* parent) : QObject(parent) {
    connect(&socket_, &QTcpSocket::connected, this, &RespClient::connected);
    connect(&socket_, &QTcpSocket::disconnected, this, &RespClient::disconnected);
    connect(&socket_, &QTcpSocket::readyRead, this, &RespClient::onReadyRead);
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        last_error_ = socket_.errorString();
        emit errorOccurred(last_error_);
    });
}

bool RespClient::isConnected() const {
    return socket_.state() == QAbstractSocket::ConnectedState;
}

QString RespClient::lastError() const {
    return last_error_;
}

void RespClient::connectToServer(const QString& host, quint16 port) {
    buffer_.clear();
    socket_.abort();
    socket_.connectToHost(host, port);
}

void RespClient::disconnectFromServer() {
    socket_.disconnectFromHost();
}

void RespClient::sendCommand(const QStringList& parts) {
    if (!isConnected()) {
        emit errorOccurred("RESP connection is not open");
        return;
    }
    socket_.write(encodeCommand(parts));
}

void RespClient::onReadyRead() {
    buffer_.append(socket_.readAll());
    while (true) {
        QString response = tryParseResponse();
        if (response.isEmpty()) break;
        emit responseReceived(response);
    }
}

QByteArray RespClient::encodeCommand(const QStringList& parts) const {
    QByteArray out = "*" + QByteArray::number(parts.size()) + "\r\n";
    for (const QString& part : parts) {
        QByteArray bytes = part.toUtf8();
        out += "$" + QByteArray::number(bytes.size()) + "\r\n";
        out += bytes + "\r\n";
    }
    return out;
}

QString RespClient::tryParseResponse() {
    int pos = 0;
    bool ok = true;
    QString parsed = parseValue(pos, ok);
    if (!ok) return {};
    buffer_.remove(0, pos);
    return parsed;
}

QString RespClient::parseLineValue(int& pos, QChar prefix, const QString& label, bool& ok) const {
    if (pos >= buffer_.size() || QChar(buffer_[pos]) != prefix) {
        ok = false;
        return {};
    }
    int end = buffer_.indexOf("\r\n", pos);
    if (end < 0) {
        ok = false;
        return {};
    }
    QString value = QString::fromUtf8(buffer_.mid(pos + 1, end - pos - 1));
    pos = end + 2;
    return label.isEmpty() ? value : label + value;
}

QString RespClient::parseValue(int& pos, bool& ok) const {
    if (pos >= buffer_.size()) {
        ok = false;
        return {};
    }

    char type = buffer_[pos];
    if (type == '+') return parseLineValue(pos, '+', "", ok);
    if (type == '-') return parseLineValue(pos, '-', "ERR ", ok);
    if (type == ':') return parseLineValue(pos, ':', "", ok);

    if (type == '$') {
        int len_end = buffer_.indexOf("\r\n", pos);
        if (len_end < 0) {
            ok = false;
            return {};
        }
        bool len_ok = false;
        int len = QString::fromUtf8(buffer_.mid(pos + 1, len_end - pos - 1)).toInt(&len_ok);
        if (!len_ok) {
            ok = false;
            return {};
        }
        pos = len_end + 2;
        if (len == -1) return "(nil)";
        if (buffer_.size() < pos + len + 2) {
            ok = false;
            return {};
        }
        QString value = QString::fromUtf8(buffer_.mid(pos, len));
        pos += len + 2;
        return value;
    }

    if (type == '*') {
        int count_end = buffer_.indexOf("\r\n", pos);
        if (count_end < 0) {
            ok = false;
            return {};
        }
        bool count_ok = false;
        int count = QString::fromUtf8(buffer_.mid(pos + 1, count_end - pos - 1)).toInt(&count_ok);
        if (!count_ok || count < 0) {
            ok = false;
            return {};
        }
        pos = count_end + 2;
        QStringList items;
        for (int i = 0; i < count; ++i) {
            QString item = parseValue(pos, ok);
            if (!ok) return {};
            items << QString("%1) %2").arg(i + 1).arg(item);
        }
        return items.join('\n');
    }

    ok = false;
    return {};
}
