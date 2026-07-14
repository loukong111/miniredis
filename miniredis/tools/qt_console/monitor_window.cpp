#include "monitor_window.hpp"

#include <QByteArrayView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSplitter>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVector>

#include <cmath>
#include <utility>

namespace {

uint16_t qtCrc16(QByteArrayView data) {
    uint16_t crc = 0;
    for (unsigned char byte : data) {
        crc ^= static_cast<uint16_t>(byte) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            } else {
                crc = static_cast<uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

QByteArray qtClusterHashKey(const QString& key) {
    QByteArray bytes = key.toUtf8();
    int open = bytes.indexOf('{');
    if (open < 0) return bytes;
    int close = bytes.indexOf('}', open + 1);
    if (close < 0 || close == open + 1) return bytes;
    return bytes.mid(open + 1, close - open - 1);
}

int qtClusterHashSlot(const QString& key) {
    return qtCrc16(QByteArrayView(qtClusterHashKey(key))) % 16384;
}

QString compactNumber(double value) {
    double rounded = std::round(value);
    if (std::fabs(value - rounded) < 0.0001) {
        return QString::number(static_cast<qlonglong>(rounded));
    }
    return QString::number(value, 'f', 4);
}

QString formatBytes(double value) {
    const QStringList units = {"B", "KiB", "MiB", "GiB"};
    double scaled = value;
    int unit = 0;
    while (scaled >= 1024.0 && unit + 1 < units.size()) {
        scaled /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return QString("%1 B").arg(static_cast<qlonglong>(value));
    }
    return QString("%1 %2").arg(scaled, 0, 'f', 2).arg(units[unit]);
}

QString formatStatValue(const QString& key, const QJsonValue& value) {
    if (value.isBool()) return value.toBool() ? "是" : "否";
    if (value.isString()) return value.toString().isEmpty() ? "-" : value.toString();
    if (!value.isDouble()) return "-";

    double number = value.toDouble();
    if (key == "hit_rate") {
        return QString("%1%").arg(number * 100.0, 0, 'f', 2);
    }
    if (key.endsWith("_bytes") || key == "maxmemory_bytes" ||
        key == "client_output_buffer_limit") {
        if (key == "maxmemory_bytes" && number == 0.0) return "不限制";
        return formatBytes(number);
    }
    if (key.endsWith("_unix_ms")) {
        if (number == 0.0) return "-";
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(number));
        return dt.toString("yyyy-MM-dd HH:mm:ss");
    }
    if (key.endsWith("_us")) {
        return QString("%1 us").arg(compactNumber(number));
    }
    if (key.endsWith("_ms")) {
        return QString("%1 ms").arg(compactNumber(number));
    }
    return compactNumber(number);
}

QStringList maskedProcessArgs(QStringList args) {
    const QStringList sensitive_flags = {"--requirepass", "-a"};
    for (int i = 0; i < args.size(); ++i) {
        if (sensitive_flags.contains(args[i]) && i + 1 < args.size()) {
            args[i + 1] = "******";
            ++i;
        }
    }
    return args;
}

QProcessEnvironment processEnvironmentWithPassword(const QString& password) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!password.isEmpty()) {
        env.insert("MINIREDIS_REQUIREPASS", password);
    }
    return env;
}

} // namespace

