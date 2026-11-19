#include "gui/main_window.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/api_client.h"
#include "gui/dialogs.h"
#include "gui/task_dialog.h"

namespace {

QString StateText(const QString& state) {
    if (state == "running") return "运行中";
    if (state == "starting") return "启动中";
    if (state == "stopping") return "停止中";
    if (state == "error") return "错误";
    return "已停止";
}

QColor StateColor(const QString& state) {
    if (state == "running") return QColor("#34d399");
    if (state == "starting" || state == "stopping") return QColor("#fbbf24");
    if (state == "error") return QColor("#f87171");
    return QColor("#9aa8bf");
}

QFrame* MakeCard(const QString& title, QLabel** value, QWidget* parent) {
    auto* card = new QFrame(parent);
    card->setObjectName("card");
    auto* caption = new QLabel(title, card);
    caption->setObjectName("cardTitle");
    *value = new QLabel("—", card);
    (*value)->setObjectName("cardValue");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->addWidget(caption);
    layout->addWidget(*value);
    return card;
}

QString WithScheme(QString url) {
    url = url.trimmed();
    if (url.isEmpty()) url = "http://127.0.0.1:8787";
    if (!url.startsWith("http://", Qt::CaseInsensitive) &&
        !url.startsWith("https://", Qt::CaseInsensitive)) {
        url.prepend("http://");
    }
    return url;
}

bool IsLocalServerUrl(const QString& url) {
    const QString host = QUrl(WithScheme(url)).host().toLower();
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

QString FindVlcExecutable() {
    QString path = QStandardPaths::findExecutable("vlc");
    if (!path.isEmpty()) return path;
    const QStringList candidates = {
        qEnvironmentVariable("ProgramFiles") + "/VideoLAN/VLC/vlc.exe",
        qEnvironmentVariable("ProgramFiles(x86)") + "/VideoLAN/VLC/vlc.exe",
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    api_ = new ApiClient(this);
    buildUi();

    QSettings settings(QSettings::IniFormat, QSettings::UserScope, "McastPilot",
                       "mcastgui");
    server_url_->setText(
        settings.value("server_url", "http://127.0.0.1:8787").toString());
    token_->setText(settings.value("token").toString());
    connect_button_->setText("连接");

    poll_timer_ = new QTimer(this);
    poll_timer_->setInterval(1000);
    connect(poll_timer_, &QTimer::timeout, this, [this]() {
        if (!connected_) return;
        refreshTasks();
        if (++poll_tick_ % 5 == 0) refreshHealth();
    });

    setWindowTitle("McastPilot 控制台");
    resize(1180, 720);
    QTimer::singleShot(0, this, &MainWindow::connectServer);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(12);

    auto* connection = new QFrame(central);
    connection->setObjectName("card");
    auto* connection_layout = new QHBoxLayout(connection);
    connection_layout->setContentsMargins(14, 10, 14, 10);
    auto* server_caption = new QLabel("服务端", connection);
    server_url_ = new QLineEdit(connection);
    server_url_->setPlaceholderText("http://127.0.0.1:8787");
    server_url_->setMinimumWidth(260);
    auto* token_caption = new QLabel("Token（认证关闭可留空）", connection);
    token_ = new QLineEdit(connection);
    token_->setEchoMode(QLineEdit::Password);
    token_->setMinimumWidth(180);
    connect_button_ = new QPushButton("连接", connection);
    status_label_ = new QLabel("未连接", connection);
    status_label_->setObjectName("status");
    connection_layout->addWidget(server_caption);
    connection_layout->addWidget(server_url_, 1);
    connection_layout->addWidget(token_caption);
    connection_layout->addWidget(token_);
    connection_layout->addWidget(connect_button_);
    connection_layout->addSpacing(8);
    connection_layout->addWidget(status_label_);

    auto* cards = new QHBoxLayout();
    cards->setSpacing(10);
    cards->addWidget(MakeCard("任务总数", &tasks_card_, central));
    cards->addWidget(MakeCard("运行中", &running_card_, central));
    cards->addWidget(MakeCard("总码率", &bitrate_card_, central));
    cards->addWidget(MakeCard("认证", &auth_card_, central));

    auto* toolbar = new QHBoxLayout();
    new_button_ = new QPushButton("新建任务", central);
    edit_button_ = new QPushButton("编辑", central);
    start_button_ = new QPushButton("启动", central);
    stop_button_ = new QPushButton("停止", central);
    delete_button_ = new QPushButton("删除", central);
    logs_button_ = new QPushButton("日志", central);
    monitor_button_ = new QPushButton("接收监测", central);
    vlc_button_ = new QPushButton("VLC", central);
    refresh_button_ = new QPushButton("刷新", central);
    toolbar->addWidget(new_button_);
    toolbar->addWidget(edit_button_);
    toolbar->addWidget(start_button_);
    toolbar->addWidget(stop_button_);
    toolbar->addWidget(delete_button_);
    toolbar->addWidget(logs_button_);
    toolbar->addWidget(monitor_button_);
    toolbar->addWidget(vlc_button_);
    toolbar->addStretch(1);
    toolbar->addWidget(refresh_button_);

    table_ = new QTableWidget(central);
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels(
        {"任务", "状态", "源", "目的", "码率", "缓冲", "发送 / 丢弃"});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QTableWidget::SelectRows);
    table_->setSelectionMode(QTableWidget::SingleSelection);
    table_->setEditTriggers(QTableWidget::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setWordWrap(false);

    root->addWidget(connection);
    root->addLayout(cards);
    root->addLayout(toolbar);
    root->addWidget(table_, 1);
    setCentralWidget(central);

    auth_card_->setText("未连接");

    connect(connect_button_, &QPushButton::clicked, this, &MainWindow::manualConnect);
    connect(new_button_, &QPushButton::clicked, this, &MainWindow::newTask);
    connect(edit_button_, &QPushButton::clicked, this, &MainWindow::editTask);
    connect(start_button_, &QPushButton::clicked, this, &MainWindow::startTask);
    connect(stop_button_, &QPushButton::clicked, this, &MainWindow::stopTask);
    connect(delete_button_, &QPushButton::clicked, this, &MainWindow::deleteTask);
    connect(logs_button_, &QPushButton::clicked, this, &MainWindow::openLogs);
    connect(monitor_button_, &QPushButton::clicked, this, &MainWindow::openMonitor);
    connect(vlc_button_, &QPushButton::clicked, this, &MainWindow::openVlc);
    connect(refresh_button_, &QPushButton::clicked, this, [this]() {
        refreshNics();
        refreshTasks();
        refreshHealth();
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this,
            &MainWindow::updateSelection);
    connect(table_, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { editTask(); });

    updateSelection();
}

void MainWindow::setStatus(const QString& text, bool error) {
    status_label_->setText(text);
    status_label_->setStyleSheet(error ? "color:#f87171;" : "color:#8d9bb3;");
}

void MainWindow::showError(const QString& title, const QString& error) {
    QMessageBox::warning(this, title, error);
}

void MainWindow::manualConnect() {
    local_start_attempted_ = false;
    connectServer();
}

void MainWindow::connectServer() {
    api_->setBaseUrl(server_url_->text());
    api_->setToken(token_->text().trimmed());
    connected_ = false;
    auth_hint_shown_ = false;
    setStatus("连接中...");
    connect_button_->setEnabled(false);

    api_->get("/api/v1/health", [this](bool ok, const QJsonObject& data,
                                       const QString& error) {
        connect_button_->setEnabled(true);
        if (!ok) {
            if (!local_start_attempted_ && IsLocalServerUrl(server_url_->text())) {
                local_start_attempted_ = true;
                if (startLocalDaemon()) {
                    setStatus("未检测到本机服务，正在启动 mcastd...");
                    QTimer::singleShot(1800, this, &MainWindow::connectServer);
                    return;
                }
            }
            setStatus("连接失败：" + error, true);
            return;
        }
        const bool auto_started = local_start_attempted_;
        local_start_attempted_ = false;
        connected_ = true;
        auth_card_->setText(data.value("auth").toBool() ? "认证 开启" : "认证 关闭");
        setStatus("已连接 · v" + data.value("version").toString() +
                  (auto_started ? " · 本机服务已自动启动（0.0.0.0）" : ""));
        connect_button_->setText("重新连接");
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "McastPilot",
                           "mcastgui");
        settings.setValue("server_url", server_url_->text().trimmed());
        settings.setValue("token", token_->text());
        refreshNics();
        refreshTasks();
        if (!poll_timer_->isActive()) poll_timer_->start();
    });
}

bool MainWindow::startLocalDaemon() {
    const QUrl url(WithScheme(server_url_->text()));
    const int port = url.port(8787);
    const QDir app_dir(QCoreApplication::applicationDirPath());
    const QDir root_dir(app_dir.absoluteFilePath("../.."));
    const QString script = root_dir.absoluteFilePath("scripts/mcastd.ps1");

    QStringList daemon_args{
        "--listen", "0.0.0.0",
        "--port", QString::number(port),
    };
    if (QFileInfo::exists(script)) {
        QStringList args{
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden",
            "-File", QDir::toNativeSeparators(script),
        };
        args << daemon_args;
        return QProcess::startDetached("powershell.exe", args, root_dir.absolutePath());
    }

    const QString executable = app_dir.absoluteFilePath("mcastd.exe");
    if (QFileInfo::exists(executable)) {
        return QProcess::startDetached(executable, daemon_args, app_dir.absolutePath());
    }
    return false;
}

void MainWindow::refreshHealth() {
    api_->get("/api/v1/health", [this](bool ok, const QJsonObject& data,
                                       const QString& error) {
        if (!ok) {
            setStatus("连接断开：" + error, true);
            return;
        }
        auth_card_->setText(data.value("auth").toBool() ? "认证 开启" : "认证 关闭");
        if (connected_) setStatus("已连接 · v" + data.value("version").toString());
    });
}

void MainWindow::refreshNics() {
    api_->get("/api/v1/nics", [this](bool ok, const QJsonObject& data,
                                     const QString& error) {
        if (!ok) {
            setStatus("读取网卡失败：" + error, true);
            return;
        }
        nics_ = data.value("nics").toArray();
    });
}

void MainWindow::refreshTasks() {
    api_->get("/api/v1/tasks", [this](bool ok, const QJsonObject& data,
                                      const QString& error) {
        if (!ok) {
            if (error.contains("token") || error.contains("401")) {
                setStatus("需要访问 token", true);
                if (!auth_hint_shown_) {
                    auth_hint_shown_ = true;
                    token_->setFocus();
                    token_->selectAll();
                    showError("需要访问 token",
                              "服务端启用了认证，请填写 token 后点“连接”。");
                }
            } else {
                setStatus("连接失败：" + error, true);
            }
            return;
        }
        auth_hint_shown_ = false;
        tasks_ = data.value("tasks").toArray();
        updateTable();
        if (connected_) setStatus("已连接");
    });
}

void MainWindow::updateTable() {
    const QString selected = selectedTaskId();
    task_map_.clear();
    table_->setRowCount(static_cast<int>(tasks_.size()));
    int row = 0;
    for (const auto& value : tasks_) {
        const QJsonObject task = value.toObject();
        const QString id = task.value("id").toString();
        const QString state = task.value("state").toString();
        const QJsonObject source = task.value("source").toObject();
        const QJsonObject output = task.value("output").toObject();
        const QJsonObject stats = task.value("stats").toObject();
        const QString uri = source.value("uri").toString();
        const QString source_label =
            uri.contains("://") ? uri : QFileInfo(uri).fileName();
        const QString destination = QString("%1:%2%3")
                                        .arg(output.value("group").toString())
                                        .arg(output.value("port").toInt())
                                        .arg(output.value("extra").toArray().isEmpty()
                                                 ? QString()
                                                 : QString(" +%1").arg(
                                                       output.value("extra").toArray().size()));
        const bool running = state == "running";

        auto* name_item = new QTableWidgetItem(task.value("name").toString());
        name_item->setData(Qt::UserRole, id);
        name_item->setToolTip(id);
        auto* state_item = new QTableWidgetItem(StateText(state));
        state_item->setForeground(StateColor(state));
        auto* source_item = new QTableWidgetItem(source_label);
        source_item->setToolTip(uri);
        auto* dest_item = new QTableWidgetItem(destination);
        const QString nic = output.value("nic").toString();
        dest_item->setToolTip(nic.isEmpty() ? "系统默认路由" : nic);
        auto* bitrate_item = new QTableWidgetItem(
            running ? QString::number(stats.value("bitrate_mbps").toDouble(), 'f', 2) +
                          " Mbps"
                    : "—");
        auto* buffer_item = new QTableWidgetItem(
            running ? QString::number(stats.value("buffer_fill").toDouble(), 'f', 0) + "%"
                    : "—");
        auto* send_item = new QTableWidgetItem(
            QString("%1 / %2")
                .arg(static_cast<qulonglong>(stats.value("packets_sent").toDouble()))
                .arg(static_cast<qulonglong>(stats.value("packets_dropped").toDouble())));
        send_item->setToolTip(
            QString("缓冲丢包 %1 KB · 发送错误 %2 · 录制 %3 MB")
                .arg(stats.value("buffer_drop_bytes").toDouble() / 1024, 0, 'f', 0)
                .arg(stats.value("send_errors").toDouble(), 0, 'f', 0)
                .arg(stats.value("recorded_bytes").toDouble() / 1024 / 1024, 0, 'f', 2));

        table_->setItem(row, 0, name_item);
        table_->setItem(row, 1, state_item);
        table_->setItem(row, 2, source_item);
        table_->setItem(row, 3, dest_item);
        table_->setItem(row, 4, bitrate_item);
        table_->setItem(row, 5, buffer_item);
        table_->setItem(row, 6, send_item);
        task_map_[id] = task;
        ++row;
    }

    int restore = -1;
    if (!selected.isEmpty()) {
        for (int i = 0; i < table_->rowCount(); ++i) {
            if (table_->item(i, 0) && table_->item(i, 0)->data(Qt::UserRole).toString() ==
                                          selected) {
                restore = i;
                break;
            }
        }
    }
    if (restore < 0 && table_->rowCount() > 0) restore = 0;
    if (restore >= 0) table_->selectRow(restore);

    updateCards();
    updateSelection();
}

QString MainWindow::selectedTaskId() const {
    const int row = table_ ? table_->currentRow() : -1;
    if (row < 0) return {};
    auto* item = table_->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QJsonObject MainWindow::selectedTask() const {
    return task_map_.value(selectedTaskId());
}

void MainWindow::updateSelection() {
    const QJsonObject task = selectedTask();
    const bool has = !task.isEmpty();
    const QString state = task.value("state").toString();
    const bool can_start = has && (state == "idle" || state == "error");
    const bool can_stop = has && (state == "running" || state == "starting");
    const bool can_edit = can_start;

    new_button_->setEnabled(connected_);
    refresh_button_->setEnabled(connected_);
    monitor_button_->setEnabled(connected_);
    start_button_->setEnabled(can_start);
    stop_button_->setEnabled(can_stop);
    edit_button_->setEnabled(can_edit);
    delete_button_->setEnabled(can_edit);
    logs_button_->setEnabled(has);
    vlc_button_->setEnabled(has && state == "running");
}

void MainWindow::updateCards() {
    int running = 0;
    double bitrate = 0.0;
    for (const auto& value : tasks_) {
        const QJsonObject task = value.toObject();
        if (task.value("state").toString() == "running") {
            ++running;
            bitrate += task.value("stats").toObject().value("bitrate_mbps").toDouble();
        }
    }
    tasks_card_->setText(QString::number(tasks_.size()));
    running_card_->setText(QString::number(running));
    bitrate_card_->setText(QString::number(bitrate, 'f', 2) + " Mbps");
}

void MainWindow::newTask() {
    if (!connected_) {
        showError("新建任务", "请先连接服务端。");
        return;
    }
    TaskDialog dialog(api_, nics_, this);
    if (dialog.exec() != QDialog::Accepted) return;
    api_->post("/api/v1/tasks", dialog.body(),
               [this](bool ok, const QJsonObject&, const QString& error) {
                   if (!ok) {
                       showError("新建任务失败", error);
                       return;
                   }
                   refreshTasks();
               });
}

void MainWindow::editTask() {
    const QJsonObject task = selectedTask();
    if (task.isEmpty()) return;
    const QString state = task.value("state").toString();
    if (state != "idle" && state != "error") {
        showError("编辑任务", "任务运行中，请先停止再编辑。");
        return;
    }
    TaskDialog dialog(api_, nics_, this);
    dialog.setTask(task);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString id = task.value("id").toString();
    api_->patch("/api/v1/tasks/" + id, dialog.body(),
                [this](bool ok, const QJsonObject&, const QString& error) {
                    if (!ok) {
                        showError("保存任务失败", error);
                        return;
                    }
                    refreshTasks();
                });
}

void MainWindow::startTask() {
    const QString id = selectedTaskId();
    if (id.isEmpty()) return;
    api_->post("/api/v1/tasks/" + id + "/start", QJsonObject(),
               [this](bool ok, const QJsonObject&, const QString& error) {
                   if (!ok) showError("启动失败", error);
                   refreshTasks();
               });
}

void MainWindow::stopTask() {
    const QString id = selectedTaskId();
    if (id.isEmpty()) return;
    api_->post("/api/v1/tasks/" + id + "/stop", QJsonObject(),
               [this](bool ok, const QJsonObject&, const QString& error) {
                   if (!ok) showError("停止失败", error);
                   refreshTasks();
               });
}

void MainWindow::deleteTask() {
    const QJsonObject task = selectedTask();
    if (task.isEmpty()) return;
    const QString id = task.value("id").toString();
    const QString name = task.value("name").toString();
    if (QMessageBox::question(this, "删除任务",
                              QString("删除任务“%1”？").arg(name)) !=
        QMessageBox::Yes) {
        return;
    }
    api_->del("/api/v1/tasks/" + id,
              [this](bool ok, const QJsonObject&, const QString& error) {
                  if (!ok) showError("删除失败", error);
                  refreshTasks();
              });
}

void MainWindow::openLogs() {
    const QJsonObject task = selectedTask();
    if (task.isEmpty()) return;
    auto* dialog = new LogsDialog(api_, task.value("id").toString(),
                                  task.value("name").toString(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openMonitor() {
    auto* dialog = new MonitorDialog(api_, nics_, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::openVlc() {
    const QJsonObject task = selectedTask();
    if (task.isEmpty()) return;
    if (task.value("state").toString() != "running") {
        showError("VLC", "任务未运行。");
        return;
    }
    const QJsonObject output = task.value("output").toObject();
    const QString group = output.value("group").toString();
    const int port = output.value("port").toInt();
    if (group.isEmpty() || port <= 0) {
        showError("VLC", "任务没有有效的 UDP 目的地址。");
        return;
    }
    const QString vlc = FindVlcExecutable();
    if (vlc.isEmpty()) {
        showError("VLC", "没有找到 vlc.exe，请安装 VLC 或把 VLC 加入 PATH。");
        return;
    }
    const QString url = QString("udp://@%1:%2").arg(group).arg(port);
    if (!QProcess::startDetached(vlc, {url})) {
        showError("VLC", "启动 VLC 失败。");
        return;
    }
    setStatus("已在桌面打开 VLC：" + url);
}
