#include "gui/dialogs.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/api_client.h"

namespace {

QString FormatBytes(double value) {
    if (value < 1024) return QString::number(value, 'f', 0) + " B";
    if (value < 1024 * 1024) return QString::number(value / 1024, 'f', 1) + " KB";
    if (value < 1024.0 * 1024 * 1024) return QString::number(value / 1024 / 1024, 'f', 2) + " MB";
    return QString::number(value / 1024 / 1024 / 1024, 'f', 2) + " GB";
}

QString FormatDuration(double seconds) {
    const int total = static_cast<int>(seconds);
    if (total < 60) return QString::number(total) + "s";
    const int minutes = total / 60;
    const int rest = total % 60;
    if (minutes < 60) return QString("%1m %2s").arg(minutes).arg(rest);
    return QString("%1h %2m").arg(minutes / 60).arg(minutes % 60);
}

}  // namespace

MediaBrowserDialog::MediaBrowserDialog(ApiClient* api, QWidget* parent)
    : QDialog(parent), api_(api) {
    setWindowTitle("服务器媒体库");
    resize(680, 480);

    path_label_ = new QLabel("媒体库根目录", this);
    path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_label_->setStyleSheet("color:#8d9bb3;");
    up_button_ = new QPushButton("上一级", this);
    select_button_ = new QPushButton("选择文件", this);
    select_button_->setEnabled(false);
    list_ = new QListWidget(this);
    list_->setAlternatingRowColors(true);

    auto* top = new QHBoxLayout();
    top->addWidget(up_button_);
    top->addWidget(path_label_, 1);

    auto* bottom = new QHBoxLayout();
    auto* close_button = new QPushButton("取消", this);
    bottom->addStretch(1);
    bottom->addWidget(close_button);
    bottom->addWidget(select_button_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(list_, 1);
    layout->addLayout(bottom);

    connect(up_button_, &QPushButton::clicked, this, [this]() {
        if (!parent_path_.isEmpty()) load(parent_path_);
    });
    connect(select_button_, &QPushButton::clicked, this,
            &MediaBrowserDialog::chooseCurrent);
    connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { chooseCurrent(); });
    connect(list_, &QListWidget::itemSelectionChanged, this, [this]() {
        auto* item = list_->currentItem();
        select_button_->setEnabled(item && item->data(Qt::UserRole + 1).toString() == "file");
    });

    load(QString());
}

void MediaBrowserDialog::load(const QString& path) {
    QString suffix;
    if (!path.isEmpty()) {
        suffix = "?path=" + QString::fromLatin1(QUrl::toPercentEncoding(path));
    }
    QPointer<MediaBrowserDialog> guard(this);
    api_->get("/api/v1/media/list" + suffix,
              [guard](bool ok, const QJsonObject& data, const QString& error) {
                  if (!guard) return;
                  if (!ok) {
                      guard->path_label_->setText("加载失败：" + error);
                      return;
                  }
                  guard->current_path_ = data.value("path").toString();
                  guard->parent_path_ = data.value("parent").toString();
                  guard->path_label_->setText(
                      guard->current_path_.isEmpty() ? "媒体库根目录" : guard->current_path_);
                  guard->up_button_->setEnabled(!guard->parent_path_.isEmpty());
                  guard->list_->clear();
                  const QJsonArray entries = data.value("entries").toArray();
                  for (const auto& value : entries) {
                      const QJsonObject entry = value.toObject();
                      const QString type = entry.value("type").toString();
                      const QString name = entry.value("name").toString();
                      QString text = name;
                      if (type == "file") {
                          text += "    " + FormatBytes(entry.value("size").toDouble());
                      } else {
                          text += " /";
                      }
                      auto* item = new QListWidgetItem(text, guard->list_);
                      item->setData(Qt::UserRole, entry.value("path").toString());
                      item->setData(Qt::UserRole + 1, type);
                  }
                  if (entries.isEmpty()) {
                      new QListWidgetItem("（空目录）", guard->list_);
                  }
              });
}