MonitorWindow::MonitorWindow(QWidget* parent)
    : QMainWindow(parent),
      stats_timer_(new QTimer(this)),
      server_process_(new QProcess(this)),
      benchmark_process_(new QProcess(this)) {
    setWindowTitle("MiniRedis 运维控制台");
    resize(1320, 860);
    setMinimumSize(1120, 720);

    setStyleSheet(R"(
        QMainWindow, QWidget {
            background: #f5f7fa;
            color: #172033;
            font-family: "Inter", "Segoe UI", "Noto Sans", sans-serif;
            font-size: 13px;
        }
        QGroupBox {
            background: #ffffff;
            border: 1px solid #d8dee8;
            border-radius: 8px;
            margin-top: 18px;
            padding: 18px 14px 14px 14px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: #24324b;
        }
        QLabel[role="muted"] { color: #68758a; }
        QLabel[role="section"] {
            color: #111827;
            font-size: 18px;
            font-weight: 700;
        }
        QLabel[role="badge"] {
            border-radius: 6px;
            padding: 5px 10px;
            background: #eef2f7;
            color: #334155;
            font-weight: 600;
        }
        QLineEdit, QSpinBox, QComboBox {
            background: #ffffff;
            border: 1px solid #cbd5e1;
            border-radius: 6px;
            padding: 6px 8px;
            min-height: 24px;
        }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
            border: 1px solid #2563eb;
        }
        QPushButton {
            background: #ffffff;
            border: 1px solid #cbd5e1;
            border-radius: 6px;
            padding: 7px 12px;
            color: #172033;
            font-weight: 600;
        }
        QPushButton:hover { background: #f1f5f9; border-color: #94a3b8; }
        QPushButton:pressed { background: #e2e8f0; }
        QPushButton[nav="true"] {
            background: transparent;
            border: 0;
            border-radius: 6px;
            padding: 8px 12px;
            color: #475569;
            font-weight: 700;
        }
        QPushButton[nav="true"]:hover {
            background: #e8edf5;
            color: #172033;
        }
        QPushButton[navSelected="true"] {
            background: #dbeafe;
            color: #1d4ed8;
        }
        QPushButton[class="primary"] {
            background: #2563eb;
            border-color: #1d4ed8;
            color: #ffffff;
        }
        QPushButton[class="primary"]:hover { background: #1d4ed8; }
        QPushButton[class="danger"] {
            background: #fff1f2;
            border-color: #fecdd3;
            color: #be123c;
        }
        QPushButton[class="danger"]:hover { background: #ffe4e6; }
        QCheckBox { spacing: 8px; }
        QFrame[role="workspace"] {
            border: 1px solid #d8dee8;
            border-radius: 8px;
            background: #ffffff;
        }
        QFrame[role="nav"] {
            background: #ffffff;
            border: 1px solid #d8dee8;
            border-radius: 8px;
        }
        QPlainTextEdit {
            background: #0f172a;
            color: #dbeafe;
            border: 1px solid #1e293b;
            border-radius: 8px;
            padding: 10px;
            font-family: "JetBrains Mono", "Consolas", monospace;
            font-size: 12px;
        }
        QPlainTextEdit[role="editor"] {
            background: #ffffff;
            color: #111827;
            border: 1px solid #d8dee8;
            border-radius: 8px;
            padding: 12px;
            font-family: "JetBrains Mono", "Consolas", monospace;
            font-size: 13px;
        }
        QPlainTextEdit[role="help"] {
            background: #ffffff;
            color: #334155;
            border: 1px solid #d8dee8;
            border-radius: 8px;
            padding: 10px;
            font-family: "JetBrains Mono", "Consolas", monospace;
            font-size: 12px;
        }
        QTreeWidget {
            background: #ffffff;
            border: 1px solid #d8dee8;
            border-radius: 8px;
            padding: 6px;
        }
        QTreeWidget::item {
            min-height: 24px;
            padding: 3px 4px;
        }
        QTreeWidget::item:selected {
            background: #dbeafe;
            color: #1e3a8a;
        }
        QHeaderView::section {
            background: #eef2f7;
            border: 0;
            border-bottom: 1px solid #d8dee8;
            padding: 6px;
            font-weight: 600;
        }
    )");

    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(12);
    initConnectionControls(central);

    QFrame* nav_frame = new QFrame(central);
    nav_frame->setProperty("role", "nav");
    QHBoxLayout* nav = new QHBoxLayout(nav_frame);
    nav->setContentsMargins(8, 6, 8, 6);
    nav->setSpacing(4);

    pages_ = new QStackedWidget(central);
    pages_->addWidget(buildConsoleTab());
    pages_->addWidget(buildDemoLabTab());
    pages_->addWidget(buildServerTab());
    pages_->addWidget(buildClusterTab());
    pages_->addWidget(buildStatsTab());
    pages_->addWidget(buildDiagnosticsTab());
    pages_->addWidget(buildMetricsTab());
    pages_->addWidget(buildBenchmarkTab());

    auto addNavButton = [this, nav](const QString& text, int index) {
        QPushButton* button = new QPushButton(text, this);
        button->setProperty("nav", true);
        button->setProperty("pageIndex", index);
        button->setProperty("navSelected", index == 0);
        nav->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, index]() {
            pages_->setCurrentIndex(index);
        });
    };

    addNavButton("控制台", 0);
    addNavButton("演示中心", 1);
    addNavButton("服务管理", 2);
    addNavButton("集群路由", 3);
    addNavButton("运行指标", 4);
    addNavButton("诊断工具", 5);
    addNavButton("监控指标", 6);
    addNavButton("压测", 7);
    nav->addStretch(1);
    connect(pages_, &QStackedWidget::currentChanged, this, [nav_frame](int index) {
        const auto buttons = nav_frame->findChildren<QPushButton*>();
        for (QPushButton* nav_button : buttons) {
            bool selected = nav_button->property("pageIndex").toInt() == index;
            nav_button->setProperty("navSelected", selected);
            nav_button->style()->unpolish(nav_button);
            nav_button->style()->polish(nav_button);
        }
    });

    QFrame* workspace = new QFrame(central);
    workspace->setProperty("role", "workspace");
    QVBoxLayout* workspace_layout = new QVBoxLayout(workspace);
    workspace_layout->setContentsMargins(0, 0, 0, 0);
    workspace_layout->addWidget(pages_);

    root->addWidget(nav_frame);
    root->addWidget(workspace, 1);
    setCentralWidget(central);

    connect(&resp_client_, &RespClient::connected, this, [this]() {
        updateConnectionState(true);
        if (!password_edit_->text().isEmpty()) {
            sendCommand({"AUTH", password_edit_->text()});
        }
        if (!pending_moved_retry_.isEmpty()) {
            QStringList retry = pending_moved_retry_;
            pending_moved_retry_.clear();
            appendLog("> MOVED 后重试：" + retry.join(' '));
            sendCommand(retry);
        }
    });
    connect(&resp_client_, &RespClient::disconnected, this, [this]() { updateConnectionState(false); });
    connect(&resp_client_, &RespClient::responseReceived, this, &MonitorWindow::appendLog);
    connect(&resp_client_, &RespClient::errorOccurred, this, [this](const QString& error) {
        if (!resp_client_.isConnected()) {
            stats_timer_->stop();
        }
        appendLog(error);
    });

    connect(&stats_client_, &StatsClient::statsReceived, this, &MonitorWindow::updateStatsView);
    connect(&stats_client_, &StatsClient::statsTextReceived, this, [this](const QString& body) {
        diagnostics_output_->appendPlainText("> GET /stats");
        diagnostics_output_->appendPlainText(body);
    });
    connect(&stats_client_, &StatsClient::metricsReceived, metrics_output_, &QPlainTextEdit::setPlainText);
    connect(&stats_client_, &StatsClient::healthReceived, this, [this](const QString& body) {
        diagnostics_output_->appendPlainText("> GET /healthz");
        diagnostics_output_->appendPlainText(body);
    });
    connect(&stats_client_, &StatsClient::readinessReceived, this, [this](const QString& body) {
        diagnostics_output_->appendPlainText("> GET /readyz");
        diagnostics_output_->appendPlainText(body);
    });
    connect(&stats_client_, &StatsClient::errorOccurred, this, [this](const QString& error) {
        if (!resp_client_.isConnected()) {
            stats_timer_->stop();
        }
        appendLog(error);
    });

    connect(server_process_, &QProcess::readyReadStandardOutput, this, [this]() {
        appendServerLog(QString::fromUtf8(server_process_->readAllStandardOutput()));
    });
    connect(server_process_, &QProcess::readyReadStandardError, this, [this]() {
        appendServerLog(QString::fromUtf8(server_process_->readAllStandardError()));
    });
    connect(server_process_, &QProcess::started, this, [this]() {
        updateServerState();
        appendServerLog("服务进程已启动");
        stats_timer_->start();
        QTimer::singleShot(500, this, [this]() { resp_client_.connectToServer(host(), respPort()); });
    });
    connect(server_process_, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus status) {
        updateServerState();
        appendServerLog(QString("服务进程已退出：code=%1 status=%2")
                            .arg(exit_code)
                            .arg(status == QProcess::NormalExit ? "正常退出" : "异常退出"));
    });
    connect(server_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        updateServerState();
        appendServerLog("服务进程错误：" + server_process_->errorString());
    });

    connect(benchmark_process_, &QProcess::readyReadStandardOutput, this, [this]() {
        appendBenchmarkLog(QString::fromUtf8(benchmark_process_->readAllStandardOutput()));
    });
    connect(benchmark_process_, &QProcess::readyReadStandardError, this, [this]() {
        appendBenchmarkLog(QString::fromUtf8(benchmark_process_->readAllStandardError()));
    });
    connect(benchmark_process_, &QProcess::started, this, [this]() {
        updateBenchmarkState();
        appendBenchmarkLog("redis-benchmark 已启动");
    });
    connect(benchmark_process_, &QProcess::finished, this, [this](int exit_code, QProcess::ExitStatus status) {
        updateBenchmarkState();
        appendBenchmarkLog(QString("redis-benchmark 已退出：code=%1 status=%2")
                               .arg(exit_code)
                               .arg(status == QProcess::NormalExit ? "正常退出" : "异常退出"));
    });
    connect(benchmark_process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        updateBenchmarkState();
        appendBenchmarkLog("redis-benchmark 错误：" + benchmark_process_->errorString());
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

bool MonitorWindow::exportScreenshots(const QString& directory) {
    if (!pages_) return false;
    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(".")) {
        return false;
    }

    resize(1440, 900);
    show();
    raise();
    QCoreApplication::processEvents();

    host_edit_->setText("127.0.0.1");
    resp_port_spin_->setValue(6366);
    stats_port_spin_->setValue(8080);
    updateConnectionState(true);

    QJsonObject demo_stats;
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    demo_stats["node_addr"] = "127.0.0.1:6366";
    demo_stats["ready"] = true;
    demo_stats["io_threads"] = 4;
    demo_stats["cache_shards"] = 16;
    demo_stats["total_commands"] = 12843;
    demo_stats["get_hits"] = 4762;
    demo_stats["get_misses"] = 318;
    demo_stats["hit_rate"] = 0.9374;
    demo_stats["latency_samples"] = 12843;
    demo_stats["avg_command_latency_us"] = 418;
    demo_stats["max_command_latency_us"] = 3847;
    demo_stats["key_count"] = 128;
    demo_stats["used_memory_bytes"] = 184320;
    demo_stats["maxmemory_bytes"] = 67108864;
    demo_stats["evicted_keys"] = 12;
    demo_stats["mem_pool_used_blocks"] = 93;
    demo_stats["mem_pool_free_blocks"] = 931;
    demo_stats["connected_clients"] = 6;
    demo_stats["total_connections"] = 41;
    demo_stats["rejected_connections"] = 0;
    demo_stats["slowlog_len"] = 3;
    demo_stats["slowlog_log_slower_than_us"] = 10000;
    demo_stats["slowlog_max_len"] = 128;
    demo_stats["max_clients"] = 10000;
    demo_stats["max_request_bytes"] = 1048576;
    demo_stats["max_key_bytes"] = 4096;
    demo_stats["max_value_bytes"] = 1048576;
    demo_stats["max_pipeline_commands"] = 1024;
    demo_stats["client_output_buffer_limit"] = 4194304;
    demo_stats["snapshot_running"] = false;
    demo_stats["snapshot_last_success_unix_ms"] = static_cast<double>(now_ms - 90000);
    demo_stats["snapshot_last_failure_unix_ms"] = 0;
    demo_stats["snapshot_last_duration_ms"] = 18;
    demo_stats["snapshot_last_key_count"] = 128;
    demo_stats["snapshot_failures"] = 0;
    demo_stats["aof_rewrite_running"] = false;
    demo_stats["aof_rewrite_buffer_bytes"] = 0;
    demo_stats["aof_last_rewrite_unix_ms"] = static_cast<double>(now_ms - 45000);
    demo_stats["aof_last_rewrite_failure_unix_ms"] = 0;
    demo_stats["aof_last_rewrite_duration_ms"] = 31;
    demo_stats["aof_last_rewrite_records"] = 128;
    demo_stats["aof_rewrite_failures"] = 0;
    demo_stats["aof_rewrite_last_status"] = "ok";
    demo_stats["aof_rewrite_last_error"] = "";
    updateStatsView(demo_stats);

    if (console_output_) {
        console_output_->setPlainText(
            "> PING\n"
            "PONG\n"
            "> SET user:1 tom\n"
            "OK\n"
            "> APPEND user:1 !\n"
            "(integer) 4\n"
            "> GET user:1\n"
            "tom!\n"
            "> TYPE user:1\n"
            "string\n"
            "> INCR counter\n"
            "(integer) 1\n"
            "> PEXPIRE user:1 60000\n"
            "(integer) 1\n"
            "> PTTL user:1\n"
            "(integer) 59872\n"
            "> INFO stats\n"
            "total_commands:12843\n"
            "connected_clients:6\n"
            "hit_rate:0.9374\n"
            "avg_command_latency_us:418\n");
    }
    if (demo_output_) {
        demo_output_->setPlainText(
            "=== 复制 / PSYNC 演示 ===\n"
            "步骤 1：向主节点写入数据，并查看复制 offset\n"
            "> INFO replication\n"
            "role:master\n"
            "master_repl_offset:42\n"
            "repl_backlog_active:1\n"
            "步骤 2：停止副本 6367，主节点继续接收写入\n"
            "步骤 3：重启副本，优先使用保存的 offset 进行 REPLPSYNC\n"
            "步骤 4：连接副本，验证离线期间的写入已同步\n"
            "\n"
            "=== AOF 恢复演示 ===\n"
            "步骤 1：写入数据，执行重写，并查看持久化状态\n"
            "aof_rewrite_last_status:ok\n"
            "步骤 2：重启服务，验证 AOF/snapshot 恢复\n"
            "步骤 3：查询恢复后的 key 和重写状态\n"
            "\n"
            "=== 集群故障观察演示 ===\n"
            "步骤 1：查看初始集群拓扑\n"
            "步骤 2：停止一个非当前节点，触发 pfail/fail\n"
            "步骤 3：查看心跳故障检测后的集群状态\n");
    }
    if (server_log_) {
        server_status_label_->setText("运行中");
        server_status_label_->setStyleSheet("background:#dcfce7;color:#166534;border-radius:6px;padding:5px 10px;font-weight:700;");
        server_log_->setPlainText(
            "> build/miniredis --bind 127.0.0.1 --port 6366 --stats-port 8080 --snapshot-file build/qt_snapshot.dat --appendonly-file build/qt_appendonly.aof --appendfsync everysec\n"
            "2026-07-08 19:57:10.112 [INFO] [server] listening on 127.0.0.1:6366\n"
            "2026-07-08 19:57:10.113 [INFO] [stats] HTTP server listening on 127.0.0.1:8080\n"
            "2026-07-08 19:57:10.114 [INFO] [snapshot] loaded snapshot with 128 keys\n"
            "2026-07-08 19:57:10.115 [INFO] [aof] replayed appendonly file, records=384, ignored_bad_tail=0\n"
            "2026-07-08 19:57:13.641 [INFO] [aof] BGREWRITEAOF finished, records=128, duration_ms=31\n");
    }
    if (cluster_output_) {
        cluster_output_->setPlainText(
            "> CLUSTER INFO\n"
            "cluster_enabled:1\n"
            "cluster_state:ok\n"
            "cluster_known_nodes:3\n"
            "cluster_current_node:127.0.0.1:6366\n"
            "\n"
            "> CLUSTER NODES\n"
            "002b04bd93d4c945002b04bd93d4c945002b04bd 127.0.0.1:6366 myself,master - 0 0 0 connected 0-5460\n"
            "9e5a0982c947cc799e5a0982c947cc799e5a0982 127.0.0.1:6367 master - 0 0 0 connected 5461-10922\n"
            "74f5b91c8a8dbf5474f5b91c8a8dbf5474f5b91c 127.0.0.1:6368 master - 0 0 0 connected 10923-16383\n"
            "\n"
            "> CLUSTER KEYSLOT user:{42}:name\n"
            "(integer) 8000\n"
            "\n"
            "> GET moved:{demo}:key\n"
            "MOVED 8000 127.0.0.1:6367\n"
            "正在跟随 MOVED 到 127.0.0.1:6367\n");
    }
    if (diagnostics_output_) {
        diagnostics_output_->setPlainText(
            "> GET /healthz\n"
            "{\"status\":\"ok\"}\n"
            "\n"
            "> GET /readyz\n"
            "{\"ready\":true,\"aof_rewrite_running\":false,\"snapshot_running\":false}\n"
            "\n"
            "> INFO persistence\n"
            "loading:0\n"
            "appendonly_enabled:1\n"
            "aof_rewrite_last_status:ok\n");
    }
    if (metrics_output_) {
        metrics_output_->setPlainText(
            "# HELP miniredis_total_commands Total RESP commands processed\n"
            "# TYPE miniredis_total_commands counter\n"
            "miniredis_total_commands 12843\n"
            "miniredis_connected_clients 6\n"
            "miniredis_key_count 128\n"
            "miniredis_hit_rate 0.9374\n"
            "miniredis_avg_command_latency_us 418\n"
            "miniredis_aof_rewrite_failures 0\n"
            "miniredis_snapshot_failures 0\n");
    }
    if (benchmark_output_) {
        benchmark_status_label_->setText("完成");
        benchmark_status_label_->setStyleSheet("background:#dcfce7;color:#166534;border-radius:6px;padding:5px 10px;font-weight:700;");
        benchmark_output_->setPlainText(
            "> redis-benchmark -h 127.0.0.1 -p 6366 -n 100000 -c 50 -d 64 -t set,get\n"
            "====== SET ======\n"
            "  100000 requests completed in 3.34 seconds\n"
            "  50 parallel clients\n"
            "  throughput summary: 29922.20 requests per second\n"
            "  latency summary (msec): p50=1.551 p95=2.879 p99=3.831 max=8.631\n"
            "\n"
            "====== GET ======\n"
            "  100000 requests completed in 3.39 seconds\n"
            "  50 parallel clients\n"
            "  throughput summary: 29455.08 requests per second\n"
            "  latency summary (msec): p50=1.583 p95=2.823 p99=3.671 max=6.791\n");
    }

    const QVector<std::pair<int, QString>> captures{
        {0, "qt-console.png"},
        {1, "qt-demo-lab.png"},
        {2, "qt-server.png"},
        {3, "qt-cluster.png"},
        {4, "qt-stats.png"},
        {7, "qt-benchmark.png"},
    };

    bool ok = true;
    for (const auto& [index, filename] : captures) {
        pages_->setCurrentIndex(index);
        QCoreApplication::processEvents();
        QPixmap image = grab();
        ok = image.save(dir.filePath(filename), "PNG") && ok;
    }
    return ok;
}

void MonitorWindow::initConnectionControls(QWidget* parent) {
    host_edit_ = new QLineEdit("127.0.0.1", parent);
    resp_port_spin_ = new QSpinBox(parent);
    resp_port_spin_->setRange(1, 65535);
    resp_port_spin_->setValue(6366);
    stats_port_spin_ = new QSpinBox(parent);
    stats_port_spin_->setRange(1, 65535);
    stats_port_spin_->setValue(8080);
    password_edit_ = new QLineEdit(parent);
    password_edit_->setEchoMode(QLineEdit::Password);
    follow_moved_check_ = new QCheckBox("自动跟随 MOVED", parent);
    follow_moved_check_->setChecked(true);
    status_label_ = new QLabel("未连接", parent);
    status_label_->setProperty("role", "badge");
    status_label_->setAlignment(Qt::AlignCenter);

    host_edit_->hide();
    resp_port_spin_->hide();
    stats_port_spin_->hide();
    password_edit_->hide();
    follow_moved_check_->hide();
}

void MonitorWindow::showConnectionDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("添加 MiniRedis 连接");
    dialog.setModal(true);
    dialog.resize(520, 260);

    QVBoxLayout* root = new QVBoxLayout(&dialog);
    root->setSpacing(12);

    QLabel* title = new QLabel("MiniRedis 数据源", &dialog);
    title->setProperty("role", "section");
    QLabel* hint = new QLabel("配置一个 RESP 连接和 HTTP 监控端口，类似数据库客户端中的添加数据源。", &dialog);
    hint->setProperty("role", "muted");
    hint->setWordWrap(true);
    root->addWidget(title);
    root->addWidget(hint);

    QGridLayout* form = new QGridLayout();
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(10);
    QLineEdit* host = new QLineEdit(host_edit_->text(), &dialog);
    QSpinBox* resp_port = new QSpinBox(&dialog);
    resp_port->setRange(1, 65535);
    resp_port->setValue(resp_port_spin_->value());
    QSpinBox* stats_port = new QSpinBox(&dialog);
    stats_port->setRange(1, 65535);
    stats_port->setValue(stats_port_spin_->value());
    QLineEdit* password = new QLineEdit(password_edit_->text(), &dialog);
    password->setEchoMode(QLineEdit::Password);
    QCheckBox* follow_moved = new QCheckBox("自动跟随 MOVED 重定向", &dialog);
    follow_moved->setChecked(follow_moved_check_->isChecked());

    form->addWidget(new QLabel("主机", &dialog), 0, 0);
    form->addWidget(host, 0, 1);
    form->addWidget(new QLabel("RESP 端口", &dialog), 0, 2);
    form->addWidget(resp_port, 0, 3);
    form->addWidget(new QLabel("监控端口", &dialog), 1, 0);
    form->addWidget(stats_port, 1, 1);
    form->addWidget(new QLabel("密码", &dialog), 1, 2);
    form->addWidget(password, 1, 3);
    form->addWidget(follow_moved, 2, 1, 1, 3);
    form->setColumnStretch(1, 1);
    form->setColumnStretch(3, 1);
    root->addLayout(form);

    QHBoxLayout* actions = new QHBoxLayout();
    QPushButton* save_btn = new QPushButton("保存", &dialog);
    QPushButton* connect_btn = new QPushButton("连接", &dialog);
    QPushButton* disconnect_btn = new QPushButton("断开", &dialog);
    QPushButton* ping_btn = new QPushButton("PING", &dialog);
    QPushButton* cancel_btn = new QPushButton("取消", &dialog);
    connect_btn->setProperty("class", "primary");
    actions->addStretch(1);
    actions->addWidget(save_btn);
    actions->addWidget(connect_btn);
    actions->addWidget(disconnect_btn);
    actions->addWidget(ping_btn);
    actions->addWidget(cancel_btn);
    root->addLayout(actions);

    auto apply = [this, host, resp_port, stats_port, password, follow_moved]() {
        host_edit_->setText(host->text().trimmed().isEmpty() ? "127.0.0.1" : host->text().trimmed());
        resp_port_spin_->setValue(resp_port->value());
        stats_port_spin_->setValue(stats_port->value());
        password_edit_->setText(password->text());
        follow_moved_check_->setChecked(follow_moved->isChecked());
    };

    connect(save_btn, &QPushButton::clicked, &dialog, [&dialog, apply]() {
        apply();
        dialog.accept();
    });
    connect(connect_btn, &QPushButton::clicked, &dialog, [this, &dialog, apply]() {
        apply();
        resp_client_.connectToServer(this->host(), respPort());
        stats_timer_->start();
        dialog.accept();
    });
    connect(disconnect_btn, &QPushButton::clicked, &dialog, [this, apply]() {
        apply();
        stats_timer_->stop();
        resp_client_.disconnectFromServer();
    });
    connect(ping_btn, &QPushButton::clicked, &dialog, [this, apply]() {
        apply();
        sendCommand({"PING"});
    });
    connect(cancel_btn, &QPushButton::clicked, &dialog, &QDialog::reject);

    dialog.exec();
}

