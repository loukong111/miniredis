#include "monitor_window.hpp"

#include <QCheckBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

MonitorWindow::MonitorWindow(QWidget* parent) : QMainWindow(parent), stats_timer_(new QTimer(this)) {
    setWindowTitle("MiniRedis Qt Console");
    resize(1120, 760);

    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->addWidget(buildConnectionPanel());

    tabs_ = new QTabWidget(central);
    tabs_->addTab(buildKvTab(), "KV Commands");
    tabs_->addTab(buildClusterTab(), "Cluster");
    tabs_->addTab(buildStatsTab(), "Stats");
    tabs_->addTab(buildRawTab(), "Raw Output");
    root->addWidget(tabs_, 1);
    setCentralWidget(central);

    connect(&resp_client_, &RespClient::connected, this, [this]() {
        updateConnectionState(true);
        if (!password_edit_->text().isEmpty()) {
            sendCommand({"AUTH", password_edit_->text()});
        }
    });
    connect(&resp_client_, &RespClient::disconnected, this, [this]() { updateConnectionState(false); });
    connect(&resp_client_, &RespClient::responseReceived, this, &MonitorWindow::appendLog);
    connect(&resp_client_, &RespClient::errorOccurred, this, &MonitorWindow::appendLog);

    connect(&stats_client_, &StatsClient::statsReceived, this, &MonitorWindow::updateStatsView);
    connect(&stats_client_, &StatsClient::metricsReceived, metrics_output_, &QPlainTextEdit::setPlainText);
    connect(&stats_client_, &StatsClient::errorOccurred, this, &MonitorWindow::appendLog);

    connect(stats_timer_, &QTimer::timeout, this, [this]() {
        stats_client_.fetchStats(host(), statsPort());
        stats_client_.fetchMetrics(host(), statsPort());
    });
    stats_timer_->setInterval(2000);

    updateConnectionState(false);
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
    layout->addWidget(status_label_, 1, 2);
    layout->addWidget(connect_btn, 1, 3);
    layout->addWidget(disconnect_btn, 1, 4);
    layout->addWidget(ping_btn, 1, 5);

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
    QPushButton* expire_btn = new QPushButton("EXPIRE", page);
    QPushButton* ttl_btn = new QPushButton("TTL", page);

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
    layout->addWidget(expire_btn, 3, 2);
    layout->addWidget(ttl_btn, 3, 3);
    layout->addWidget(new QLabel("MGET keys", page), 4, 0);
    layout->addWidget(keys_edit, 4, 1, 1, 2);
    layout->addWidget(mget_btn, 4, 3);
    layout->addWidget(output_, 5, 0, 1, 4);
    layout->setRowStretch(5, 1);

    connect(set_btn, &QPushButton::clicked, this, [this, key_edit, value_edit, ttl_spin, set_ex_check]() {
        QStringList cmd{"SET", key_edit->text(), value_edit->text()};
        if (set_ex_check->isChecked()) cmd << "EX" << QString::number(ttl_spin->value());
        sendCommand(cmd);
    });
    connect(get_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"GET", key_edit->text()}); });
    connect(del_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"DEL", key_edit->text()}); });
    connect(expire_btn, &QPushButton::clicked, this, [this, key_edit, ttl_spin]() {
        sendCommand({"EXPIRE", key_edit->text(), QString::number(ttl_spin->value())});
    });
    connect(ttl_btn, &QPushButton::clicked, this, [this, key_edit]() { sendCommand({"TTL", key_edit->text()}); });
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
    QPushButton* info_btn = new QPushButton("CLUSTER INFO", page);
    QPushButton* nodes_btn = new QPushButton("CLUSTER NODES", page);
    QPushButton* slots_btn = new QPushButton("CLUSTER SLOTS", page);
    QPushButton* keyslot_btn = new QPushButton("KEYSLOT", page);
    cluster_output_ = new QPlainTextEdit(page);
    cluster_output_->setReadOnly(true);

    controls->addWidget(info_btn, 0, 0);
    controls->addWidget(nodes_btn, 0, 1);
    controls->addWidget(slots_btn, 0, 2);
    controls->addWidget(new QLabel("Key", page), 1, 0);
    controls->addWidget(key_edit, 1, 1);
    controls->addWidget(keyslot_btn, 1, 2);
    layout->addLayout(controls);
    layout->addWidget(cluster_output_, 1);

    connect(info_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "INFO"}); });
    connect(nodes_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "NODES"}); });
    connect(slots_btn, &QPushButton::clicked, this, [this]() { sendCommand({"CLUSTER", "SLOTS"}); });
    connect(keyslot_btn, &QPushButton::clicked, this, [this, key_edit]() {
        sendCommand({"CLUSTER", "KEYSLOT", key_edit->text()});
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

QWidget* MonitorWindow::buildRawTab() {
    QWidget* page = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(page);
    metrics_output_ = new QPlainTextEdit(page);
    metrics_output_->setReadOnly(true);
    layout->addWidget(metrics_output_);
    return page;
}

void MonitorWindow::sendCommand(const QStringList& parts) {
    if (parts.isEmpty()) return;
    QString rendered = "> " + parts.join(' ');
    appendLog(rendered);
    if (tabs_->currentIndex() == 1) cluster_output_->appendPlainText(rendered);
    resp_client_.sendCommand(parts);
}

void MonitorWindow::appendLog(const QString& text) {
    output_->appendPlainText(text);
    if (tabs_->currentIndex() == 1) cluster_output_->appendPlainText(text);
}

void MonitorWindow::updateConnectionState(bool connected) {
    status_label_->setText(connected ? "Connected" : "Disconnected");
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