void MediaBrowserDialog::chooseCurrent() {
    auto* item = list_->currentItem();
    if (!item) return;
    const QString type = item->data(Qt::UserRole + 1).toString();
    const QString path = item->data(Qt::UserRole).toString();
    if (type == "dir") {
        load(path);
    } else if (type == "file") {
        selected_path_ = path;
        accept();
    }
}

LogsDialog::LogsDialog(ApiClient* api, const QString& task_id, const QString& title,
                       QWidget* parent)
    : QDialog(parent), api_(api), task_id_(task_id) {
    setWindowTitle("任务日志 - " + title);
    resize(860, 560);

    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setStyleSheet("font-family: Consolas, monospace;");
    auto* refresh_button = new QPushButton("刷新", this);
    auto* bottom_button = new QPushButton("关闭", this);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(refresh_button);
    buttons->addStretch(1);
    buttons->addWidget(bottom_button);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(text_, 1);
    layout->addLayout(buttons);

    connect(refresh_button, &QPushButton::clicked, this, &LogsDialog::refresh);
    connect(bottom_button, &QPushButton::clicked, this, &QDialog::accept);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &LogsDialog::refresh);
    timer_->start();
    refresh();
}

void LogsDialog::refresh() {
    QPointer<LogsDialog> guard(this);
    api_->get("/api/v1/tasks/" + task_id_ + "/logs?tail=400",
              [guard](bool ok, const QJsonObject& data, const QString& error) {
                  if (!guard) return;
                  if (!ok) {
                      guard->text_->setPlainText("日志加载失败：" + error);
                      return;
                  }
                  QStringList lines;
                  for (const auto& line : data.value("lines").toArray()) {
                      lines << line.toString();
                  }
                  guard->text_->setPlainText(lines.isEmpty() ? "暂无日志" : lines.join('\n'));
                  guard->text_->moveCursor(QTextCursor::End);
              });
}

MonitorDialog::MonitorDialog(ApiClient* api, const QJsonArray& nics, QWidget* parent)
    : QDialog(parent), api_(api) {
    setWindowTitle("接收监测");
    resize(640, 460);

    group_ = new QLineEdit("239.255.1.1", this);
    port_ = new QSpinBox(this);
    port_->setRange(1, 65535);
    port_->setValue(5000);
    iface_ = new QComboBox(this);
    iface_->addItem("自动 / 任意", "");
    for (const auto& value : nics) {
        const QJsonObject nic = value.toObject();
        const QJsonArray ipv4 = nic.value("ipv4").toArray();
        if (ipv4.isEmpty()) continue;
        const QString ip = ipv4.first().toObject().value("addr").toString();
        QString label = nic.value("name").toString() + " — " + ip;
        if (!nic.value("up").toBool()) label += "（未连接）";
        if (nic.value("virtual").toBool()) label += "（虚拟）";
        iface_->addItem(label, ip);
    }
    duration_ = new QSpinBox(this);
    duration_->setRange(0, 3600);
    duration_->setSuffix(" s");
    duration_->setSpecialValueText("一直运行");

    auto* form = new QFormLayout();
    form->addRow("组播 / 目的地址", group_);
    form->addRow("端口", port_);
    form->addRow("接收网卡", iface_);
    form->addRow("监测时长", duration_);

    start_button_ = new QPushButton("开始监测", this);
    stop_button_ = new QPushButton("停止监测", this);
    stop_button_->setEnabled(false);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(start_button_);
    buttons->addWidget(stop_button_);
    buttons->addStretch(1);

    auto* stats = new QGridLayout();
    auto addStat = [this, stats](int row, int column, const QString& title, QLabel*& value) {
        auto* caption = new QLabel(title, this);
        caption->setStyleSheet("color:#8d9bb3;");
        value = new QLabel("—", this);
        value->setStyleSheet("font-size:18px; font-weight:600;");
        stats->addWidget(caption, row * 2, column);
        stats->addWidget(value, row * 2 + 1, column);
    };
    addStat(0, 0, "实时码率", bitrate_);
    addStat(0, 1, "接收包数", packets_);
    addStat(0, 2, "接收字节", bytes_);
    addStat(1, 0, "运行时长", elapsed_);
    addStat(1, 1, "TS 同步错误", sync_errors_);
    addStat(1, 2, "CC 连续性错误", cc_errors_);

    status_ = new QLabel("未运行", this);
    status_->setStyleSheet("color:#8d9bb3; font-family: Consolas, monospace;");
    auto* note = new QLabel("关闭此窗口不会停止服务端监测；需要时点“停止监测”。", this);
    note->setStyleSheet("color:#8d9bb3; font-size:11px;");

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(buttons);
    layout->addLayout(stats);
    layout->addWidget(status_);
    layout->addWidget(note);
    layout->addStretch(1);

    connect(start_button_, &QPushButton::clicked, this, &MonitorDialog::startMonitor);
    connect(stop_button_, &QPushButton::clicked, this, &MonitorDialog::stopMonitor);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &MonitorDialog::refresh);
    refresh();
}