QWidget* MonitorWindow::buildConsoleTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    QHBoxLayout* toolbar = new QHBoxLayout();
    QPushButton* run_btn = new QPushButton("执行选中/全部", page);
    run_btn->setToolTip("有选中文本时只执行选中内容；没有选中时执行编辑器全部内容");
    QPushButton* clear_output_btn = new QPushButton("清空输出", page);
    QPushButton* clear_editor_btn = new QPushButton("清空编辑器", page);
    QPushButton* ping_btn = new QPushButton("PING", page);
    QPushButton* auth_btn = new QPushButton("AUTH", page);
    QPushButton* stats_btn = new QPushButton("刷新指标", page);
    console_template_combo_ = new QComboBox(page);
    console_template_combo_->addItems({
        "插入模板...",
        "PING",
        "SET key value",
        "GET key",
        "GETDEL key",
        "GETEX key EX 60",
        "MGET key1 key2",
        "APPEND key value",
        "TYPE key",
        "INCR counter",
        "EXPIRE key 60",
        "PEXPIRE key 1500",
        "PERSIST key",
        "INFO persistence",
        "INFO replication",
        "CLUSTER INFO",
        "CLUSTER NODES",
        "CLUSTER MEET host:port",
        "CLUSTER FORGET host:port",
        "BGREWRITEAOF"
    });
    console_history_combo_ = new QComboBox(page);
    console_history_combo_->addItem("历史记录...");
    console_history_combo_->setMinimumWidth(180);
    run_btn->setProperty("class", "primary");
    toolbar->addWidget(run_btn);
    toolbar->addWidget(clear_output_btn);
    toolbar->addWidget(clear_editor_btn);
    toolbar->addSpacing(12);
    toolbar->addWidget(console_template_combo_);
    toolbar->addWidget(console_history_combo_);
    toolbar->addWidget(ping_btn);
    toolbar->addWidget(auth_btn);
    toolbar->addWidget(stats_btn);
    toolbar->addStretch(1);

    QSplitter* horizontal = new QSplitter(Qt::Horizontal, page);
    QWidget* resource_shell = new QWidget(horizontal);
    resource_shell->setMinimumWidth(260);
    QVBoxLayout* resource_layout = new QVBoxLayout(resource_shell);
    resource_layout->setContentsMargins(0, 0, 0, 0);
    resource_layout->setSpacing(8);

    QHBoxLayout* resource_toolbar = new QHBoxLayout();
    resource_toolbar->setSpacing(6);
    QLabel* resource_title = new QLabel("资源管理器", resource_shell);
    resource_title->setStyleSheet("font-weight:700;color:#172033;");
    QPushButton* add_connection_btn = new QPushButton("+", resource_shell);
    add_connection_btn->setToolTip("添加 MiniRedis 连接");
    add_connection_btn->setFixedSize(34, 30);
    QPushButton* refresh_connection_btn = new QPushButton("刷新", resource_shell);
    refresh_connection_btn->setToolTip("刷新运行指标和监控指标");
    resource_toolbar->addWidget(resource_title);
    resource_toolbar->addStretch(1);
    resource_toolbar->addWidget(status_label_);
    resource_toolbar->addWidget(add_connection_btn);
    resource_toolbar->addWidget(refresh_connection_btn);
    resource_layout->addLayout(resource_toolbar);

    resource_tree_ = new QTreeWidget(resource_shell);
    resource_tree_->setHeaderHidden(true);
    resource_tree_->setMinimumWidth(230);

    auto addTreeCommand = [this](QTreeWidgetItem* parent, const QString& title, const QString& command) {
        QTreeWidgetItem* item = new QTreeWidgetItem(parent, {title});
        item->setData(0, Qt::UserRole, command);
        return item;
    };
    QTreeWidgetItem* conn = new QTreeWidgetItem(resource_tree_, {"连接"});
    QTreeWidgetItem* current = new QTreeWidgetItem(conn, {"miniredis@127.0.0.1:6366"});
    QTreeWidgetItem* kv = new QTreeWidgetItem(current, {"KV 命令"});
    addTreeCommand(kv, "SET", "SET user:1 tom");
    addTreeCommand(kv, "GET", "GET user:1");
    addTreeCommand(kv, "GETDEL", "GETDEL user:1");
    addTreeCommand(kv, "GETEX", "GETEX user:1 EX 60");
    addTreeCommand(kv, "SETNX", "SETNX user:1 tom");
    addTreeCommand(kv, "MGET", "MGET user:1 user:2 missing");
    addTreeCommand(kv, "APPEND", "APPEND user:1 !");
    addTreeCommand(kv, "STRLEN", "STRLEN user:1");
    addTreeCommand(kv, "TYPE", "TYPE user:1");
    addTreeCommand(kv, "INCR", "INCR counter");
    addTreeCommand(kv, "EXPIRE / TTL", "EXPIRE user:1 60\nTTL user:1");
    addTreeCommand(kv, "PEXPIRE / PTTL", "PEXPIRE user:1 1500\nPTTL user:1");
    addTreeCommand(kv, "PERSIST", "PERSIST user:1");
    addTreeCommand(kv, "DEL / EXISTS", "EXISTS user:1\nDEL user:1");
    QTreeWidgetItem* observability = new QTreeWidgetItem(current, {"观测诊断"});
    addTreeCommand(observability, "INFO", "INFO");
    addTreeCommand(observability, "SLOWLOG", "SLOWLOG LEN\nSLOWLOG GET 10");
    addTreeCommand(observability, "COMMAND", "COMMAND COUNT\nCOMMAND");
    addTreeCommand(observability, "AOF 重写", "BGREWRITEAOF");
    QTreeWidgetItem* cluster = new QTreeWidgetItem(current, {"集群"});
    addTreeCommand(cluster, "CLUSTER INFO", "CLUSTER INFO");
    addTreeCommand(cluster, "CLUSTER NODES", "CLUSTER NODES");
    addTreeCommand(cluster, "CLUSTER SLOTS", "CLUSTER SLOTS");
    addTreeCommand(cluster, "CLUSTER KEYSLOT", "CLUSTER KEYSLOT user:{42}:name");
    addTreeCommand(cluster, "CLUSTER MEET", "CLUSTER MEET 127.0.0.1:6368");
    addTreeCommand(cluster, "CLUSTER FORGET", "CLUSTER FORGET 127.0.0.1:6368");
    addTreeCommand(cluster, "CLUSTER MIGRATE", "CLUSTER MIGRATE 42 127.0.0.1:6367");
    QTreeWidgetItem* acl = new QTreeWidgetItem(current, {"认证 / ACL"});
    addTreeCommand(acl, "AUTH", "AUTH change-me");
    addTreeCommand(acl, "ACL WHOAMI", "ACL WHOAMI");
    addTreeCommand(acl, "ACL LIST", "ACL LIST");
    resource_tree_->expandAll();

    QSplitter* vertical = new QSplitter(Qt::Vertical, horizontal);
    QWidget* editor_shell = new QWidget(vertical);
    QVBoxLayout* editor_layout = new QVBoxLayout(editor_shell);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    editor_layout->setSpacing(8);
    QLabel* editor_title = new QLabel("命令编辑器", editor_shell);
    editor_title->setProperty("role", "section");
    console_editor_ = new QPlainTextEdit(editor_shell);
    console_editor_->setProperty("role", "editor");
    console_editor_->setPlaceholderText("输入一条或多条命令，每行一条。例如：\nSET user:1 tom\nGET user:1\nINCR counter");
    console_editor_->setPlainText("PING\nSET demo:counter 0\nINCR demo:counter\nGET demo:counter\nINFO stats");
    editor_layout->addWidget(editor_title);
    editor_layout->addWidget(console_editor_, 1);

    QWidget* output_shell = new QWidget(vertical);
    QVBoxLayout* output_layout = new QVBoxLayout(output_shell);
    output_layout->setContentsMargins(0, 0, 0, 0);
    output_layout->setSpacing(8);
    QLabel* output_title = new QLabel("控制台输出", output_shell);
    output_title->setProperty("role", "section");
    console_output_ = new QPlainTextEdit(output_shell);
    console_output_->setReadOnly(true);
    output_layout->addWidget(output_title);
    output_layout->addWidget(console_output_, 1);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 2);

    QWidget* inspector = new QWidget(horizontal);
    inspector->setMinimumWidth(310);
    QVBoxLayout* inspector_layout = new QVBoxLayout(inspector);
    inspector_layout->setContentsMargins(0, 0, 0, 0);
    inspector_layout->setSpacing(10);

    QGroupBox* summary = new QGroupBox("运行概览", inspector);
    QGridLayout* summary_grid = new QGridLayout(summary);
    auto addSummary = [this, summary_grid, summary](int row, const QString& label, const QString& key) {
        QLabel* name = new QLabel(label, summary);
        name->setProperty("role", "muted");
        QLabel* value = new QLabel("-", summary);
        value->setProperty("role", "badge");
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        console_stat_labels_[key] = value;
        summary_grid->addWidget(name, row, 0);
        summary_grid->addWidget(value, row, 1);
    };
    addSummary(0, "节点", "node_addr");
    addSummary(1, "就绪状态", "ready");
    addSummary(2, "命令数", "total_commands");
    addSummary(3, "Key 数量", "key_count");
    addSummary(4, "命中率", "hit_rate");
    addSummary(5, "连接数", "connected_clients");
    addSummary(6, "平均延迟", "avg_command_latency_us");
    addSummary(7, "已用内存", "used_memory_bytes");

    command_help_ = new QPlainTextEdit(inspector);
    command_help_->setReadOnly(true);
    command_help_->setProperty("role", "help");
    command_help_->setPlainText(
        "常用命令\n"
        "PING [消息]\n"
        "AUTH 密码 | AUTH 用户 密码\n"
        "SET key value [EX 秒数]\n"
        "SETNX key value\n"
        "GET key | GETDEL key | GETEX key EX 秒数\n"
        "MGET key [key ...]\n"
        "APPEND key value | STRLEN key | TYPE key\n"
        "INCR key | DECR key\n"
        "INCRBY key n | DECRBY key n\n"
        "DEL key [key ...] | EXISTS key [key ...]\n"
        "EXPIRE key 秒数 | PEXPIRE key 毫秒\n"
        "TTL key | PTTL key | PERSIST key\n\n"
        "诊断与监控\n"
        "INFO [server|clients|memory|stats|persistence|replication|cluster]\n"
        "SLOWLOG LEN | SLOWLOG GET 10 | SLOWLOG RESET\n"
        "COMMAND | COMMAND COUNT\n"
        "BGREWRITEAOF\n\n"
        "集群\n"
        "CLUSTER INFO | NODES | SLOTS | SLOTMAP\n"
        "CLUSTER KEYSLOT key\n"
        "CLUSTER COUNTKEYSINSLOT slot\n"
        "CLUSTER MEET 节点 | FORGET 节点\n"
        "CLUSTER MIGRATE slot 目标节点\n"
        "ASKING");

    inspector_layout->addWidget(summary);
    inspector_layout->addWidget(command_help_, 1);

    resource_layout->addWidget(resource_tree_, 1);

    horizontal->addWidget(resource_shell);
    horizontal->addWidget(vertical);
    horizontal->addWidget(inspector);
    horizontal->setStretchFactor(0, 0);
    horizontal->setStretchFactor(1, 1);
    horizontal->setStretchFactor(2, 0);

    root->addLayout(toolbar);
    root->addWidget(horizontal, 1);

    connect(run_btn, &QPushButton::clicked, this, &MonitorWindow::executeConsoleScript);
    connect(clear_output_btn, &QPushButton::clicked, console_output_, &QPlainTextEdit::clear);
    connect(clear_editor_btn, &QPushButton::clicked, console_editor_, &QPlainTextEdit::clear);
    connect(ping_btn, &QPushButton::clicked, this, [this]() { insertConsoleTemplate("PING"); });
    connect(auth_btn, &QPushButton::clicked, this, [this]() {
        QString pass = password_edit_ ? password_edit_->text() : "";
        insertConsoleTemplate(pass.isEmpty() ? "AUTH change-me" : "AUTH " + pass);
    });
    connect(stats_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchStats(host(), statsPort());
        stats_client_.fetchMetrics(host(), statsPort());
    });
    connect(add_connection_btn, &QPushButton::clicked, this, &MonitorWindow::showConnectionDialog);
    connect(refresh_connection_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchStats(host(), statsPort());
        stats_client_.fetchMetrics(host(), statsPort());
    });
    connect(console_template_combo_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0) return;
        insertConsoleTemplate(console_template_combo_->itemText(index));
        console_template_combo_->setCurrentIndex(0);
    });
    connect(console_history_combo_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0) return;
        console_editor_->setPlainText(console_history_combo_->itemData(index).toString());
        console_editor_->setFocus();
    });
    connect(resource_tree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int) {
                QString command = item->data(0, Qt::UserRole).toString();
                if (!command.isEmpty()) insertConsoleTemplate(command);
            });

    return page;
}

