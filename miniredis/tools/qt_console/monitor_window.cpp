#include "monitor_window.hpp"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

MonitorWindow::MonitorWindow(QWidget* parent)
    : QMainWindow(parent),
      stats_timer_(new QTimer(this)),
      server_process_(new QProcess(this)),
      benchmark_process_(new QProcess(this)) {
    setWindowTitle("MiniRedis Qt Console");
    resize(1180, 820);

    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->addWidget(buildConnectionPanel());

    tabs_ = new QTabWidget(central);
    tabs_->addTab(buildServerTab(), "Server");
    tabs_->addTab(buildKvTab(), "KV Commands");
    tabs_->addTab(buildClusterTab(), "Cluster");
    tabs_->addTab(buildStatsTab(), "Stats");
    tabs_->addTab(buildMetricsTab(), "Metrics");
    tabs_->addTab(buildRawCommandTab(), "Raw Command");
    tabs_->addTab(buildBenchmarkTab(), "Benchmark");
    root->addWidget(tabs_, 1);
    setCentralWidget(central);

    connect(&resp_client_, &RespClient::connected, this, [this]() {
        updateConnectionState(true);
        if (!password_edit_->text().isEmpty()) {
            sendCommand({"AUTH", password_edit_->text()});
        }
        if (!pending_moved_retry_.isEmpty()) {
            QStringList retry = pending_moved_retry_;
            pending_moved_retry_.clear();
            appendLog("> retry after MOVED: " + retry.join(' '));
            sendCommand(retry);
        }
    });
    connect(&resp_client_, &RespClient::disconnected, this, [this]() { updateConnectionState(false); });
    connect(&resp_client_, &RespClient::responseReceived, this, &MonitorWindow::appendLog);
    connect(&resp_client_, &RespClient::errorOccurred, this, &MonitorWindow::appendLog);

    connect(&stats_client_, &StatsClient::statsReceived, this, &MonitorWindow::updateStatsView);
    connect(&stats_client_, &StatsClient::metricsReceived, metrics_output_, &QPlainTextEdit::setPlainText);
    connect(&stats_client_, &StatsClient::errorOccurred, this, &MonitorWindow::appendLog);

    connect(server_process_, &QProcess::readyReadStandardOutput, this, [this]() {
        appendServerLog(QString::fromUtf8(server_process_->readAllStandardOutput()));
    });
    connect(server_process_, &QProcess::readyReadStandardError, this, [this]() {
        appendServerLog(QString::fromUtf8(server_process_->readAllStandardError()));
    });
    connect(server_process_, &QProcess::started, this, [this]() {
        updateServerState();
        appendServerLog("server process started");
        stats_timer_->start();
        QTimer::singleShot(500, this, [this]() { resp_client_.connectToServer(host(), respPort()); });
    });
    connect(server_process_, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus status) {
        updateServerState();
        appendServerLog(QString("server process exited: code=%1 status=%2")
                            .arg(exit_code)
                            .arg(status == QProcess::NormalExit ? "normal" : "crashed"));
    });
    connect(server_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        updateServerState();
        appendServerLog("server process error: " + server_process_->errorString());
    });

    connect(benchmark_process_, &QProcess::readyReadStandardOutput, this, [this]() {
        appendBenchmarkLog(QString::fromUtf8(benchmark_process_->readAllStandardOutput()));
    });
    connect(benchmark_process_, &QProcess::readyReadStandardError, this, [this]() {
        appendBenchmarkLog(QString::fromUtf8(benchmark_process_->readAllStandardError()));
    });
    connect(benchmark_process_, &QProcess::started, this, [this]() {
        updateBenchmarkState();
        appendBenchmarkLog("redis-benchmark started");
    });
    connect(benchmark_process_, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus status) {
        updateBenchmarkState();
        appendBenchmarkLog(QString("redis-benchmark exited: code=%1 status=%2")
                               .arg(exit_code)
                               .arg(status == QProcess::NormalExit ? "normal" : "crashed"));
    });
    connect(benchmark_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        updateBenchmarkState();
        appendBenchmarkLog("redis-benchmark error: " + benchmark_process_->errorString());
    });

    connect(stats_timer_, &QTimer::timeout, this, [this]() {
        stats_client_.fetchStats(host(), statsPort());
        stats_client_.fetchMetrics(host(), statsPort());
    });
    stats_timer_->setInterval(2000);

    updateConnectionState(false);
    updateServerState();
    updateBenchmarkState();
}

MonitorWindow::~MonitorWindow() {
    stopBenchmark();
    stopClusterDemo();
    stopServer();
}

