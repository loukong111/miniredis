#pragma once

#include "resp_client.hpp"
#include "stats_client.hpp"

#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QTreeWidget;

class MonitorWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MonitorWindow(QWidget* parent = nullptr);
    ~MonitorWindow() override;
    bool exportScreenshots(const QString& directory);

private:
    void initConnectionControls(QWidget* parent);
    void buildMenus();
    void openToolDialog(int index, const QString& title);
    void switchPage(int index);
    void showConnectionDialog();
    QWidget* buildConsoleTab();
    QWidget* buildDemoLabTab();
    QWidget* buildServerTab();
    QWidget* buildClusterTab();
    QWidget* buildStatsTab();
    QWidget* buildDiagnosticsTab();
    QWidget* buildMetricsTab();
    QWidget* buildBenchmarkTab();

    void startServer();
    void stopServer();
    void restartServer();
    void startClusterDemo();
    void startReplicaDemo();
    void stopClusterDemo();
    void failClusterDemoNode();
    void runPersistenceDemo();
    void runBenchmark();
    void runBenchmarkMatrix();
    void stopBenchmark();
    void runScriptTool(const QString& script, const QStringList& args = {});
    void stopScriptTool();
    void sendCommand(const QStringList& parts);
    void executeConsoleScript();
    void insertConsoleTemplate(const QString& text);
    void runReplicationPsyncDemo();
    void runAofRecoveryDemo();
    void runClusterFailoverDemo();
    bool maybeFollowMoved(const QString& text);
    void appendLog(const QString& text);
    void appendDemoLog(const QString& text);
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
    QProcess* script_process_;
    QMap<int, QProcess*> cluster_processes_;

    QLineEdit* host_edit_;
    QSpinBox* resp_port_spin_;
    QSpinBox* stats_port_spin_;
    QLineEdit* auth_user_edit_;
    QLineEdit* password_edit_;
    QCheckBox* follow_moved_check_;
    QLabel* status_label_;
    QStackedWidget* pages_;
    QTreeWidget* resource_tree_;
    QComboBox* console_history_combo_;
    QComboBox* console_template_combo_;
    QPlainTextEdit* console_editor_;
    QPlainTextEdit* console_output_;
    QPlainTextEdit* command_help_;
    QPlainTextEdit* demo_output_;
    QPlainTextEdit* cluster_output_;
    QPlainTextEdit* diagnostics_output_;
    QPlainTextEdit* metrics_output_;
    QPlainTextEdit* server_log_;
    QPlainTextEdit* benchmark_output_;
    QMap<QString, QLabel*> stat_labels_;
    QMap<QString, QLabel*> console_stat_labels_;
    QVector<QAction*> page_actions_;
    QWidget* resource_panel_;
    QWidget* inspector_panel_;

    QLineEdit* server_path_edit_;
    QLineEdit* bind_edit_;
    QLineEdit* snapshot_file_edit_;
    QLineEdit* appendonly_file_edit_;
    QLineEdit* cluster_config_file_edit_;
    QLineEdit* replicaof_edit_;
    QLineEdit* replicas_edit_;
    QLineEdit* acl_users_edit_;
    QLineEdit* node_addr_edit_;
    QLineEdit* nodes_edit_;
    QSpinBox* server_resp_port_spin_;
    QSpinBox* server_stats_port_spin_;
    QSpinBox* snapshot_interval_spin_;
    QSpinBox* max_clients_spin_;
    QSpinBox* io_threads_spin_;
    QSpinBox* cache_shards_spin_;
    QSpinBox* maxmemory_spin_;
    QSpinBox* slowlog_threshold_spin_;
    QSpinBox* slowlog_max_len_spin_;
    QComboBox* eviction_policy_combo_;
    QComboBox* appendfsync_combo_;
    QCheckBox* aof_check_;
    QCheckBox* cluster_check_;
    QLabel* server_status_label_;

    QSpinBox* benchmark_requests_spin_;
    QSpinBox* benchmark_clients_spin_;
    QSpinBox* benchmark_payload_spin_;
    QLabel* benchmark_status_label_;
    QStringList last_command_;
    QStringList pending_moved_retry_;
    QString last_log_text_;
    int moved_follow_hops_ = 0;
};