QWidget* MonitorWindow::buildDemoLabTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    QLabel* title = new QLabel("演示中心", page);
    title->setProperty("role", "section");
    QLabel* desc = new QLabel(
        "一键演示核心能力：复制增量同步、AOF 恢复、Cluster 故障观察。演示日志会显示每一步触发的命令和预期观察点。",
        page);
    desc->setProperty("role", "muted");
    desc->setWordWrap(true);

    QGridLayout* cards = new QGridLayout();
    cards->setHorizontalSpacing(12);
    cards->setVerticalSpacing(12);

    auto makeCard = [page](const QString& title_text, const QString& body_text) {
        QGroupBox* box = new QGroupBox(title_text, page);
        QVBoxLayout* layout = new QVBoxLayout(box);
        QLabel* body = new QLabel(body_text, box);
        body->setProperty("role", "muted");
        body->setWordWrap(true);
        layout->addWidget(body);
        return std::pair<QGroupBox*, QVBoxLayout*>(box, layout);
    };

    auto [replica_box, replica_layout] = makeCard(
        "复制与增量同步",
        "启动主节点和副本节点，写入数据，观察主从 offset 与 backlog；随后停止副本、继续写主节点、重启副本，观察增量同步结果。");
    QPushButton* repl_btn = new QPushButton("运行复制演示", replica_box);
    repl_btn->setProperty("class", "primary");
    replica_layout->addWidget(repl_btn);

    auto [aof_box, aof_layout] = makeCard(
        "AOF 恢复",
        "启动带 AOF 的单节点，写入普通 key 和 TTL key，执行 BGREWRITEAOF，重启服务后用命令验证恢复与 rewrite 状态。");
    QPushButton* aof_btn = new QPushButton("运行 AOF 恢复演示", aof_box);
    aof_btn->setProperty("class", "primary");
    aof_layout->addWidget(aof_btn);

    auto [cluster_box, cluster_layout] = makeCard(
        "集群故障观察",
        "启动三节点 Cluster，查询 slot/node 信息，停止一个非当前节点，等待心跳标记 pfail/fail，并展示 CLUSTER INFO/NODES。");
    QPushButton* cluster_btn = new QPushButton("运行集群演示", cluster_box);
    cluster_btn->setProperty("class", "primary");
    cluster_layout->addWidget(cluster_btn);

    QPushButton* stop_all_btn = new QPushButton("停止所有演示进程", page);
    stop_all_btn->setProperty("class", "danger");
    QPushButton* clear_btn = new QPushButton("清空演示日志", page);

    cards->addWidget(replica_box, 0, 0);
    cards->addWidget(aof_box, 0, 1);
    cards->addWidget(cluster_box, 0, 2);
    cards->setColumnStretch(0, 1);
    cards->setColumnStretch(1, 1);
    cards->setColumnStretch(2, 1);

    QHBoxLayout* actions = new QHBoxLayout();
    actions->addWidget(stop_all_btn);
    actions->addWidget(clear_btn);
    actions->addStretch(1);

    demo_output_ = new QPlainTextEdit(page);
    demo_output_->setReadOnly(true);

    root->addWidget(title);
    root->addWidget(desc);
    root->addLayout(cards);
    root->addLayout(actions);
    root->addWidget(demo_output_, 1);

    connect(repl_btn, &QPushButton::clicked, this, &MonitorWindow::runReplicationPsyncDemo);
    connect(aof_btn, &QPushButton::clicked, this, &MonitorWindow::runAofRecoveryDemo);
    connect(cluster_btn, &QPushButton::clicked, this, &MonitorWindow::runClusterFailoverDemo);
    connect(stop_all_btn, &QPushButton::clicked, this, [this]() {
        appendDemoLog("正在停止所有演示进程...");
        stopBenchmark();
        stopClusterDemo();
        stopServer();
    });
    connect(clear_btn, &QPushButton::clicked, demo_output_, &QPlainTextEdit::clear);

    return page;
}

QWidget* MonitorWindow::buildServerTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);
    QGroupBox* config_box = new QGroupBox("服务启动", page);
    QVBoxLayout* config = new QVBoxLayout(config_box);
    config->setSpacing(12);

    server_path_edit_ = new QLineEdit("build/miniredis", config_box);
    bind_edit_ = new QLineEdit("127.0.0.1", config_box);
    snapshot_file_edit_ = new QLineEdit("build/qt_snapshot.dat", config_box);
    appendonly_file_edit_ = new QLineEdit("build/qt_appendonly.aof", config_box);
    cluster_config_file_edit_ = new QLineEdit("build/qt_cluster.conf", config_box);
    replicaof_edit_ = new QLineEdit("", config_box);
    replicaof_edit_->setPlaceholderText("主节点 ip:port，主节点留空");
    replicas_edit_ = new QLineEdit("", config_box);
    replicas_edit_->setPlaceholderText("副本1:port,副本2:port");
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
    io_threads_spin_ = new QSpinBox(config_box);
    io_threads_spin_->setRange(1, 128);
    io_threads_spin_->setValue(4);
    cache_shards_spin_ = new QSpinBox(config_box);
    cache_shards_spin_->setRange(1, 1024);
    cache_shards_spin_->setValue(16);
    maxmemory_spin_ = new QSpinBox(config_box);
    maxmemory_spin_->setRange(0, 2147483647);
    maxmemory_spin_->setValue(0);
    maxmemory_spin_->setSuffix(" B");
    slowlog_threshold_spin_ = new QSpinBox(config_box);
    slowlog_threshold_spin_->setRange(0, 2147483647);
    slowlog_threshold_spin_->setValue(10000);
    slowlog_threshold_spin_->setSuffix(" us");
    slowlog_max_len_spin_ = new QSpinBox(config_box);
    slowlog_max_len_spin_->setRange(0, 100000);
    slowlog_max_len_spin_->setValue(128);
    eviction_policy_combo_ = new QComboBox(config_box);
    eviction_policy_combo_->addItems({"noeviction", "lru"});
    appendfsync_combo_ = new QComboBox(config_box);
    appendfsync_combo_->addItems({"everysec", "always", "no"});
    aof_check_ = new QCheckBox("AOF", config_box);
    cluster_check_ = new QCheckBox("集群模式", config_box);
    server_status_label_ = new QLabel("已停止", config_box);
    server_status_label_->setMinimumWidth(190);
    server_status_label_->setAlignment(Qt::AlignCenter);
    server_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QPushButton* browse_server_btn = new QPushButton("选择", config_box);
    QPushButton* browse_snapshot_btn = new QPushButton("选择", config_box);
    QPushButton* browse_aof_btn = new QPushButton("选择", config_box);
    QPushButton* browse_cluster_config_btn = new QPushButton("选择", config_box);
    QPushButton* start_btn = new QPushButton("启动服务", config_box);
    QPushButton* stop_btn = new QPushButton("停止服务", config_box);
    QPushButton* restart_btn = new QPushButton("重启服务", config_box);
    QPushButton* persistence_demo_btn = new QPushButton("持久化演示", config_box);
    QPushButton* rewrite_aof_btn = new QPushButton("重写 AOF", config_box);
    QPushButton* start_replica_btn = new QPushButton("启动主从", config_box);
    QPushButton* start_cluster_btn = new QPushButton("启动三节点集群", config_box);
    QPushButton* stop_cluster_btn = new QPushButton("停止集群", config_box);
    QPushButton* fail_cluster_btn = new QPushButton("模拟节点故障", config_box);
    QPushButton* connect_btn = new QPushButton("连接", config_box);
    start_btn->setProperty("class", "primary");
    start_cluster_btn->setProperty("class", "primary");
    stop_btn->setProperty("class", "danger");
    stop_cluster_btn->setProperty("class", "danger");
    fail_cluster_btn->setProperty("class", "danger");

    auto makeSectionLabel = [config_box](const QString& text) {
        QLabel* label = new QLabel(text, config_box);
        label->setProperty("role", "muted");
        label->setStyleSheet("font-weight:700;color:#334155;");
        return label;
    };
    auto makeFieldLabel = [config_box](const QString& text) {
        QLabel* label = new QLabel(text, config_box);
        label->setMinimumWidth(105);
        label->setProperty("role", "muted");
        return label;
    };
    auto tuneGrid = [](QGridLayout* grid) {
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(8);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
    };

    QGridLayout* binary_row = new QGridLayout();
    binary_row->setHorizontalSpacing(10);
    binary_row->addWidget(makeFieldLabel("程序路径"), 0, 0);
    binary_row->addWidget(server_path_edit_, 0, 1);
    binary_row->addWidget(browse_server_btn, 0, 2);
    binary_row->setColumnStretch(1, 1);
    config->addLayout(binary_row);

    QHBoxLayout* config_columns = new QHBoxLayout();
    config_columns->setSpacing(16);
    QVBoxLayout* left_column = new QVBoxLayout();
    QVBoxLayout* right_column = new QVBoxLayout();
    left_column->setSpacing(12);
    right_column->setSpacing(12);
    config_columns->addLayout(left_column, 1);
    config_columns->addLayout(right_column, 1);

    QGridLayout* connection_grid = new QGridLayout();
    tuneGrid(connection_grid);
    connection_grid->addWidget(makeSectionLabel("连接"), 0, 0, 1, 4);
    connection_grid->addWidget(makeFieldLabel("监听地址"), 1, 0);
    connection_grid->addWidget(bind_edit_, 1, 1);
    connection_grid->addWidget(makeFieldLabel("RESP 端口"), 1, 2);
    connection_grid->addWidget(server_resp_port_spin_, 1, 3);
    connection_grid->addWidget(makeFieldLabel("监控端口"), 2, 0);
    connection_grid->addWidget(server_stats_port_spin_, 2, 1);
    connection_grid->addWidget(makeFieldLabel("最大连接"), 2, 2);
    connection_grid->addWidget(max_clients_spin_, 2, 3);
    left_column->addLayout(connection_grid);

    QGridLayout* persistence_grid = new QGridLayout();
    tuneGrid(persistence_grid);
    persistence_grid->addWidget(makeSectionLabel("持久化"), 0, 0, 1, 4);
    persistence_grid->addWidget(makeFieldLabel("快照文件"), 1, 0);
    persistence_grid->addWidget(snapshot_file_edit_, 1, 1, 1, 2);
    persistence_grid->addWidget(browse_snapshot_btn, 1, 3);
    persistence_grid->addWidget(makeFieldLabel("快照间隔"), 2, 0);
    persistence_grid->addWidget(snapshot_interval_spin_, 2, 1);
    persistence_grid->addWidget(aof_check_, 2, 2);
    persistence_grid->addWidget(appendfsync_combo_, 2, 3);
    persistence_grid->addWidget(makeFieldLabel("AOF 文件"), 3, 0);
    persistence_grid->addWidget(appendonly_file_edit_, 3, 1, 1, 2);
    persistence_grid->addWidget(browse_aof_btn, 3, 3);
    left_column->addLayout(persistence_grid);

    QGridLayout* runtime_grid = new QGridLayout();
    tuneGrid(runtime_grid);
    runtime_grid->addWidget(makeSectionLabel("运行参数"), 0, 0, 1, 4);
    runtime_grid->addWidget(makeFieldLabel("IO 线程"), 1, 0);
    runtime_grid->addWidget(io_threads_spin_, 1, 1);
    runtime_grid->addWidget(makeFieldLabel("缓存分片"), 1, 2);
    runtime_grid->addWidget(cache_shards_spin_, 1, 3);
    runtime_grid->addWidget(makeFieldLabel("内存上限"), 2, 0);
    runtime_grid->addWidget(maxmemory_spin_, 2, 1);
    runtime_grid->addWidget(makeFieldLabel("淘汰策略"), 2, 2);
    runtime_grid->addWidget(eviction_policy_combo_, 2, 3);
    runtime_grid->addWidget(makeFieldLabel("慢日志阈值"), 3, 0);
    runtime_grid->addWidget(slowlog_threshold_spin_, 3, 1);
    runtime_grid->addWidget(makeFieldLabel("慢日志容量"), 3, 2);
    runtime_grid->addWidget(slowlog_max_len_spin_, 3, 3);
    right_column->addLayout(runtime_grid);

    QGridLayout* cluster_grid = new QGridLayout();
    tuneGrid(cluster_grid);
    cluster_grid->addWidget(makeSectionLabel("集群 / 复制"), 0, 0, 1, 4);
    cluster_grid->addWidget(cluster_check_, 1, 0);
    cluster_grid->addWidget(cluster_config_file_edit_, 1, 1, 1, 2);
    cluster_grid->addWidget(browse_cluster_config_btn, 1, 3);
    cluster_grid->addWidget(makeFieldLabel("当前节点"), 2, 0);
    cluster_grid->addWidget(node_addr_edit_, 2, 1);
    cluster_grid->addWidget(makeFieldLabel("节点列表"), 2, 2);
    cluster_grid->addWidget(nodes_edit_, 2, 3);
    cluster_grid->addWidget(makeFieldLabel("复制来源"), 3, 0);
    cluster_grid->addWidget(replicaof_edit_, 3, 1);
    cluster_grid->addWidget(makeFieldLabel("副本列表"), 3, 2);
    cluster_grid->addWidget(replicas_edit_, 3, 3);
    right_column->addLayout(cluster_grid);
    left_column->addStretch(1);
    right_column->addStretch(1);
    config->addLayout(config_columns);

    QHBoxLayout* status_row = new QHBoxLayout();
    status_row->setSpacing(10);
    status_row->addWidget(makeFieldLabel("状态"));
    status_row->addWidget(server_status_label_);
    status_row->addStretch(1);
    status_row->addWidget(start_btn);
    status_row->addWidget(stop_btn);
    status_row->addWidget(restart_btn);
    status_row->addWidget(connect_btn);
    config->addLayout(status_row);

    QGridLayout* action_grid = new QGridLayout();
    action_grid->setHorizontalSpacing(10);
    action_grid->setVerticalSpacing(8);
    action_grid->addWidget(persistence_demo_btn, 0, 0);
    action_grid->addWidget(rewrite_aof_btn, 0, 1);
    action_grid->addWidget(start_replica_btn, 0, 2);
    action_grid->addWidget(start_cluster_btn, 0, 3);
    action_grid->addWidget(stop_cluster_btn, 0, 4);
    action_grid->addWidget(fail_cluster_btn, 0, 5);
    for (int col = 0; col < 6; ++col) action_grid->setColumnStretch(col, 1);
    config->addLayout(action_grid);

    QGroupBox* log_box = new QGroupBox("进程日志", page);
    QVBoxLayout* log_layout = new QVBoxLayout(log_box);
    server_log_ = new QPlainTextEdit(log_box);
    server_log_->setReadOnly(true);
    log_layout->addWidget(server_log_);

    root->addWidget(config_box);
    root->addWidget(log_box, 1);

    connect(browse_server_btn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "选择 miniredis 可执行文件", QDir::currentPath());
        if (!path.isEmpty()) server_path_edit_->setText(path);
    });
    connect(browse_snapshot_btn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "选择快照文件", QDir::currentPath());
        if (!path.isEmpty()) snapshot_file_edit_->setText(path);
    });
    connect(browse_aof_btn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "选择 AOF 文件", QDir::currentPath());
        if (!path.isEmpty()) appendonly_file_edit_->setText(path);
    });
    connect(browse_cluster_config_btn, &QPushButton::clicked, this, [this]() {
        QString path = QFileDialog::getSaveFileName(this, "选择集群配置文件", QDir::currentPath());
        if (!path.isEmpty()) cluster_config_file_edit_->setText(path);
    });
    connect(start_btn, &QPushButton::clicked, this, &MonitorWindow::startServer);
    connect(stop_btn, &QPushButton::clicked, this, &MonitorWindow::stopServer);
    connect(restart_btn, &QPushButton::clicked, this, &MonitorWindow::restartServer);
    connect(persistence_demo_btn, &QPushButton::clicked, this, &MonitorWindow::runPersistenceDemo);
    connect(rewrite_aof_btn, &QPushButton::clicked, this, [this]() { sendCommand({"BGREWRITEAOF"}); });
    connect(start_replica_btn, &QPushButton::clicked, this, &MonitorWindow::startReplicaDemo);
    connect(start_cluster_btn, &QPushButton::clicked, this, &MonitorWindow::startClusterDemo);
    connect(stop_cluster_btn, &QPushButton::clicked, this, &MonitorWindow::stopClusterDemo);
    connect(fail_cluster_btn, &QPushButton::clicked, this, &MonitorWindow::failClusterDemoNode);
    connect(connect_btn, &QPushButton::clicked, this, [this]() {
        resp_client_.connectToServer(host(), respPort());
        stats_timer_->start();
    });

    return page;
}