QWidget* MonitorWindow::buildConnectionPanel() {
    QGroupBox* box = new QGroupBox("Connection", this);
    QGridLayout* layout = new QGridLayout(box);

    host_edit_ = new QLineEdit("127.0.0.1", box);
    resp_port_spin_ = new QSpinBox(box);
    resp_port_spin_->setRange(1, 65535);
    resp_port_spin_->setValue(6366);
    stats_port_spin_ = new QSpinBox(box);
    stats_port_spin_->setRange(1, 65535);
    stats_port_spin_->setValue(8080);
    password_edit_ = new QLineEdit(box);
    password_edit_->setEchoMode(QLineEdit::Password);
    follow_moved_check_ = new QCheckBox("Follow MOVED", box);
    follow_moved_check_->setChecked(true);
    status_label_ = new QLabel("Disconnected", box);

    QPushButton* connect_btn = new QPushButton("Connect", box);
    QPushButton* disconnect_btn = new QPushButton("Disconnect", box);
    QPushButton* ping_btn = new QPushButton("PING", box);

    layout->addWidget(new QLabel("Host", box), 0, 0);
    layout->addWidget(host_edit_, 0, 1);
    layout->addWidget(new QLabel("RESP Port", box), 0, 2);
    layout->addWidget(resp_port_spin_, 0, 3);
    layout->addWidget(new QLabel("Stats Port", box), 0, 4);
    layout->addWidget(stats_port_spin_, 0, 5);
    layout->addWidget(new QLabel("Password", box), 1, 0);
    layout->addWidget(password_edit_, 1, 1);
    layout->addWidget(follow_moved_check_, 1, 2);
    layout->addWidget(status_label_, 1, 3);
    layout->addWidget(connect_btn, 1, 4);
    layout->addWidget(disconnect_btn, 1, 5);
    layout->addWidget(ping_btn, 1, 6);

    connect(connect_btn, &QPushButton::clicked, this, [this]() {
        resp_client_.connectToServer(host(), respPort());
        stats_timer_->start();
    });
    connect(disconnect_btn, &QPushButton::clicked, this, [this]() {
        stats_timer_->stop();
        resp_client_.disconnectFromServer();
    });
    connect(ping_btn, &QPushButton::clicked, this, [this]() { sendCommand({"PING"}); });

    return box;
}

