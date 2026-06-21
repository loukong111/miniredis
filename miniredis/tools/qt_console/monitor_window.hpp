#pragma once

#include "resp_client.hpp"
#include "stats_client.hpp"

#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTimer;

class MonitorWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MonitorWindow(QWidget* parent = nullptr);

private:
    QWidget* buildConnectionPanel();
    QWidget* buildKvTab();
    QWidget* buildClusterTab();
    QWidget* buildStatsTab();
    QWidget* buildRawTab();

    void sendCommand(const QStringList& parts);
    void appendLog(const QString& text);
    void updateConnectionState(bool connected);
    void updateStatsView(const QJsonObject& stats);
    QString host() const;
    quint16 respPort() const;
    quint16 statsPort() const;

    RespClient resp_client_;
    StatsClient stats_client_;
    QTimer* stats_timer_;

    QLineEdit* host_edit_;
    QSpinBox* resp_port_spin_;
    QSpinBox* stats_port_spin_;
    QLineEdit* password_edit_;
    QLabel* status_label_;
    QTabWidget* tabs_;
    QPlainTextEdit* output_;
    QPlainTextEdit* cluster_output_;
    QPlainTextEdit* metrics_output_;
    QMap<QString, QLabel*> stat_labels_;
};