QWidget* MonitorWindow::buildClusterTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    QGridLayout* controls = new QGridLayout();
    controls->setHorizontalSpacing(8);
    controls->setVerticalSpacing(8);

    QLineEdit* key_edit = new QLineEdit(page);
    key_edit->setPlaceholderText("user:{42}:name");
    QLineEdit* target_node_edit = new QLineEdit("127.0.0.1:6367", page);
    target_node_edit->setPlaceholderText("127.0.0.1:6367");
    QSpinBox* slot_spin = new QSpinBox(page);
    slot_spin->setRange(0, 16383);
    QPushButton* info_btn = new QPushButton("CLUSTER INFO", page);
    QPushButton* info_section_btn = new QPushButton("INFO 集群", page);
    QPushButton* nodes_btn = new QPushButton("CLUSTER NODES", page);
    QPushButton* slots_btn = new QPushButton("CLUSTER SLOTS", page);
    QPushButton* myid_btn = new QPushButton("CLUSTER MYID", page);
    QPushButton* keyslot_btn = new QPushButton("计算槽位", page);
    QPushButton* count_slot_btn = new QPushButton("统计槽位 Key", page);
    QPushButton* moved_probe_btn = new QPushButton("测试 MOVED", page);
    QPushButton* migration_demo_btn = new QPushButton("迁移演示", page);
    QPushButton* migrate_slot_btn = new QPushButton("迁移槽位", page);
    QPushButton* setslot_node_btn = new QPushButton("SETSLOT NODE", page);
    QPushButton* setslot_migrating_btn = new QPushButton("MIGRATING", page);
    QPushButton* setslot_importing_btn = new QPushButton("IMPORTING", page);
    QPushButton* setslot_stable_btn = new QPushButton("STABLE", page);
    cluster_output_ = new QPlainTextEdit(page);
    cluster_output_->setReadOnly(true);

    controls->addWidget(info_btn, 0, 0);
    controls->addWidget(info_section_btn, 0, 1);
    controls->addWidget(nodes_btn, 0, 2);
    controls->addWidget(slots_btn, 0, 3);
    controls->addWidget(myid_btn, 0, 4);
    controls->addWidget(new QLabel("Key"), 1, 0);
    controls->addWidget(key_edit, 1, 1, 1, 2);
    controls->addWidget(keyslot_btn, 1, 3);
    controls->addWidget(moved_probe_btn, 1, 4);
    controls->addWidget(migration_demo_btn, 1, 5);
    controls->addWidget(new QLabel("槽位"), 2, 0);
    controls->addWidget(slot_spin, 2, 1);
    controls->addWidget(count_slot_btn, 2, 2);
    controls->addWidget(new QLabel("目标节点"), 3, 0);
    controls->addWidget(target_node_edit, 3, 1, 1, 2);
    controls->addWidget(migrate_slot_btn, 3, 3);
    controls->addWidget(setslot_node_btn, 3, 4);
    controls->addWidget(setslot_migrating_btn, 4, 1);
    controls->addWidget(setslot_importing_btn, 4, 2);
    controls->addWidget(setslot_stable_btn, 4, 3);
    layout->addLayout(controls);
    layout->addWidget(cluster_output_, 1);

    connect(info_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "INFO"}); });
    connect(info_section_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "cluster"}); });
    connect(nodes_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "NODES"}); });
    connect(slots_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "SLOTS"}); });
    connect(myid_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "MYID"}); });
    connect(keyslot_btn, &QPushButton::clicked, this, [this, key_edit]() {
        QString key = key_edit->text();
        if (key.isEmpty()) {
            appendLog("ERR CLUSTER KEYSLOT 需要填写 key");
            return;
        }
        sendCommand({"CLUSTER", "KEYSLOT", key});
    });
    connect(count_slot_btn, &QPushButton::clicked, this, [this, slot_spin]() {
        sendCommand({"CLUSTER", "COUNTKEYSINSLOT", QString::number(slot_spin->value())});
    });
    connect(moved_probe_btn, &QPushButton::clicked, this, [this, key_edit]() {
        QString key = key_edit->text().trimmed();
        if (key.isEmpty()) key = "moved:{demo}:key";
        sendCommand({"GET", key});
    });
    connect(migration_demo_btn, &QPushButton::clicked, this, [this, key_edit, slot_spin, target_node_edit]() {
        QString target = target_node_edit->text().trimmed();
        if (target.isEmpty()) {
            appendLog("ERR 集群迁移演示需要填写目标节点");
            return;
        }

        QString key;
        int slot = -1;
        for (int i = 1; i <= 10000; ++i) {
            QString candidate = QString("qt:migrate:{demo-%1}").arg(i);
            int candidate_slot = qtClusterHashSlot(candidate);
            if (candidate_slot <= 5461) {
                key = candidate;
                slot = candidate_slot;
                break;
            }
        }
        if (key.isEmpty()) {
            appendLog("ERR 未找到属于第一个节点的演示 key");
            return;
        }

        key_edit->setText(key);
        slot_spin->setValue(slot);
        appendLog(QString("> 集群迁移演示：key=%1 slot=%2 target=%3")
                      .arg(key)
                      .arg(slot)
                      .arg(target));
        sendCommand({"SET", key, "qt-migrated-value"});
        QTimer::singleShot(300, this, [this, slot]() {
            sendCommand({"CLUSTER", "COUNTKEYSINSLOT", QString::number(slot)});
        });
        QTimer::singleShot(600, this, [this, slot, target]() {
            sendCommand({"CLUSTER", "MIGRATE", QString::number(slot), target});
        });
        QTimer::singleShot(1200, this, [this, key]() {
            sendCommand({"GET", key});
        });
        QTimer::singleShot(1500, this, [this]() {
            sendCommand({"CLUSTER", "NODES"});
        });
    });
    connect(migrate_slot_btn, &QPushButton::clicked, this, [this, slot_spin, target_node_edit]() {
        QString target = target_node_edit->text().trimmed();
        if (target.isEmpty()) {
            appendLog("ERR CLUSTER MIGRATE 需要填写目标节点");
            return;
        }
        sendCommand({"CLUSTER", "MIGRATE", QString::number(slot_spin->value()), target});
    });
    connect(setslot_node_btn, &QPushButton::clicked, this, [this, slot_spin, target_node_edit]() {
        QString target = target_node_edit->text().trimmed();
        if (target.isEmpty()) {
            appendLog("ERR CLUSTER SETSLOT NODE 需要填写目标节点");
            return;
        }
        sendCommand({"CLUSTER", "SETSLOT", QString::number(slot_spin->value()), "NODE", target});
    });
    connect(setslot_migrating_btn, &QPushButton::clicked, this, [this, slot_spin, target_node_edit]() {
        QString target = target_node_edit->text().trimmed();
        if (target.isEmpty()) {
            appendLog("ERR CLUSTER SETSLOT MIGRATING 需要填写目标节点");
            return;
        }
        sendCommand({"CLUSTER", "SETSLOT", QString::number(slot_spin->value()), "MIGRATING", target});
    });
    connect(setslot_importing_btn, &QPushButton::clicked, this, [this, slot_spin, target_node_edit]() {
        QString target = target_node_edit->text().trimmed();
        if (target.isEmpty()) {
            appendLog("ERR CLUSTER SETSLOT IMPORTING 需要填写来源节点");
            return;
        }
        sendCommand({"CLUSTER", "SETSLOT", QString::number(slot_spin->value()), "IMPORTING", target});
    });
    connect(setslot_stable_btn, &QPushButton::clicked, this, [this, slot_spin]() {
        sendCommand({"CLUSTER", "SETSLOT", QString::number(slot_spin->value()), "STABLE"});
    });

    return page;
}