QWidget* MonitorWindow::buildServerTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    QGroupBox* config_box = new QGroupBox("Server Launch", page);
    QGridLayout* config = new QGridLayout(config_box);

    server_path_edit_ = new QLineEdit("build/miniredis", config_box);
    bind_edit_ = new QLineEdit("127.0.0.1", config_box);
    snapshot_file_edit_ = new QLineEdit("build/qt_snapshot.dat", config_box);
    node_addr_edit_ = new QLineEdit("127.0.0.1:6366", config_box);
    nodes_edit_ = new QLineEdit("127.0.0.1:6366", config_box);
    server_resp_port_spin_ = new QSpinBox(config_box);
    server_resp_port_spin_->setRange(1, 65535);
    server_resp_port_spin_->setValue(6366);
    server_stats_port_spin_ = new QSpinBox(config_box);
    server_stats_port_spin_->setRange(1, 65535);
    server_stats_port_spin_->setValue(8080);
    snapshot_interval_spin_ = new QSpinBox(config_box);
    snapshot_interval_spin_->setRange(1, 86400);
    snapshot_interval_spin_->setValue(30);
    max_clients_spin_ = new QSpinBox(config_box);
    max_clients_spin_->setRange(1, 1000000);
    max_clients_spin_->setValue(10000);
    cluster_check_ = new QCheckBox("Cluster mode", config_box);
    server_status_label_ = new QLabel("Stopped", config_box);
    server_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QPushButton* browse_server_btn = new QPushButton("Browse", config_box);
    QPushButton* browse_snapshot_btn = new QPushButton("Browse", config_box);
    QPushButton* start_btn = new QPushButton("Start Server", config_box);
    QPushButton* stop_btn = new QPushButton("Stop Server", config_box);
    QPushButton* start_cluster_btn = new QPushButton("Start 3-Node Cluster", config_box);
    QPushButton* stop_cluster_btn = new QPushButton("Stop Cluster", config_box);
    QPushButton* fail_cluster_btn = new QPushButton("Fail Node", config_box);
    QPushButton* connect_btn = new QPushButton("Connect", config_box);

    config->addWidget(new QLabel("Binary", config_box), 0, 0);
    config->addWidget(server_path_edit_, 0, 1, 1, 3);
    config->addWidget(browse_server_btn, 0, 4);
    config->addWidget(new QLabel("Bind", config_box), 1, 0);
    config->addWidget(bind_edit_, 1, 1);
    config->addWidget(new QLabel("RESP Port", config_box), 1, 2);
    config->addWidget(server_resp_port_spin_, 1, 3);
    config->addWidget(new QLabel("Stats Port", config_box), 1, 4);
    config->addWidget(server_stats_port_spin_, 1, 5);
    config->addWidget(new QLabel("Snapshot", config_box), 2, 0);
    config->addWidget(snapshot_file_edit_, 2, 1, 1, 3);
    config->addWidget(browse_snapshot_btn, 2, 4);
    config->addWidget(new QLabel("Interval", config_box), 2, 5);
    config->addWidget(snapshot_interval_spin_, 2, 6);
    config->addWidget(new QLabel("Max clients", config_box), 3, 0);
    config->addWidget(max_clients_spin_, 3, 1);
    config->addWidget(cluster_check_, 3, 2);
    config->addWidget(new QLabel("Node addr", config_box), 4, 0);
    config->addWidget(node_addr_edit_, 4, 1, 1, 2);
    config->addWidget(new QLabel("Nodes", config_box), 4, 3);
    config->addWidget(nodes_edit_, 4, 4, 1, 3);
    config->addWidget(new QLabel("Status", config_box), 5, 0);
    config->addWidget(server_status_label_, 5, 1);
    config->addWidget(start_btn, 5, 2);
    config->addWidget(stop_btn, 5, 3);
    config->addWidget(connect_btn, 5, 4);
    config->addWidget(start_cluster_btn, 6, 2);
    config->addWidget(stop_cluster_btn, 6, 3);
    config->addWidget(fail_cluster_btn, 6, 4);

    server_log_ = new QPlainTextEdit(page);
    server_log_->setReadOnly(true);

    root->addWidget(config_box);
    root->addWidget(server_log_, 1);

    connect(browse_server_btn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Select miniredis binary", QDir::currentPath());
        if (!path.isEmpty()) server_path_edit_->setText(path);
    });
    connect(browse_snapshot_btn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "Select snapshot file", QDir::currentPath());
        if (!path.isEmpty()) snapshot_file_edit_->setText(path);
    });
    connect(start_btn, &QPushButton::clicked, this, &MonitorWindow::startServer);
    connect(stop_btn, &QPushButton::clicked, this, &MonitorWindow::stopServer);
    connect(start_cluster_btn, &QPushButton::clicked, this, &MonitorWindow::startClusterDemo);
    connect(stop_cluster_btn, &QPushButton::clicked, this, &MonitorWindow::stopClusterDemo);
    connect(fail_cluster_btn, &QPushButton::clicked, this, &MonitorWindow::failClusterDemoNode);
    connect(connect_btn, &QPushButton::clicked, this, [this]() {
        resp_client_.connectToServer(host(), respPort());
        stats_timer_->start();
    });

    return page;
}

