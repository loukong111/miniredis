#pragma once

#include "resp_client.hpp"
#include "stats_client.hpp"

#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QString>
#include <QStringList>

class QCheckBox;
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
    ~MonitorWindow() override;

private:
    QWidget* buildConnectionPanel();
    QWidget* buildServerTab();
    QWidget* buildKvTab();
    QWidget* buildClusterTab();
    QWidget* buildStatsTab();
    QWidget* buildMetricsTab();
    QWidget* buildRawCommandTab();
    QWidget* buildBenchmarkTab();

    void startServer();
    void stopServer();
    void startClusterDemo();
    void stopClusterDemo();
    void failClusterDemoNode();
    void runBenchmark();
    void stopBenchmark();
    void sendCommand(const QStringList& parts);
    bool maybeFollowMoved(const QString& text);
    void appendLog(const QString& text);
    void appendServerLog(const QString& text);
    void appendBenchmarkLog(const QString& text);
    void updateConnectionState(bool connected);
    void updateServerState();
    void updateBenchmarkState();
    void updateStatsView(const QJsonObject& stats);
    QString host() const;
    quint16 respPort() const;
    quint16 statsPort() const;

    RespClient resp_client_;
    StatsClient stats_client_;
    QTimer* stats_timer_;
    QProcess* server_process_;
    QProcess* benchmark_process_;
    QMap<int, QProcess*> cluster_processes_;

    QLineEdit* host_edit_;
    QSpinBox* resp_port_spin_;
    QSpinBox* stats_port_spin_;
    QLineEdit* password_edit_;
    QCheckBox* follow_moved_check_;
    QLabel* status_label_;
    QTabWidget* tabs_;
    QPlainTextEdit* output_;
    QPlainTextEdit* cluster_output_;
    QPlainTextEdit* metrics_output_;
    QPlainTextEdit* raw_command_output_;
    QPlainTextEdit* server_log_;
    QPlainTextEdit* benchmark_output_;
    QMap<QString, QLabel*> stat_labels_;

    QLineEdit* server_path_edit_;
    QLineEdit* bind_edit_;
    QLineEdit* snapshot_file_edit_;
    QLineEdit* node_addr_edit_;
    QLineEdit* nodes_edit_;
    QSpinBox* server_resp_port_spin_;
    QSpinBox* server_stats_port_spin_;
    QSpinBox* snapshot_interval_spin_;
    QSpinBox* max_clients_spin_;
    QCheckBox* cluster_check_;
    QLabel* server_status_label_;

    QLineEdit* raw_command_edit_;
    QSpinBox* benchmark_requests_spin_;
    QSpinBox* benchmark_clients_spin_;
    QSpinBox* benchmark_payload_spin_;
    QLabel* benchmark_status_label_;
    QStringList last_command_;
    QStringList pending_moved_retry_;
    int moved_follow_hops_ = 0;
};