QWidget* MonitorWindow::buildStatsTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    QHBoxLayout* toolbar = new QHBoxLayout();
    QLabel* title = new QLabel("运行指标", page);
    title->setProperty("role", "section");
    QPushButton* refresh_btn = new QPushButton("刷新", page);
    refresh_btn->setProperty("class", "primary");
    toolbar->addWidget(title);
    toolbar->addStretch(1);
    toolbar->addWidget(refresh_btn);
    root->addLayout(toolbar);

    QScrollArea* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    QWidget* content = new QWidget(scroll);
    QGridLayout* panels = new QGridLayout(content);
    panels->setHorizontalSpacing(12);
    panels->setVerticalSpacing(12);
    panels->setContentsMargins(0, 0, 0, 0);

    auto makePanel = [page](const QString& title_text) {
        QGroupBox* box = new QGroupBox(title_text, page);
        QGridLayout* layout = new QGridLayout(box);
        layout->setHorizontalSpacing(12);
        layout->setVerticalSpacing(8);
        layout->setColumnStretch(1, 1);
        return std::pair<QGroupBox*, QGridLayout*>(box, layout);
    };

    auto addMetric = [this](QGridLayout* layout, int row, const QString& label_text,
                            const QString& key) {
        QLabel* name = new QLabel(label_text, layout->parentWidget());
        name->setProperty("role", "muted");
        QLabel* value = new QLabel("-", layout->parentWidget());
        value->setProperty("role", "badge");
        value->setMinimumWidth(150);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        stat_labels_[key] = value;
        layout->addWidget(name, row, 0);
        layout->addWidget(value, row, 1);
    };

    auto [runtime_box, runtime] = makePanel("运行状态");
    addMetric(runtime, 0, "节点", "node_addr");
    addMetric(runtime, 1, "就绪", "ready");
    addMetric(runtime, 2, "IO 线程", "io_threads");
    addMetric(runtime, 3, "缓存分片", "cache_shards");

    auto [traffic_box, traffic] = makePanel("流量与延迟");
    addMetric(traffic, 0, "命令总数", "total_commands");
    addMetric(traffic, 1, "GET 命中", "get_hits");
    addMetric(traffic, 2, "GET 未命中", "get_misses");
    addMetric(traffic, 3, "命中率", "hit_rate");
    addMetric(traffic, 4, "延迟样本", "latency_samples");
    addMetric(traffic, 5, "平均延迟", "avg_command_latency_us");
    addMetric(traffic, 6, "最大延迟", "max_command_latency_us");

    auto [memory_box, memory] = makePanel("内存");
    addMetric(memory, 0, "Key 数量", "key_count");
    addMetric(memory, 1, "已用内存", "used_memory_bytes");
    addMetric(memory, 2, "内存上限", "maxmemory_bytes");
    addMetric(memory, 3, "淘汰 Key", "evicted_keys");
    addMetric(memory, 4, "内存池已用", "mem_pool_used_blocks");
    addMetric(memory, 5, "内存池空闲", "mem_pool_free_blocks");

    auto [clients_box, clients] = makePanel("连接与慢日志");
    addMetric(clients, 0, "当前连接", "connected_clients");
    addMetric(clients, 1, "累计接入", "total_connections");
    addMetric(clients, 2, "拒绝连接", "rejected_connections");
    addMetric(clients, 3, "慢日志条数", "slowlog_len");
    addMetric(clients, 4, "慢日志阈值", "slowlog_log_slower_than_us");
    addMetric(clients, 5, "慢日志容量", "slowlog_max_len");

    auto [protection_box, protection] = makePanel("资源保护");
    addMetric(protection, 0, "最大连接", "max_clients");
    addMetric(protection, 1, "请求大小上限", "max_request_bytes");
    addMetric(protection, 2, "Key 大小上限", "max_key_bytes");
    addMetric(protection, 3, "Value 大小上限", "max_value_bytes");
    addMetric(protection, 4, "Pipeline 上限", "max_pipeline_commands");
    addMetric(protection, 5, "输出缓冲上限", "client_output_buffer_limit");

    auto [persistence_box, persistence] = makePanel("持久化");
    addMetric(persistence, 0, "快照运行中", "snapshot_running");
    addMetric(persistence, 1, "快照成功时间", "snapshot_last_success_unix_ms");
    addMetric(persistence, 2, "快照失败时间", "snapshot_last_failure_unix_ms");
    addMetric(persistence, 3, "快照耗时", "snapshot_last_duration_ms");
    addMetric(persistence, 4, "快照 Key 数", "snapshot_last_key_count");
    addMetric(persistence, 5, "快照失败数", "snapshot_failures");

    auto [aof_box, aof] = makePanel("AOF 重写");
    addMetric(aof, 0, "重写运行中", "aof_rewrite_running");
    addMetric(aof, 1, "重写缓冲", "aof_rewrite_buffer_bytes");
    addMetric(aof, 2, "最近重写", "aof_last_rewrite_unix_ms");
    addMetric(aof, 3, "重写失败时间", "aof_last_rewrite_failure_unix_ms");
    addMetric(aof, 4, "重写耗时", "aof_last_rewrite_duration_ms");
    addMetric(aof, 5, "重写记录数", "aof_last_rewrite_records");
    addMetric(aof, 6, "重写失败数", "aof_rewrite_failures");
    addMetric(aof, 7, "最近状态", "aof_rewrite_last_status");
    addMetric(aof, 8, "最近错误", "aof_rewrite_last_error");

    panels->addWidget(runtime_box, 0, 0);
    panels->addWidget(traffic_box, 0, 1);
    panels->addWidget(memory_box, 0, 2);
    panels->addWidget(clients_box, 1, 0);
    panels->addWidget(protection_box, 1, 1);
    panels->addWidget(persistence_box, 1, 2);
    panels->addWidget(aof_box, 2, 0, 1, 3);
    panels->setColumnStretch(0, 1);
    panels->setColumnStretch(1, 1);
    panels->setColumnStretch(2, 1);
    panels->setRowStretch(3, 1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    connect(refresh_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchStats(host(), statsPort());
        stats_client_.fetchMetrics(host(), statsPort());
    });

    return page;
}

QWidget* MonitorWindow::buildMetricsTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    QGroupBox* box = new QGroupBox("Prometheus 监控指标", page);
    QVBoxLayout* box_layout = new QVBoxLayout(box);
    metrics_output_ = new QPlainTextEdit(box);
    metrics_output_->setReadOnly(true);
    box_layout->addWidget(metrics_output_);
    layout->addWidget(box);
    return page;
}

QWidget* MonitorWindow::buildDiagnosticsTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    QGroupBox* http_box = new QGroupBox("HTTP 健康检查", page);
    QHBoxLayout* http = new QHBoxLayout(http_box);
    QPushButton* health_btn = new QPushButton("存活检查", http_box);
    QPushButton* ready_btn = new QPushButton("就绪检查", http_box);
    QPushButton* stats_btn = new QPushButton("统计 JSON", http_box);
    QPushButton* metrics_btn = new QPushButton("Prometheus 指标", http_box);
    health_btn->setProperty("class", "primary");
    ready_btn->setProperty("class", "primary");
    http->addWidget(health_btn);
    http->addWidget(ready_btn);
    http->addWidget(stats_btn);
    http->addWidget(metrics_btn);
    http->addStretch(1);

    QGroupBox* resp_box = new QGroupBox("RESP 诊断命令", page);
    QGridLayout* resp = new QGridLayout(resp_box);
    resp->setHorizontalSpacing(8);
    resp->setVerticalSpacing(8);
    QPushButton* info_all_btn = new QPushButton("INFO 全部", resp_box);
    QPushButton* info_server_btn = new QPushButton("INFO 服务", resp_box);
    QPushButton* info_clients_btn = new QPushButton("INFO 连接", resp_box);
    QPushButton* info_memory_btn = new QPushButton("INFO 内存", resp_box);
    QPushButton* info_stats_btn = new QPushButton("INFO 统计", resp_box);
    QPushButton* info_persistence_btn = new QPushButton("INFO 持久化", resp_box);
    QPushButton* info_replication_btn = new QPushButton("INFO 复制", resp_box);
    QPushButton* info_cluster_btn = new QPushButton("INFO 集群", resp_box);
    QPushButton* acl_whoami_btn = new QPushButton("ACL WHOAMI", resp_box);
    QPushButton* acl_list_btn = new QPushButton("ACL LIST", resp_box);
    QPushButton* slowlog_len_btn = new QPushButton("SLOWLOG LEN", resp_box);
    QPushButton* slowlog_get_btn = new QPushButton("SLOWLOG GET", resp_box);
    QPushButton* command_count_btn = new QPushButton("COMMAND COUNT", resp_box);
    QPushButton* command_btn = new QPushButton("COMMAND", resp_box);
    QPushButton* clear_btn = new QPushButton("清空", resp_box);
    clear_btn->setProperty("class", "danger");

    resp->addWidget(info_all_btn, 0, 0);
    resp->addWidget(info_server_btn, 0, 1);
    resp->addWidget(info_clients_btn, 0, 2);
    resp->addWidget(info_memory_btn, 0, 3);
    resp->addWidget(info_stats_btn, 1, 0);
    resp->addWidget(info_persistence_btn, 1, 1);
    resp->addWidget(info_replication_btn, 1, 2);
    resp->addWidget(info_cluster_btn, 1, 3);
    resp->addWidget(acl_whoami_btn, 2, 0);
    resp->addWidget(acl_list_btn, 2, 1);
    resp->addWidget(slowlog_len_btn, 2, 2);
    resp->addWidget(slowlog_get_btn, 2, 3);
    resp->addWidget(command_count_btn, 3, 0);
    resp->addWidget(command_btn, 3, 1);
    resp->addWidget(clear_btn, 3, 3);

    QGroupBox* output_box = new QGroupBox("诊断输出", page);
    QVBoxLayout* output_layout = new QVBoxLayout(output_box);
    diagnostics_output_ = new QPlainTextEdit(output_box);
    diagnostics_output_->setReadOnly(true);
    output_layout->addWidget(diagnostics_output_);

    root->addWidget(http_box);
    root->addWidget(resp_box);
    root->addWidget(output_box, 1);

    connect(health_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchHealthz(host(), statsPort());
    });
    connect(ready_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchReadyz(host(), statsPort());
    });
    connect(stats_btn, &QPushButton::clicked, this, [this]() {
        stats_client_.fetchStatsText(host(), statsPort());
    });
    connect(metrics_btn, &QPushButton::clicked, this, [this]() {
        diagnostics_output_->appendPlainText("> GET /metrics");
        stats_client_.fetchMetrics(host(), statsPort());
        pages_->setCurrentIndex(6);
    });

    connect(info_all_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO"}); });
    connect(info_server_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "server"}); });
    connect(info_clients_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "clients"}); });
    connect(info_memory_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "memory"}); });
    connect(info_stats_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "stats"}); });
    connect(info_persistence_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "persistence"}); });
    connect(info_replication_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "replication"}); });
    connect(info_cluster_btn, &QPushButton::clicked, this, [this]() { sendCommand({"INFO", "cluster"}); });
    connect(acl_whoami_btn, &QPushButton::clicked, this, [this]() { sendCommand({"ACL", "WHOAMI"}); });
    connect(acl_list_btn, &QPushButton::clicked, this, [this]() { sendCommand({"ACL", "LIST"}); });
    connect(slowlog_len_btn, &QPushButton::clicked, this, [this]() { sendCommand({"SLOWLOG", "LEN"}); });
    connect(slowlog_get_btn, &QPushButton::clicked, this, [this]() { sendCommand({"SLOWLOG", "GET", "10"}); });
    connect(command_count_btn, &QPushButton::clicked, this, [this]() { sendCommand({"COMMAND", "COUNT"}); });
    connect(command_btn, &QPushButton::clicked, this, [this]() { sendCommand({"COMMAND"}); });
    connect(clear_btn, &QPushButton::clicked, diagnostics_output_, &QPlainTextEdit::clear);

    return page;
}

QWidget* MonitorWindow::buildBenchmarkTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(page);
    root->setContentsMargins(12, 12, 12, 12);
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
    benchmark_status_label_ = new QLabel("空闲", page);
    benchmark_status_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QPushButton* run_btn = new QPushButton("运行 SET/GET", page);
    QPushButton* matrix_btn = new QPushButton("运行矩阵压测", page);
    QPushButton* stop_btn = new QPushButton("停止", page);
    QPushButton* clear_btn = new QPushButton("清空", page);
    run_btn->setProperty("class", "primary");
    matrix_btn->setProperty("class", "primary");
    stop_btn->setProperty("class", "danger");
    benchmark_output_ = new QPlainTextEdit(page);
    benchmark_output_->setReadOnly(true);

    controls->addWidget(new QLabel("请求数", page), 0, 0);
    controls->addWidget(benchmark_requests_spin_, 0, 1);
    controls->addWidget(new QLabel("客户端数", page), 0, 2);
    controls->addWidget(benchmark_clients_spin_, 0, 3);
    controls->addWidget(new QLabel("值字节数", page), 0, 4);
    controls->addWidget(benchmark_payload_spin_, 0, 5);
    controls->addWidget(new QLabel("状态", page), 1, 0);
    controls->addWidget(benchmark_status_label_, 1, 1);
    controls->addWidget(run_btn, 1, 2);
    controls->addWidget(matrix_btn, 1, 3);
    controls->addWidget(stop_btn, 1, 4);
    controls->addWidget(clear_btn, 1, 5);
    root->addLayout(controls);
    root->addWidget(benchmark_output_, 1);

    connect(run_btn, &QPushButton::clicked, this, &MonitorWindow::runBenchmark);
    connect(matrix_btn, &QPushButton::clicked, this, &MonitorWindow::runBenchmarkMatrix);
    connect(stop_btn, &QPushButton::clicked, this, &MonitorWindow::stopBenchmark);
    connect(clear_btn, &QPushButton::clicked, benchmark_output_, &QPlainTextEdit::clear);

    return page;
}

void MonitorWindow::startServer() {
    if (server_process_->state() != QProcess::NotRunning) {
        appendServerLog("服务已经在运行");
        return;
    }

    QString program = server_path_edit_->text().trimmed();
    if (program.isEmpty()) {
        appendServerLog("ERR 服务程序路径为空");
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
        "--max-clients", QString::number(max_clients_spin_->value()),
        "--io-threads", QString::number(io_threads_spin_->value()),
        "--cache-shards", QString::number(cache_shards_spin_->value()),
        "--maxmemory", QString::number(maxmemory_spin_->value()),
        "--eviction-policy", eviction_policy_combo_->currentText(),
        "--slowlog-log-slower-than-us", QString::number(slowlog_threshold_spin_->value()),
        "--slowlog-max-len", QString::number(slowlog_max_len_spin_->value())
    };

    if (!replicaof_edit_->text().trimmed().isEmpty()) {
        args << "--replicaof" << replicaof_edit_->text().trimmed();
    }
    if (!replicas_edit_->text().trimmed().isEmpty()) {
        args << "--replicas" << replicas_edit_->text().trimmed();
    }
    if (aof_check_->isChecked()) {
        args << "--appendonly-file"
             << (appendonly_file_edit_->text().trimmed().isEmpty()
                     ? "build/qt_appendonly.aof"
                     : appendonly_file_edit_->text().trimmed())
             << "--appendfsync" << appendfsync_combo_->currentText();
    }
    if (cluster_check_->isChecked()) {
        args << "--cluster";
        if (!node_addr_edit_->text().trimmed().isEmpty()) args << "--node-addr" << node_addr_edit_->text().trimmed();
        if (!nodes_edit_->text().trimmed().isEmpty()) args << "--nodes" << nodes_edit_->text().trimmed();
        if (!cluster_config_file_edit_->text().trimmed().isEmpty()) {
            args << "--cluster-config-file" << cluster_config_file_edit_->text().trimmed();
        }
    }

    appendServerLog("> " + program + " " + maskedProcessArgs(args).join(' '));
    server_process_->setProcessEnvironment(processEnvironmentWithPassword(password_edit_->text()));
    server_process_->setWorkingDirectory(QDir::currentPath());
    server_process_->start(program, args);
}