QWidget* MonitorWindow::buildKvTab() {
    QWidget* page = new QWidget(this);
    QGridLayout* layout = new QGridLayout(page);

    QLineEdit* key_edit = new QLineEdit(page);
    QLineEdit* value_edit = new QLineEdit(page);
    QLineEdit* keys_edit = new QLineEdit(page);
    QSpinBox* ttl_spin = new QSpinBox(page);
    ttl_spin->setRange(0, 86400);
    ttl_spin->setValue(30);
    QCheckBox* set_ex_check = new QCheckBox("EX", page);

    QPushButton* set_btn = new QPushButton("SET", page);
    QPushButton* get_btn = new QPushButton("GET", page);
    QPushButton* mget_btn = new QPushButton("MGET", page);
    QPushButton* del_btn = new QPushButton("DEL", page);
    QPushButton* exists_btn = new QPushButton("EXISTS", page);
    QPushButton* expire_btn = new QPushButton("EXPIRE", page);
    QPushButton* ttl_btn = new QPushButton("TTL", page);
    QPushButton* command_btn = new QPushButton("COMMAND", page);
    QPushButton* command_count_btn = new QPushButton("COMMAND COUNT", page);

    output_ = new QPlainTextEdit(page);
    output_->setReadOnly(true);

    layout->addWidget(new QLabel("Key", page), 0, 0);
    layout->addWidget(key_edit, 0, 1, 1, 3);
    layout->addWidget(new QLabel("Value", page), 1, 0);
    layout->addWidget(value_edit, 1, 1, 1, 3);
    layout->addWidget(new QLabel("TTL seconds", page), 2, 0);
    layout->addWidget(ttl_spin, 2, 1);
    layout->addWidget(set_ex_check, 2, 2);
    layout->addWidget(set_btn, 2, 3);
    layout->addWidget(get_btn, 3, 0);
    layout->addWidget(del_btn, 3, 1);
    layout->addWidget(exists_btn, 3, 2);
    layout->addWidget(ttl_btn, 3, 3);
    layout->addWidget(expire_btn, 4, 0);
    layout->addWidget(command_btn, 4, 1);
    layout->addWidget(command_count_btn, 4, 2);
    layout->addWidget(new QLabel("MGET keys", page), 5, 0);
    layout->addWidget(keys_edit, 5, 1, 1, 2);
    layout->addWidget(mget_btn, 5, 3);
    layout->addWidget(output_, 6, 0, 1, 4);
    layout->setRowStretch(6, 1);

    connect(set_btn, &QPushButton::clicked, this, [this, key_edit, value_edit, ttl_spin, set_ex_check]() {
        QStringList cmd{"SET", key_edit->text(), value_edit->text()};
        if (set_ex_check->isChecked()) cmd << "EX" << QString::number(ttl_spin->value());
        sendCommand(cmd);
    });
    connect(get_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"GET", key_edit->text()}); });
    connect(del_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"DEL", key_edit->text()}); });
    connect(exists_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"EXISTS", key_edit->text()}); });
    connect(expire_btn, &QPushButton::clicked, this, [this, key_edit, ttl_spin]() {
        sendCommand({"EXPIRE", key_edit->text(), QString::number(ttl_spin->value())});
    });
    connect(ttl_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"TTL", key_edit->text()}); });
    connect(command_btn, &QPushButton::clicked, this, [this]() { sendCommand({"COMMAND"}); });
    connect(command_count_btn, &QPushButton::clicked, this, [this]() { sendCommand({"COMMAND", "COUNT"}); });
    connect(mget_btn, &QPushButton::clicked, this, [this, keys_edit]() {
        QStringList cmd{"MGET"};
        for (const QString& key : keys_edit->text().split(' ', Qt::SkipEmptyParts)) cmd << key;
        sendCommand(cmd);
    });

    return page;
}

QWidget* MonitorWindow::buildClusterTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    QGridLayout* controls = new QGridLayout();

    QLineEdit* key_edit = new QLineEdit(page);
    QSpinBox* slot_spin = new QSpinBox(page);
    slot_spin->setRange(0, 16383);
    QPushButton* info_btn = new QPushButton("CLUSTER INFO", page);
    QPushButton* nodes_btn = new QPushButton("CLUSTER NODES", page);
    QPushButton* slots_btn = new QPushButton("CLUSTER SLOTS", page);
    QPushButton* myid_btn = new QPushButton("CLUSTER MYID", page);
    QPushButton* keyslot_btn = new QPushButton("KEYSLOT", page);
    QPushButton* count_slot_btn = new QPushButton("COUNTKEYSINSLOT", page);
    cluster_output_ = new QPlainTextEdit(page);
    cluster_output_->setReadOnly(true);

    controls->addWidget(info_btn, 0, 0);
    controls->addWidget(nodes_btn, 0, 1);
    controls->addWidget(slots_btn, 0, 2);
    controls->addWidget(myid_btn, 0, 3);
    controls->addWidget(new QLabel("Key", page), 1, 0);
    controls->addWidget(key_edit, 1, 1);
    controls->addWidget(keyslot_btn, 1, 2);
    controls->addWidget(new QLabel("Slot", page), 2, 0);
    controls->addWidget(slot_spin, 2, 1);
    controls->addWidget(count_slot_btn, 2, 2);
    layout->addLayout(controls);
    layout->addWidget(cluster_output_, 1);

    connect(info_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "INFO"}); });
    connect(nodes_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "NODES"}); });
    connect(slots_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "SLOTS"}); });
    connect(myid_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "MYID"}); });
    connect(keyslot_btn, &QPushButton::clicked, this, [this, key_edit]() {
        QString key = key_edit->text();
        if (key.isEmpty()) {
            appendLog("ERR key is required for CLUSTER KEYSLOT");
            return;
        }
        sendCommand({"CLUSTER", "KEYSLOT", key});
    });
    connect(count_slot_btn, &QPushButton::clicked, this, [this, slot_spin]() {
        sendCommand({"CLUSTER", "COUNTKEYSINSLOT", QString::number(slot_spin->value())});
    });

    return page;
}