void MonitorDialog::startMonitor() {
    QJsonObject body{
        {"group", group_->text().trimmed()},
        {"port", port_->value()},
        {"iface", iface_->currentData().toString()},
        {"duration_sec", duration_->value()},
    };
    QPointer<MonitorDialog> guard(this);
    api_->post("/api/v1/monitor/start", body,
               [guard](bool ok, const QJsonObject& data, const QString& error) {
                   if (!guard) return;
                   if (!ok) {
                       guard->status_->setText("启动失败：" + error);
                       return;
                   }
                   guard->setStatus(data.value("monitor").toObject());
                   guard->setRunning(true);
               });
}

void MonitorDialog::stopMonitor() {
    QPointer<MonitorDialog> guard(this);
    api_->post("/api/v1/monitor/stop", QJsonObject(),
               [guard](bool ok, const QJsonObject& data, const QString& error) {
                   if (!guard) return;
                   if (!ok) {
                       guard->status_->setText("停止失败：" + error);
                       return;
                   }
                   guard->setStatus(data.value("monitor").toObject());
                   guard->setRunning(false);
               });
}

void MonitorDialog::refresh() {
    QPointer<MonitorDialog> guard(this);
    api_->get("/api/v1/monitor",
              [guard](bool ok, const QJsonObject& data, const QString& error) {
                  if (!guard) return;
                  if (!ok) {
                      guard->status_->setText("读取失败：" + error);
                      return;
                  }
                  const QJsonObject monitor = data.value("monitor").toObject();
                  guard->setStatus(monitor);
                  guard->setRunning(monitor.value("running").toBool());
              });
}

void MonitorDialog::setStatus(const QJsonObject& monitor) {
    const bool running = monitor.value("running").toBool();
    const bool has_data = running || monitor.value("packets").toDouble() > 0;
    bitrate_->setText(has_data ? QString::number(monitor.value("bitrate_mbps").toDouble(), 'f', 2) +
                                     " Mbps"
                               : "—");
    packets_->setText(QString::number(monitor.value("packets").toDouble(), 'f', 0));
    bytes_->setText(FormatBytes(monitor.value("bytes").toDouble()));
    elapsed_->setText(FormatDuration(monitor.value("elapsed_sec").toDouble()));
    sync_errors_->setText(QString::number(monitor.value("sync_errors").toDouble(), 'f', 0));
    cc_errors_->setText(QString::number(monitor.value("cc_errors").toDouble(), 'f', 0));
    if (running) {
        status_->setText("监测中...");
    } else if (!monitor.value("last_error").toString().isEmpty()) {
        status_->setText("已停止：" + monitor.value("last_error").toString());
    } else if (has_data) {
        status_->setText("已停止");
    } else {
        status_->setText("未运行");
    }
}

void MonitorDialog::setRunning(bool running) {
    start_button_->setEnabled(!running);
    stop_button_->setEnabled(running);
    if (running) {
        if (!timer_->isActive()) timer_->start();
    } else {
        timer_->stop();
    }
}