void MonitorWindow::stopServer() {
    if (server_process_->state() == QProcess::NotRunning) {
        appendServerLog("服务未运行");
        return;
    }
    appendServerLog("正在停止服务进程...");
    stats_timer_->stop();
    resp_client_.disconnectFromServer();
    server_process_->terminate();
    if (!server_process_->waitForFinished(3000)) {
        appendServerLog("服务未在 SIGTERM 后退出，正在强制结束");
        server_process_->kill();
    }
}

void MonitorWindow::restartServer() {
    if (server_process_->state() == QProcess::NotRunning) {
        appendServerLog("服务未运行，正在启动新实例");
        startServer();
        return;
    }

    appendServerLog("正在重启服务以验证 snapshot/AOF 恢复...");
    stopServer();
    QTimer::singleShot(1200, this, [this]() {
        startServer();
        QTimer::singleShot(800, this, [this]() {
            resp_client_.connectToServer(host(), respPort());
            stats_timer_->start();
        });
    });
}

void MonitorWindow::runPersistenceDemo() {
    auto send_demo = [this]() {
        appendLog("> 持久化演示");
        sendCommand({"SET", "persist:plain", "value-from-qt"});
        sendCommand({"SET", "persist:ttl", "live", "EX", "60"});
        sendCommand({"EXPIRE", "persist:plain", "60"});
        sendCommand({"TTL", "persist:plain"});
        sendCommand({"INFO", "persistence"});
    };

    if (!resp_client_.isConnected()) {
        resp_client_.connectToServer(host(), respPort());
        QTimer::singleShot(500, this, send_demo);
        return;
    }

    send_demo();
}

void MonitorWindow::startReplicaDemo() {
    if (server_process_->state() != QProcess::NotRunning || !cluster_processes_.isEmpty()) {
        appendServerLog("启动主从演示前，请先停止当前服务或集群演示");
        return;
    }

    QString program = server_path_edit_->text().trimmed();
    if (program.isEmpty()) {
        appendServerLog("ERR 服务程序路径为空");
        return;
    }

    const QString bind = bind_edit_->text().trimmed().isEmpty() ? "127.0.0.1" : bind_edit_->text().trimmed();
    const QString connect_host = (bind == "0.0.0.0") ? "127.0.0.1" : bind;
    const int master_port = server_resp_port_spin_->value();
    const int replica_port = master_port + 1;
    const int master_stats_port = server_stats_port_spin_->value();
    const int replica_stats_port = master_stats_port + 1;
    const QString master_node = QString("%1:%2").arg(connect_host).arg(master_port);
    const QString replica_node = QString("%1:%2").arg(connect_host).arg(replica_port);

    QDir().mkpath("build/qt-replica-demo");
    appendServerLog(QString("正在启动主从：master=%1 replica=%2").arg(master_node, replica_node));

    QStringList master_args{
        "--bind", bind,
        "--port", QString::number(master_port),
        "--stats-bind", bind,
        "--stats-port", QString::number(master_stats_port),
        "--snapshot-file", QString("build/qt-replica-demo/snapshot_master_%1.dat").arg(master_port),
        "--snapshot-interval", QString::number(snapshot_interval_spin_->value()),
        "--max-clients", QString::number(max_clients_spin_->value()),
        "--io-threads", QString::number(io_threads_spin_->value()),
        "--cache-shards", QString::number(cache_shards_spin_->value()),
        "--maxmemory", QString::number(maxmemory_spin_->value()),
        "--eviction-policy", eviction_policy_combo_->currentText(),
        "--slowlog-log-slower-than-us", QString::number(slowlog_threshold_spin_->value()),
        "--slowlog-max-len", QString::number(slowlog_max_len_spin_->value()),
        "--replicas", replica_node
    };
    QStringList replica_args{
        "--bind", bind,
        "--port", QString::number(replica_port),
        "--stats-bind", bind,
        "--stats-port", QString::number(replica_stats_port),
        "--snapshot-file", QString("build/qt-replica-demo/snapshot_replica_%1.dat").arg(replica_port),
        "--snapshot-interval", QString::number(snapshot_interval_spin_->value()),
        "--max-clients", QString::number(max_clients_spin_->value()),
        "--io-threads", QString::number(io_threads_spin_->value()),
        "--cache-shards", QString::number(cache_shards_spin_->value()),
        "--maxmemory", QString::number(maxmemory_spin_->value()),
        "--eviction-policy", eviction_policy_combo_->currentText(),
        "--slowlog-log-slower-than-us", QString::number(slowlog_threshold_spin_->value()),
        "--slowlog-max-len", QString::number(slowlog_max_len_spin_->value()),
        "--replicaof", master_node
    };
    if (aof_check_->isChecked()) {
        master_args << "--appendonly-file"
                    << QString("build/qt-replica-demo/appendonly_master_%1.aof").arg(master_port)
                    << "--appendfsync" << appendfsync_combo_->currentText();
        replica_args << "--appendonly-file"
                     << QString("build/qt-replica-demo/appendonly_replica_%1.aof").arg(replica_port)
                     << "--appendfsync" << appendfsync_combo_->currentText();
    }

    appendServerLog("> " + program + " " + maskedProcessArgs(master_args).join(' '));
    server_process_->setProcessEnvironment(processEnvironmentWithPassword(password_edit_->text()));
    server_process_->setWorkingDirectory(QDir::currentPath());
    server_process_->start(program, master_args);

    QProcess* replica_process = new QProcess(this);
    cluster_processes_[replica_port] = replica_process;
    connect(replica_process, &QProcess::readyReadStandardOutput, this, [this, replica_process, replica_port]() {
        appendServerLog(QString("[replica %1] %2").arg(replica_port).arg(QString::fromUtf8(replica_process->readAllStandardOutput())));
    });
    connect(replica_process, &QProcess::readyReadStandardError, this, [this, replica_process, replica_port]() {
        appendServerLog(QString("[replica %1] %2").arg(replica_port).arg(QString::fromUtf8(replica_process->readAllStandardError())));
    });
    connect(replica_process, &QProcess::finished, this, [this, replica_process, replica_port](int exit_code, QProcess::ExitStatus status) {
        appendServerLog(QString("副本节点 %1 已退出：code=%2 status=%3")
                            .arg(replica_port)
                            .arg(exit_code)
                            .arg(status == QProcess::NormalExit ? "正常退出" : "异常退出"));
        cluster_processes_.remove(replica_port);
        replica_process->deleteLater();
        updateServerState();
    });
    appendServerLog("> " + program + " " + maskedProcessArgs(replica_args).join(' '));
    replica_process->setProcessEnvironment(processEnvironmentWithPassword(password_edit_->text()));
    replica_process->setWorkingDirectory(QDir::currentPath());
    replica_process->start(program, replica_args);

    replicas_edit_->setText(replica_node);
    replicaof_edit_->clear();
    host_edit_->setText(connect_host);
    resp_port_spin_->setValue(master_port);
    stats_port_spin_->setValue(master_stats_port);
    stats_timer_->start();
    QTimer::singleShot(800, this, [this, connect_host, master_port]() {
        resp_client_.connectToServer(connect_host, static_cast<quint16>(master_port));
        sendCommand({"INFO", "replication"});
    });
    appendServerLog("主从演示已启动；停止主节点用“停止服务”，停止副本用“停止集群”");
}

void MonitorWindow::startClusterDemo() {
    if (!cluster_processes_.isEmpty()) {
        appendServerLog("集群演示已经在运行");
        return;
    }
    if (server_process_->state() != QProcess::NotRunning) {
        appendServerLog("启动集群演示前，请先停止单节点服务");
        return;
    }

    QString program = server_path_edit_->text().trimmed();
    if (program.isEmpty()) {
        appendServerLog("ERR 服务程序路径为空");
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
    appendServerLog("正在启动三节点集群演示：" + nodes);

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
            "--io-threads", QString::number(io_threads_spin_->value()),
            "--cache-shards", QString::number(cache_shards_spin_->value()),
            "--maxmemory", QString::number(maxmemory_spin_->value()),
            "--eviction-policy", eviction_policy_combo_->currentText(),
            "--slowlog-log-slower-than-us", QString::number(slowlog_threshold_spin_->value()),
            "--slowlog-max-len", QString::number(slowlog_max_len_spin_->value()),
            "--cluster-config-file", QString("build/qt-cluster-demo/cluster_%1.conf").arg(port),
            "--cluster-heartbeat", "1",
            "--cluster-fail-threshold", "2"
        };
        if (aof_check_->isChecked()) {
            args << "--appendonly-file"
                 << QString("build/qt-cluster-demo/appendonly_%1.aof").arg(port)
                 << "--appendfsync" << appendfsync_combo_->currentText();
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
            appendServerLog(QString("节点 %1 已启动").arg(port));
            updateServerState();
        });
        connect(process, &QProcess::finished, this, [this, process, port](int exit_code, QProcess::ExitStatus status) {
            appendServerLog(QString("节点 %1 已退出：code=%2 status=%3")
                                .arg(port)
                                .arg(exit_code)
                                .arg(status == QProcess::NormalExit ? "正常退出" : "异常退出"));
            cluster_processes_.remove(port);
            process->deleteLater();
            updateServerState();
        });
        connect(process, &QProcess::errorOccurred, this, [this, process, port](QProcess::ProcessError) {
            appendServerLog(QString("节点 %1 错误：%2").arg(port).arg(process->errorString()));
            updateServerState();
        });

        appendServerLog("> " + program + " " + maskedProcessArgs(args).join(' '));
        process->setProcessEnvironment(processEnvironmentWithPassword(password_edit_->text()));
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
        appendServerLog("集群演示未运行");
        return;
    }
    appendServerLog("正在停止集群演示...");
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
        appendServerLog("集群演示至少需要两个运行中的节点");
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
        appendServerLog("没有找到可注入故障的非当前节点");
        return;
    }

    QProcess* victim = cluster_processes_.value(victim_port, nullptr);
    if (!victim || victim->state() == QProcess::NotRunning) {
        appendServerLog(QString("节点 %1 已经停止").arg(victim_port));
        return;
    }

    appendServerLog(QString("正在注入故障：停止节点 %1").arg(victim_port));
    victim->terminate();
    if (!victim->waitForFinished(3000)) {
        victim->kill();
    }

    QTimer::singleShot(3500, this, [this]() {
        appendLog("> 节点故障后自动检查");
        sendCommand({"CLUSTER", "INFO"});
        sendCommand({"CLUSTER", "NODES"});
    });
}