QWidget* MonitorWindow::buildStatsTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    QGridLayout* grid = new QGridLayout();

    const QStringList keys = {
        "total_commands", "get_hits", "get_misses", "hit_rate",
        "key_count", "mem_pool_used_blocks", "mem_pool_free_blocks",
        "connected_clients", "total_connections", "rejected_connections",
        "avg_command_latency_us", "max_command_latency_us"
    };
    int row = 0;
    for (const QString& key : keys) {
        QLabel* name = new QLabel(key, page);
        QLabel* value = new QLabel("-", page);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        stat_labels_[key] = value;
        grid->addWidget(name, row, 0);
        grid->addWidget(value, row, 1);
        ++row;
    }
    QPushButton* refresh_btn = new QPushButton("Refresh", page);
    root->addLayout(grid);
    root->addWidget(refresh_btn);
    root->addStretch(1);

    connect(refresh_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchStats(host(), statsPort());
        stats_client_.fetchMetrics(host(), statsPort());
    });

    return page;
}

QWidget* MonitorWindow::buildMetricsTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    metrics_output_ = new QPlainTextEdit(page);
    metrics_output_->setReadOnly(true);
    layout->addWidget(metrics_output_);
    return page;
}

QWidget* MonitorWindow::buildRawCommandTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    QGridLayout* controls = new QGridLayout();

    raw_command_edit_ = new QLineEdit(page);
    raw_command_edit_->setPlaceholderText("SET user:1 tom");
    QPushButton* send_btn = new QPushButton("Send", page);
    QPushButton* clear_btn = new QPushButton("Clear", page);
    raw_command_output_ = new QPlainTextEdit(page);
    raw_command_output_->setReadOnly(true);

    controls->addWidget(new QLabel("Command", page), 0, 0);
    controls->addWidget(raw_command_edit_, 0, 1);
    controls->addWidget(send_btn, 0, 2);
    controls->addWidget(clear_btn, 0, 3);
    root->addLayout(controls);
    root->addWidget(raw_command_output_, 1);

    auto send_raw = [this]() {
        QString line = raw_command_edit_->text().trimmed();
        if (line.isEmpty()) return;
        QStringList parts = QProcess::splitCommand(line);
        if (parts.isEmpty()) {
            raw_command_output_->appendPlainText("ERR empty command");
            return;
        }
        sendCommand(parts);
    };

    connect(send_btn, &QPushButton::clicked, this, send_raw);
    connect(raw_command_edit_, &QLineEdit::returnPressed, this, send_raw);
    connect(clear_btn, &QPushButton::clicked, raw_command_output_, &QPlainTextEdit::clear);

    return page;
}

QWidget* MonitorWindow::buildBenchmarkTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    QGridLayout* controls = new QGridLayout();

    benchmark_requests_spin_ = new QSpinBox(page);
    benchmark_requests_spin_->setRange(1, 10000000);
    benchmark_requests_spin_->setValue(100000);
    benchmark_clients_spin_ = new QSpinBox(page);
    benchmark_clients_spin_->setRange(1, 10000);
    benchmark_clients_spin_->setValue(50);
    benchmark_payload_spin_ = new QSpinBox(page);
    benchmark_payload_spin_->setRange(1, 1048576);
    benchmark_payload_spin_->setValue(64);
    benchmark_status_label_ = new QLabel("Idle", page);
    benchmark_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QPushButton* run_btn = new QPushButton("Run SET/GET", page);
    QPushButton* stop_btn = new QPushButton("Stop", page);
    QPushButton* clear_btn = new QPushButton("Clear", page);
    benchmark_output_ = new QPlainTextEdit(page);
    benchmark_output_->setReadOnly(true);

    controls->addWidget(new QLabel("Requests", page), 0, 0);
    controls->addWidget(benchmark_requests_spin_, 0, 1);
    controls->addWidget(new QLabel("Clients", page), 0, 2);
    controls->addWidget(benchmark_clients_spin_, 0, 3);
    controls->addWidget(new QLabel("Payload bytes", page), 0, 4);
    controls->addWidget(benchmark_payload_spin_, 0, 5);
    controls->addWidget(new QLabel("Status", page), 1, 0);
    controls->addWidget(benchmark_status_label_, 1, 1);
    controls->addWidget(run_btn, 1, 2);
    controls->addWidget(stop_btn, 1, 3);
    controls->addWidget(clear_btn, 1, 4);
    root->addLayout(controls);
    root->addWidget(benchmark_output_, 1);

    connect(run_btn, &QPushButton::clicked, this, &MonitorWindow::runBenchmark);
    connect(stop_btn, &QPushButton::clicked, this, &MonitorWindow::stopBenchmark);
    connect(clear_btn, &QPushButton::clicked, benchmark_output_, &QPlainTextEdit::clear);

    return page;
}