void MonitorWindow::runBenchmark() {
    if (benchmark_process_->state() != QProcess::NotRunning) {
        appendBenchmarkLog("压测已经在运行");
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

    appendBenchmarkLog("> redis-benchmark " + maskedProcessArgs(args).join(' '));
    benchmark_process_->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    benchmark_process_->start("redis-benchmark", args);
}

void MonitorWindow::runBenchmarkMatrix() {
    if (benchmark_process_->state() != QProcess::NotRunning) {
        appendBenchmarkLog("压测已经在运行");
        return;
    }

    QString script = "scripts/benchmark.sh";
    if (!QFileInfo::exists(script)) {
        appendBenchmarkLog("ERR 未找到 scripts/benchmark.sh");
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("SERVER", server_path_edit_->text().trimmed().isEmpty()
                             ? "build/miniredis"
                             : server_path_edit_->text().trimmed());
    env.insert("REQUESTS", QString::number(benchmark_requests_spin_->value()));
    env.insert("CLIENTS", QString::number(benchmark_clients_spin_->value()));
    env.insert("VALUE_SIZES", QString::number(benchmark_payload_spin_->value()));
    env.insert("BENCH_MATRIX", "1");
    env.insert("IO_THREADS_LIST", QString("1 %1").arg(io_threads_spin_->value()));
    env.insert("CACHE_SHARDS_LIST", QString("1 %1").arg(cache_shards_spin_->value()));
    env.insert("SAVE_RAW", "0");

    benchmark_process_->setProcessEnvironment(env);
    benchmark_process_->setWorkingDirectory(QDir::currentPath());
    appendBenchmarkLog("> BENCH_MATRIX=1 scripts/benchmark.sh");
    benchmark_process_->start("bash", {script});
}

void MonitorWindow::stopBenchmark() {
    if (benchmark_process_->state() == QProcess::NotRunning) {
        appendBenchmarkLog("压测未运行");
        return;
    }
    appendBenchmarkLog("正在停止 redis-benchmark...");
    benchmark_process_->terminate();
    if (!benchmark_process_->waitForFinished(3000)) {
        appendBenchmarkLog("redis-benchmark 未正常停止，正在强制结束");
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

void MonitorWindow::executeConsoleScript() {
    if (!console_editor_) return;
    QTextCursor cursor = console_editor_->textCursor();
    QString script = cursor.hasSelection() ? cursor.selectedText() : console_editor_->toPlainText();
    script.replace(QChar::ParagraphSeparator, '\n');
    script.replace(QChar::LineSeparator, '\n');
    script = script.trimmed();
    const QStringList lines = script.split('\n');
    int line_no = 0;
    int sent = 0;
    for (QString line : lines) {
        ++line_no;
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("--")) continue;

        QStringList parts = QProcess::splitCommand(line);
        if (parts.isEmpty()) {
            if (console_output_) {
                console_output_->appendPlainText(QString("ERR 第 %1 行为空命令").arg(line_no));
            }
            continue;
        }
        sendCommand(parts);
        ++sent;
    }
    if (sent == 0 && console_output_) {
        console_output_->appendPlainText("ERR 没有可执行命令");
        return;
    }
    if (sent > 0 && console_history_combo_ && !script.isEmpty()) {
        for (int i = 1; i < console_history_combo_->count(); ++i) {
            if (console_history_combo_->itemData(i).toString() == script) {
                console_history_combo_->removeItem(i);
                break;
            }
        }
        QString title = script.section('\n', 0, 0);
        if (script.contains('\n')) title += " ...";
        console_history_combo_->insertItem(1, title, script);
        while (console_history_combo_->count() > 11) {
            console_history_combo_->removeItem(console_history_combo_->count() - 1);
        }
        console_history_combo_->setCurrentIndex(0);
    }
}

void MonitorWindow::insertConsoleTemplate(const QString& text) {
    if (!console_editor_) return;
    QString current = console_editor_->toPlainText();
    if (!current.isEmpty() && !current.endsWith('\n')) {
        console_editor_->appendPlainText("");
    }
    console_editor_->appendPlainText(text);
    console_editor_->setFocus();
}

void MonitorWindow::runReplicationPsyncDemo() {
    appendDemoLog("=== 复制 / PSYNC 演示 ===");
    if (server_process_->state() != QProcess::NotRunning) stopServer();
    if (!cluster_processes_.isEmpty()) stopClusterDemo();

    aof_check_->setChecked(true);
    appendfsync_combo_->setCurrentText("everysec");
    startReplicaDemo();

    const QString bind = bind_edit_->text().trimmed().isEmpty() ? "127.0.0.1" : bind_edit_->text().trimmed();
    const QString connect_host = (bind == "0.0.0.0") ? "127.0.0.1" : bind;
    const int master_port = server_resp_port_spin_->value();
    const int replica_port = master_port + 1;
    const int replica_stats_port = server_stats_port_spin_->value() + 1;
    const QString master_node = QString("%1:%2").arg(connect_host).arg(master_port);
    const QString program = server_path_edit_->text().trimmed();

    QTimer::singleShot(1800, this, [this, connect_host, master_port]() {
        appendDemoLog("步骤 1：向主节点写入数据，并查看复制 offset");
        resp_client_.connectToServer(connect_host, static_cast<quint16>(master_port));
        QTimer::singleShot(350, this, [this]() {
            sendCommand({"SET", "repl:psync:base", "v1"});
            sendCommand({"INCR", "repl:psync:counter"});
            sendCommand({"INFO", "replication"});
        });
    });

    QTimer::singleShot(3600, this, [this, replica_port]() {
        appendDemoLog(QString("步骤 2：停止副本 %1，主节点继续接收写入").arg(replica_port));
        QProcess* replica = cluster_processes_.value(replica_port, nullptr);
        if (replica && replica->state() != QProcess::NotRunning) {
            replica->terminate();
            if (!replica->waitForFinished(2500)) replica->kill();
        }
        sendCommand({"SET", "repl:psync:offline", "written-while-replica-down"});
        sendCommand({"INCRBY", "repl:psync:counter", "10"});
        sendCommand({"INFO", "replication"});
    });

    QTimer::singleShot(5600, this, [this, program, bind, connect_host, replica_port,
                                    replica_stats_port, master_node]() {
        appendDemoLog("步骤 3：重启副本，优先使用保存的 offset 进行 REPLPSYNC");
        if (program.isEmpty()) {
            appendDemoLog("ERR 服务程序路径为空");
            return;
        }
        QStringList args{
            "--bind", bind,
            "--port", QString::number(replica_port),
            "--stats-bind", bind,
            "--stats-port", QString::number(replica_stats_port),
            "--snapshot-file", QString("build/qt-replica-demo/snapshot_replica_%1.dat").arg(replica_port),
            "--snapshot-interval", QString::number(snapshot_interval_spin_->value()),
            "--max-clients", QString::number(max_clients_spin_->value()),
            "--io-threads", QString::number(io_threads_spin_->value()),
            "--cache-shards", QString::number(cache_shards_spin_->value()),
            "--maxmemory", QString::number(maxmemory_spin_->value()),
            "--eviction-policy", eviction_policy_combo_->currentText(),
            "--slowlog-log-slower-than-us", QString::number(slowlog_threshold_spin_->value()),
            "--slowlog-max-len", QString::number(slowlog_max_len_spin_->value()),
            "--replicaof", master_node
        };
        if (aof_check_->isChecked()) {
            args << "--appendonly-file"
                 << QString("build/qt-replica-demo/appendonly_replica_%1.aof").arg(replica_port)
                 << "--appendfsync" << appendfsync_combo_->currentText();
        }
        QProcess* replica_process = new QProcess(this);
        cluster_processes_[replica_port] = replica_process;
        connect(replica_process, &QProcess::readyReadStandardOutput, this,
                [this, replica_process, replica_port]() {
                    appendServerLog(QString("[replica %1] %2")
                                        .arg(replica_port)
                                        .arg(QString::fromUtf8(replica_process->readAllStandardOutput())));
                });
        connect(replica_process, &QProcess::readyReadStandardError, this,
                [this, replica_process, replica_port]() {
                    appendServerLog(QString("[replica %1] %2")
                                        .arg(replica_port)
                                        .arg(QString::fromUtf8(replica_process->readAllStandardError())));
                });
        connect(replica_process, &QProcess::finished, this,
                [this, replica_process, replica_port](int, QProcess::ExitStatus) {
                    cluster_processes_.remove(replica_port);
                    replica_process->deleteLater();
                    updateServerState();
                });
        appendServerLog("> " + program + " " + maskedProcessArgs(args).join(' '));
        replica_process->setProcessEnvironment(processEnvironmentWithPassword(password_edit_->text()));
        replica_process->setWorkingDirectory(QDir::currentPath());
        replica_process->start(program, args);
        updateServerState();
    });

    QTimer::singleShot(8400, this, [this, connect_host, replica_port, replica_stats_port]() {
        appendDemoLog("步骤 4：连接副本，验证离线期间的写入已同步");
        host_edit_->setText(connect_host);
        resp_port_spin_->setValue(replica_port);
        stats_port_spin_->setValue(replica_stats_port);
        resp_client_.connectToServer(connect_host, static_cast<quint16>(replica_port));
        QTimer::singleShot(450, this, [this]() {
            sendCommand({"GET", "repl:psync:offline"});
            sendCommand({"GET", "repl:psync:counter"});
            sendCommand({"INFO", "replication"});
        });
    });
}

void MonitorWindow::runAofRecoveryDemo() {
    appendDemoLog("=== AOF 恢复演示 ===");
    if (!cluster_processes_.isEmpty()) stopClusterDemo();
    if (server_process_->state() != QProcess::NotRunning) stopServer();

    aof_check_->setChecked(true);
    appendfsync_combo_->setCurrentText("everysec");
    snapshot_file_edit_->setText("build/qt-aof-demo/snapshot.dat");
    appendonly_file_edit_->setText("build/qt-aof-demo/appendonly.aof");
    QDir().mkpath("build/qt-aof-demo");
    startServer();

    QTimer::singleShot(1500, this, [this]() {
        appendDemoLog("步骤 1：写入数据，执行重写，并查看持久化状态");
        sendCommand({"SET", "aof:plain", "value-from-qt"});
        sendCommand({"SET", "aof:ttl", "live", "EX", "120"});
        sendCommand({"APPEND", "aof:plain", ":after-append"});
        sendCommand({"BGREWRITEAOF"});
        sendCommand({"INFO", "persistence"});
    });

    QTimer::singleShot(4200, this, [this]() {
        appendDemoLog("步骤 2：重启服务，验证 AOF/snapshot 恢复");
        restartServer();
    });

    QTimer::singleShot(7600, this, [this]() {
        appendDemoLog("步骤 3：查询恢复后的 key 和重写状态");
        sendCommand({"GET", "aof:plain"});
        sendCommand({"TTL", "aof:ttl"});
        sendCommand({"INFO", "persistence"});
    });
}

void MonitorWindow::runClusterFailoverDemo() {
    appendDemoLog("=== 集群故障观察演示 ===");
    if (server_process_->state() != QProcess::NotRunning) stopServer();
    if (!cluster_processes_.isEmpty()) stopClusterDemo();

    startClusterDemo();
    QTimer::singleShot(1800, this, [this]() {
        appendDemoLog("步骤 1：查看初始集群拓扑");
        sendCommand({"CLUSTER", "INFO"});
        sendCommand({"CLUSTER", "NODES"});
        sendCommand({"CLUSTER", "SLOTS"});
    });
    QTimer::singleShot(3800, this, [this]() {
        appendDemoLog("步骤 2：停止一个非当前节点，触发 pfail/fail");
        failClusterDemoNode();
    });
    QTimer::singleShot(8200, this, [this]() {
        appendDemoLog("步骤 3：查看心跳故障检测后的集群状态");
        sendCommand({"CLUSTER", "INFO"});
        sendCommand({"CLUSTER", "NODES"});
        sendCommand({"INFO", "cluster"});
    });
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
        appendLog("ERR MOVED 自动跟随达到跳转上限");
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

    appendLog(QString("正在跟随 MOVED 到 %1:%2").arg(target_host).arg(port));
    host_edit_->setText(target_host);
    resp_port_spin_->setValue(port);
    resp_client_.connectToServer(target_host, static_cast<quint16>(port));
    return true;
}

void MonitorWindow::appendLog(const QString& text) {
    QString line = text.trimmed();
    if (line == "Connection refused") {
        line = "连接失败：目标服务未启动或端口不可用";
    } else if (line == "RESP connection is not open") {
        line = "RESP 连接未打开，请先连接服务";
    }

    const bool is_command_log = line.startsWith(">");
    if (!is_command_log && line == last_log_text_) {
        return;
    }
    last_log_text_ = is_command_log ? QString() : line;

    if (console_output_) console_output_->appendPlainText(line);
    if (demo_output_) demo_output_->appendPlainText(line);
    if (cluster_output_ && pages_->currentIndex() == 3) cluster_output_->appendPlainText(line);
    if (diagnostics_output_ && pages_->currentIndex() == 5) diagnostics_output_->appendPlainText(line);
    if (!maybeFollowMoved(line) && !line.startsWith(">") && !line.startsWith("正在跟随 MOVED")) {
        moved_follow_hops_ = 0;
    }
}

void MonitorWindow::appendDemoLog(const QString& text) {
    if (!demo_output_) return;
    QString line = text;
    if (line.endsWith('\n')) line.chop(1);
    if (!line.isEmpty()) demo_output_->appendPlainText(line);
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
    status_label_->setText(connected ? "已连接" : "未连接");
    status_label_->setStyleSheet(connected
        ? "background:#dcfce7;color:#166534;border-radius:6px;padding:5px 10px;font-weight:700;"
        : "background:#fee2e2;color:#991b1b;border-radius:6px;padding:5px 10px;font-weight:700;");
}

void MonitorWindow::updateServerState() {
    if (!server_status_label_) return;
    if (!cluster_processes_.isEmpty()) {
        server_status_label_->setText(QString("集群运行中（%1 个节点）").arg(cluster_processes_.size()));
        server_status_label_->setStyleSheet("background:#dbeafe;color:#1d4ed8;border-radius:6px;padding:5px 10px;font-weight:700;");
        return;
    }
    bool running = server_process_->state() != QProcess::NotRunning;
    server_status_label_->setText(running ? "运行中" : "已停止");
    server_status_label_->setStyleSheet(running
        ? "background:#dcfce7;color:#166534;border-radius:6px;padding:5px 10px;font-weight:700;"
        : "background:#eef2f7;color:#475569;border-radius:6px;padding:5px 10px;font-weight:700;");
}

void MonitorWindow::updateBenchmarkState() {
    if (!benchmark_status_label_) return;
    bool running = benchmark_process_->state() != QProcess::NotRunning;
    benchmark_status_label_->setText(running ? "运行中" : "空闲");
    benchmark_status_label_->setStyleSheet(running
        ? "background:#dbeafe;color:#1d4ed8;border-radius:6px;padding:5px 10px;font-weight:700;"
        : "background:#eef2f7;color:#475569;border-radius:6px;padding:5px 10px;font-weight:700;");
}

void MonitorWindow::updateStatsView(const QJsonObject& stats) {
    for (auto it = stat_labels_.begin(); it != stat_labels_.end(); ++it) {
        it.value()->setText(formatStatValue(it.key(), stats.value(it.key())));
    }
    for (auto it = console_stat_labels_.begin(); it != console_stat_labels_.end(); ++it) {
        it.value()->setText(formatStatValue(it.key(), stats.value(it.key())));
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