void MonitorWindow::startServer() {
    if (server_process_->state() != QProcess::NotRunning) {
        appendServerLog("server is already running");
        return;
    }

    QString program = server_path_edit_->text().trimmed();
    if (program.isEmpty()) {
        appendServerLog("ERR server binary path is empty");
        return;
    }

    resp_port_spin_->setValue(server_resp_port_spin_->value());
    stats_port_spin_->setValue(server_stats_port_spin_->value());
    QString connect_host = bind_edit_->text().trimmed();
    if (connect_host.isEmpty() || connect_host == "0.0.0.0") connect_host = "127.0.0.1";
    host_edit_->setText(connect_host);

    QStringList args{
        "--bind", bind_edit_->text().trimmed().isEmpty() ? "127.0.0.1" : bind_edit_->text().trimmed(),
        "--port", QString::number(server_resp_port_spin_->value()),
        "--stats-bind", bind_edit_->text().trimmed().isEmpty() ? "127.0.0.1" : bind_edit_->text().trimmed(),
        "--stats-port", QString::number(server_stats_port_spin_->value()),
        "--snapshot-file", snapshot_file_edit_->text().trimmed().isEmpty() ? "build/qt_snapshot.dat" : snapshot_file_edit_->text().trimmed(),
        "--snapshot-interval", QString::number(snapshot_interval_spin_->value()),
        "--max-clients", QString::number(max_clients_spin_->value())
    };

    if (!password_edit_->text().isEmpty()) {
        args << "--requirepass" << password_edit_->text();
    }
    if (cluster_check_->isChecked()) {
        args << "--cluster";
        if (!node_addr_edit_->text().trimmed().isEmpty()) args << "--node-addr" << node_addr_edit_->text().trimmed();
        if (!nodes_edit_->text().trimmed().isEmpty()) args << "--nodes" << nodes_edit_->text().trimmed();
    }

    appendServerLog("> " + program + " " + args.join(' '));
    server_process_->setWorkingDirectory(QDir::currentPath());
    server_process_->start(program, args);
}

void MonitorWindow::stopServer() {
    if (server_process_->state() == QProcess::NotRunning) {
        appendServerLog("server is not running");
        return;
    }
    appendServerLog("stopping server process...");
    stats_timer_->stop();
    resp_client_.disconnectFromServer();
    server_process_->terminate();
    if (!server_process_->waitForFinished(3000)) {
        appendServerLog("server did not stop after SIGTERM, killing it");
        server_process_->kill();
    }
}

void MonitorWindow::startClusterDemo() {
    if (!cluster_processes_.isEmpty()) {
        appendServerLog("cluster demo is already running");
        return;
    }
    if (server_process_->state() != QProcess::NotRunning) {
        appendServerLog("stop the single server before starting cluster demo");
        return;
    }

    QString program = server_path_edit_->text().trimmed();
    if (program.isEmpty()) {
        appendServerLog("ERR server binary path is empty");
        return;
    }

    const QString bind = bind_edit_->text().trimmed().isEmpty() ? "127.0.0.1" : bind_edit_->text().trimmed();
    const QString connect_host = (bind == "0.0.0.0") ? "127.0.0.1" : bind;
    const int base_port = server_resp_port_spin_->value();
    const int base_stats_port = server_stats_port_spin_->value();
    const QVector<int> ports{base_port, base_port + 1, base_port + 2};

    QStringList node_addrs;
    for (int port : ports) {
        node_addrs << QString("%1:%2").arg(connect_host).arg(port);
    }
    const QString nodes = node_addrs.join(',');

    QDir().mkpath("build/qt-cluster-demo");
    appendServerLog("starting 3-node cluster demo: " + nodes);

    for (int i = 0; i < ports.size(); ++i) {
        const int port = ports[i];
        const int stats_port = base_stats_port + i;
        QString snapshot = QString("build/qt-cluster-demo/snapshot_%1.dat").arg(port);

        QStringList args{
            "--cluster",
            "--bind", bind,
            "--port", QString::number(port),
            "--node-addr", QString("%1:%2").arg(connect_host).arg(port),
            "--nodes", nodes,
            "--stats-bind", bind,
            "--stats-port", QString::number(stats_port),
            "--snapshot-file", snapshot,
            "--snapshot-interval", QString::number(snapshot_interval_spin_->value()),
            "--max-clients", QString::number(max_clients_spin_->value()),
            "--cluster-heartbeat", "1",
            "--cluster-fail-threshold", "2"
        };
        if (!password_edit_->text().isEmpty()) {
            args << "--requirepass" << password_edit_->text();
        }

        QProcess* process = new QProcess(this);
        cluster_processes_[port] = process;
        connect(process, &QProcess::readyReadStandardOutput, this, [this, process, port]() {
            appendServerLog(QString("[node %1] %2").arg(port).arg(QString::fromUtf8(process->readAllStandardOutput())));
        });
        connect(process, &QProcess::readyReadStandardError, this, [this, process, port]() {
            appendServerLog(QString("[node %1] %2").arg(port).arg(QString::fromUtf8(process->readAllStandardError())));
        });
        connect(process, &QProcess::started, this, [this, port]() {
            appendServerLog(QString("node %1 started").arg(port));
            updateServerState();
        });
        connect(process, &QProcess::finished, this, [this, process, port](int exit_code, QProcess::ExitStatus status) {
            appendServerLog(QString("node %1 exited: code=%2 status=%3")
                                .arg(port)
                                .arg(exit_code)
                                .arg(status == QProcess::NormalExit ? "normal" : "crashed"));
            cluster_processes_.remove(port);
            process->deleteLater();
            updateServerState();
        });
        connect(process, &QProcess::errorOccurred, this, [this, process, port](QProcess::ProcessError) {
            appendServerLog(QString("node %1 error: %2").arg(port).arg(process->errorString()));
            updateServerState();
        });

        appendServerLog("> " + program + " " + args.join(' '));
        process->setWorkingDirectory(QDir::currentPath());
        process->start(program, args);
    }

    host_edit_->setText(connect_host);
    resp_port_spin_->setValue(base_port);
    stats_port_spin_->setValue(base_stats_port);
    server_resp_port_spin_->setValue(base_port);
    server_stats_port_spin_->setValue(base_stats_port);
    cluster_check_->setChecked(true);
    node_addr_edit_->setText(node_addrs.first());
    nodes_edit_->setText(nodes);
    stats_timer_->start();

    QTimer::singleShot(700, this, [this, connect_host, base_port]() {
        resp_client_.connectToServer(connect_host, static_cast<quint16>(base_port));
    });
}

void MonitorWindow::stopClusterDemo() {
    if (cluster_processes_.isEmpty()) {
        appendServerLog("cluster demo is not running");
        return;
    }
    appendServerLog("stopping cluster demo...");
    auto processes = cluster_processes_;
    for (auto it = processes.begin(); it != processes.end(); ++it) {
        QProcess* process = it.value();
        if (!process || process->state() == QProcess::NotRunning) continue;
        process->terminate();
        if (!process->waitForFinished(3000)) {
            process->kill();
        }
    }
    cluster_processes_.clear();
    stats_timer_->stop();
    resp_client_.disconnectFromServer();
    updateServerState();
}

void MonitorWindow::failClusterDemoNode() {
    if (cluster_processes_.size() < 2) {
        appendServerLog("cluster demo needs at least two running nodes");
        return;
    }

    int victim_port = -1;
    for (auto it = cluster_processes_.begin(); it != cluster_processes_.end(); ++it) {
        if (it.key() != respPort()) {
            victim_port = it.key();
            break;
        }
    }
    if (victim_port < 0) {
        appendServerLog("no non-current node found to fail");
        return;
    }

    QProcess* victim = cluster_processes_.value(victim_port, nullptr);
    if (!victim || victim->state() == QProcess::NotRunning) {
        appendServerLog(QString("node %1 is already stopped").arg(victim_port));
        return;
    }

    appendServerLog(QString("injecting failure: stopping node %1").arg(victim_port));
    victim->terminate();
    if (!victim->waitForFinished(3000)) {
        victim->kill();
    }

    QTimer::singleShot(3500, this, [this]() {
        appendLog("> auto check after node failure");
        sendCommand({"CLUSTER", "INFO"});
        sendCommand({"CLUSTER", "NODES"});
    });
}

void MonitorWindow::runBenchmark() {
    if (benchmark_process_->state() != QProcess::NotRunning) {
        appendBenchmarkLog("benchmark is already running");
        return;
    }

    QStringList args{
        "-h", host(),
        "-p", QString::number(respPort()),
        "-n", QString::number(benchmark_requests_spin_->value()),
        "-c", QString::number(benchmark_clients_spin_->value()),
        "-d", QString::number(benchmark_payload_spin_->value()),
        "-t", "set,get"
    };
    if (!password_edit_->text().isEmpty()) {
        args << "-a" << password_edit_->text();
    }

    appendBenchmarkLog("> redis-benchmark " + args.join(' '));
    benchmark_process_->start("redis-benchmark", args);
}

void MonitorWindow::stopBenchmark() {
    if (benchmark_process_->state() == QProcess::NotRunning) {
        appendBenchmarkLog("benchmark is not running");
        return;
    }
    appendBenchmarkLog("stopping redis-benchmark...");
    benchmark_process_->terminate();
    if (!benchmark_process_->waitForFinished(3000)) {
        appendBenchmarkLog("redis-benchmark did not stop, killing it");
        benchmark_process_->kill();
    }
}

void MonitorWindow::sendCommand(const QStringList& parts) {
    if (parts.isEmpty()) return;
    QString rendered = "> " + parts.join(' ');
    appendLog(rendered);
    if (parts.first().compare("AUTH", Qt::CaseInsensitive) != 0) {
        last_command_ = parts;
    }
    resp_client_.sendCommand(parts);
}

bool MonitorWindow::maybeFollowMoved(const QString& text) {
    if (!follow_moved_check_ || !follow_moved_check_->isChecked()) return false;
    if (last_command_.isEmpty()) return false;

    QString line = text.trimmed();
    if (line.startsWith("ERR MOVED ")) {
        line = line.mid(QString("ERR ").size());
    }
    if (!line.startsWith("MOVED ")) return false;
    if (moved_follow_hops_ >= 3) {
        appendLog("ERR MOVED auto-follow reached hop limit");
        moved_follow_hops_ = 0;
        return false;
    }

    QStringList parts = line.split(' ', Qt::SkipEmptyParts);
    if (parts.size() < 3) return false;

    QString node = parts.at(2);
    int sep = node.lastIndexOf(':');
    if (sep <= 0 || sep + 1 >= node.size()) return false;

    bool ok = false;
    int port = node.mid(sep + 1).toInt(&ok);
    if (!ok || port <= 0 || port > 65535) return false;

    QString target_host = node.left(sep);
    pending_moved_retry_ = last_command_;
    ++moved_follow_hops_;

    appendLog(QString("following MOVED to %1:%2").arg(target_host).arg(port));
    host_edit_->setText(target_host);
    resp_port_spin_->setValue(port);
    resp_client_.connectToServer(target_host, static_cast<quint16>(port));
    return true;
}

void MonitorWindow::appendLog(const QString& text) {
    output_->appendPlainText(text);
    if (tabs_->currentIndex() == 2) cluster_output_->appendPlainText(text);
    if (tabs_->currentIndex() == 5) raw_command_output_->appendPlainText(text);
    if (!maybeFollowMoved(text) && !text.startsWith(">") && !text.startsWith("following MOVED")) {
        moved_follow_hops_ = 0;
    }
}

void MonitorWindow::appendServerLog(const QString& text) {
    if (!server_log_) return;
    QString trimmed = text;
    if (trimmed.endsWith('\n')) trimmed.chop(1);
    if (!trimmed.isEmpty()) server_log_->appendPlainText(trimmed);
}

void MonitorWindow::appendBenchmarkLog(const QString& text) {
    if (!benchmark_output_) return;
    QString trimmed = text;
    if (trimmed.endsWith('\n')) trimmed.chop(1);
    if (!trimmed.isEmpty()) benchmark_output_->appendPlainText(trimmed);
}

void MonitorWindow::updateConnectionState(bool connected) {
    status_label_->setText(connected ? "Connected" : "Disconnected");
}

void MonitorWindow::updateServerState() {
    if (!server_status_label_) return;
    if (!cluster_processes_.isEmpty()) {
        server_status_label_->setText(QString("Cluster running (%1 nodes)").arg(cluster_processes_.size()));
        return;
    }
    server_status_label_->setText(server_process_->state() == QProcess::NotRunning ? "Stopped" : "Running");
}

void MonitorWindow::updateBenchmarkState() {
    if (!benchmark_status_label_) return;
    benchmark_status_label_->setText(benchmark_process_->state() == QProcess::NotRunning ? "Idle" : "Running");
}

void MonitorWindow::updateStatsView(const QJsonObject& stats) {
    for (auto it = stat_labels_.begin(); it != stat_labels_.end(); ++it) {
        QJsonValue value = stats.value(it.key());
        if (value.isDouble()) it.value()->setText(QString::number(value.toDouble()));
        else if (value.isString()) it.value()->setText(value.toString());
        else it.value()->setText("-");
    }
}

QString MonitorWindow::host() const {
    return host_edit_->text().trimmed();
}

quint16 MonitorWindow::respPort() const {
    return static_cast<quint16>(resp_port_spin_->value());
}

quint16 MonitorWindow::statsPort() const {
    return static_cast<quint16>(stats_port_spin_->value());
}
