#include "gui/task_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include "gui/api_client.h"
#include "gui/dialogs.h"

TaskDialog::TaskDialog(ApiClient* api, const QJsonArray& nics, QWidget* parent)
    : QDialog(parent), api_(api), nics_(nics) {
    setWindowTitle("新建任务");
    resize(760, 720);

    name_ = new QLineEdit(this);
    input_ = new QLineEdit(this);
    auto* browse_server = new QPushButton("服务器媒体库", this);
    auto* browse_local = new QPushButton("本地文件", this);
    auto* probe = new QPushButton("探测", this);
    auto* input_row = new QHBoxLayout();
    input_row->addWidget(input_, 1);
    input_row->addWidget(browse_server);
    input_row->addWidget(browse_local);
    input_row->addWidget(probe);

    auto* input_widget = new QWidget(this);
    input_widget->setLayout(input_row);
    input_row->setContentsMargins(0, 0, 0, 0);

    loop_ = new QCheckBox("文件循环播放", this);
    loop_->setChecked(true);
    realtime_ = new QCheckBox("按实时速率读取", this);
    realtime_->setChecked(true);
    auto* flags = new QHBoxLayout();
    flags->addWidget(loop_);
    flags->addWidget(realtime_);
    flags->addStretch(1);
    auto* flags_widget = new QWidget(this);
    flags_widget->setLayout(flags);
    flags->setContentsMargins(0, 0, 0, 0);

    group_ = new QLineEdit("239.1.1.1", this);
    port_ = new QSpinBox(this);
    port_->setRange(1, 65535);
    port_->setValue(5000);
    nic_ = new QComboBox(this);
    populateNics(QString());
    ttl_ = new QSpinBox(this);
    ttl_->setRange(1, 255);
    ttl_->setValue(16);
    dscp_ = new QSpinBox(this);
    dscp_->setRange(0, 63);
    muxrate_ = new QSpinBox(this);
    muxrate_->setRange(0, 2000000000);
    muxrate_->setSingleStep(100000);
    muxrate_->setSpecialValueText("自动（输入码率×1.15）");
    packet_size_ = new QSpinBox(this);
    packet_size_->setRange(188, 65507);
    packet_size_->setSingleStep(188);
    packet_size_->setValue(1316);
    buffer_kb_ = new QSpinBox(this);
    buffer_kb_->setRange(64, 262144);
    buffer_kb_->setValue(4096);
    speed_ = new QDoubleSpinBox(this);
    speed_->setRange(0.1, 100.0);
    speed_->setDecimals(1);
    speed_->setValue(1.0);
    loopback_ = new QCheckBox("组播回环（同机接收时需要）", this);
    extras_ = new QPlainTextEdit(this);
    extras_->setPlaceholderText("额外目的地，每行 group:port，最多 4 个\n例如 239.1.1.2:5000");
    extras_->setMaximumHeight(64);
    record_ = new QCheckBox("录制 TS 到服务端数据目录", this);
    encode_mode_ = new QComboBox(this);
    encode_mode_->addItem("copy（不转码）", "copy");
    encode_mode_->addItem("H.264 + AAC 转码", "transcode");
    video_bitrate_ = new QSpinBox(this);
    video_bitrate_->setRange(100000, 50000000);
    video_bitrate_->setSingleStep(100000);
    video_bitrate_->setValue(4000000);
    audio_bitrate_ = new QSpinBox(this);
    audio_bitrate_->setRange(32000, 512000);
    audio_bitrate_->setSingleStep(1000);
    audio_bitrate_->setValue(128000);
    preset_ = new QComboBox(this);
    preset_->addItems({"ultrafast", "superfast", "veryfast", "faster", "fast",
                       "medium", "slow", "slower"});
    preset_->setCurrentText("veryfast");
    gop_ = new QSpinBox(this);
    gop_->setRange(0, 600);
    gop_->setValue(0);
    auto_restart_ = new QCheckBox("源断开后自动重连", this);
    auto_restart_->setChecked(true);
    max_retries_ = new QSpinBox(this);
    max_retries_->setRange(0, 1000);
    max_retries_->setValue(10);
    backoff_ = new QSpinBox(this);
    backoff_->setRange(100, 60000);
    backoff_->setSingleStep(100);
    backoff_->setValue(1000);
    backoff_->setSuffix(" ms");

    probe_result_ = new QLabel("尚未探测", this);
    probe_result_->setWordWrap(true);
    probe_result_->setStyleSheet(
        "color:#8d9bb3; font-family: Consolas, monospace; padding:6px; "
        "border:1px dashed #243149; border-radius:6px;");

    auto* form = new QGridLayout();
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    int row = 0;
    auto addRow = [form, &row](int column, const QString& label, QWidget* widget,
                               int span = 1) {
        auto* caption = new QLabel(label);
        caption->setStyleSheet("color:#8d9bb3;");
        form->addWidget(caption, row, column);
        form->addWidget(widget, row, column + 1, 1, span);
        if (column == 2) ++row;
    };
    form->addWidget(new QLabel("任务名称"), row, 0);
    form->addWidget(name_, row, 1, 1, 3);
    ++row;
    form->addWidget(new QLabel("输入（本地路径或网络 URL）"), row, 0);
    form->addWidget(input_widget, row, 1, 1, 3);
    ++row;
    form->addWidget(flags_widget, row, 1, 1, 3);
    ++row;
    form->addWidget(probe_result_, row, 1, 1, 3);
    ++row;
    addRow(0, "组播 / 目的地址", group_, 1);
    addRow(2, "端口", port_, 1);
    addRow(0, "发送网卡", nic_, 1);
    addRow(2, "TTL", ttl_, 1);
    addRow(0, "DSCP", dscp_, 1);
    addRow(2, "CBR 码率 bps", muxrate_, 1);
    addRow(0, "UDP 包大小", packet_size_, 1);
    addRow(2, "环形缓冲 KB", buffer_kb_, 1);
    addRow(0, "倍速", speed_, 1);
    addRow(2, "", loopback_, 3);
    ++row;
    form->addWidget(new QLabel("额外目的地"), row, 0);
    form->addWidget(extras_, row, 1, 1, 3);
    ++row;
    form->addWidget(record_, row, 1, 1, 3);
    ++row;
    form->addWidget(new QLabel("编码模式"), row, 0);
    form->addWidget(encode_mode_, row, 1, 1, 3);
    ++row;
    addRow(0, "视频码率 bps", video_bitrate_, 1);
    addRow(2, "音频码率 bps", audio_bitrate_, 1);
    addRow(0, "x264 preset", preset_, 1);
    addRow(2, "GOP（0=自动）", gop_, 1);
    form->addWidget(new QLabel("断流重连"), row, 0);
    form->addWidget(auto_restart_, row, 1);
    ++row;
    addRow(0, "最大重试次数", max_retries_, 1);
    addRow(2, "初始退避", backoff_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok,
                                         this);
    buttons->button(QDialogButtonBox::Ok)->setText("保存");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addStretch(1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &TaskDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browse_server, &QPushButton::clicked, this, &TaskDialog::browseServerMedia);
    connect(browse_local, &QPushButton::clicked, this, &TaskDialog::browseLocalFile);
    connect(probe, &QPushButton::clicked, this, &TaskDialog::probeInput);
    connect(encode_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { syncEncodeFields(); });
    syncEncodeFields();
}

void TaskDialog::populateNics(const QString& selected) {
    nic_->clear();
    nic_->addItem("系统默认路由", "");
    for (const auto& value : nics_) {
        const QJsonObject nic = value.toObject();
        const QJsonArray ipv4 = nic.value("ipv4").toArray();
        const QString ip = ipv4.isEmpty() ? QString()
                                          : ipv4.first().toObject().value("addr").toString();
        const QString key = ip.isEmpty() ? nic.value("id").toString() : ip;
        QString label = nic.value("name").toString();
        if (!ip.isEmpty()) label += " — " + ip;
        if (!nic.value("up").toBool()) label += "（未连接）";
        if (nic.value("virtual").toBool()) label += "（虚拟）";
        nic_->addItem(label, key);
    }
    if (!selected.isEmpty()) {
        int index = nic_->findData(selected);
        if (index < 0) {
            nic_->addItem(selected, selected);
            index = nic_->count() - 1;
        }
        nic_->setCurrentIndex(index);
    }
}

void TaskDialog::setTask(const QJsonObject& task) {
    setWindowTitle("编辑任务");
    name_->setText(task.value("name").toString());
    const QJsonObject source = task.value("source").toObject();
    input_->setText(source.value("uri").toString());
    loop_->setChecked(source.value("loop").toBool(true));
    realtime_->setChecked(task.value("realtime").toBool(true));
    speed_->setValue(task.value("speed").toDouble(1.0));
    const QJsonObject output = task.value("output").toObject();
    group_->setText(output.value("group").toString());
    port_->setValue(output.value("port").toInt(5000));
    ttl_->setValue(output.value("ttl").toInt(16));
    dscp_->setValue(output.value("dscp").toInt(0));
    packet_size_->setValue(output.value("pkt_size").toInt(1316));
    loopback_->setChecked(output.value("loopback").toBool());
    QStringList extra_lines;
    for (const auto& value : output.value("extra").toArray()) {
        const QJsonObject extra = value.toObject();
        extra_lines << QString("%1:%2")
                           .arg(extra.value("group").toString())
                           .arg(extra.value("port").toInt());
    }
    extras_->setPlainText(extra_lines.join('\n'));
    record_->setChecked(task.value("record").toBool());
    const QJsonObject encode = task.value("encode").toObject();
    const int mode_index = encode_mode_->findData(encode.value("mode").toString("copy"));
    if (mode_index >= 0) encode_mode_->setCurrentIndex(mode_index);
    video_bitrate_->setValue(encode.value("video_bitrate").toInt(4000000));
    audio_bitrate_->setValue(encode.value("audio_bitrate").toInt(128000));
    preset_->setCurrentText(encode.value("preset").toString("veryfast"));
    gop_->setValue(encode.value("gop").toInt(0));
    syncEncodeFields();
    const QString nic = output.value("nic").toString();
    if (!nic.isEmpty()) {
        const int index = nic_->findData(nic);
        if (index >= 0) {
            nic_->setCurrentIndex(index);
        } else {
            nic_->addItem(nic, nic);
            nic_->setCurrentIndex(nic_->count() - 1);
        }
    }
    const QJsonObject mux = task.value("mux").toObject();
    muxrate_->setValue(mux.value("muxrate").toInt(0));
    buffer_kb_->setValue(task.value("buffer_kb").toInt(4096));
    const QJsonObject restart = task.value("restart").toObject();
    auto_restart_->setChecked(restart.value("auto").toBool(true));
    max_retries_->setValue(restart.value("max_retries").toInt(10));
    backoff_->setValue(restart.value("backoff_ms").toInt(1000));
}

void TaskDialog::browseServerMedia() {
    MediaBrowserDialog dialog(api_, this);
    if (dialog.exec() == QDialog::Accepted && !dialog.selectedPath().isEmpty()) {
        input_->setText(dialog.selectedPath());
        if (name_->text().trimmed().isEmpty()) {
            name_->setText(QFileInfo(dialog.selectedPath()).fileName());
        }
    }
}

void TaskDialog::browseLocalFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, "选择媒体文件", QString(),
        "媒体文件 (*.ts *.m2ts *.mts *.trp *.mp4 *.m4v *.mkv *.mov *.flv *.avi *.mp3 *.aac);;所有文件 (*)");
    if (path.isEmpty()) return;
    input_->setText(path);
    if (name_->text().trimmed().isEmpty()) name_->setText(QFileInfo(path).fileName());
}

void TaskDialog::probeInput() {
    const QString input = input_->text().trimmed();
    if (input.isEmpty()) {
        QMessageBox::warning(this, "探测", "请先填写输入路径或 URL。");
        return;
    }
    probe_result_->setText("探测中...");
    QPointer<TaskDialog> guard(this);
    api_->post("/api/v1/probe", {{"input", input}},
               [guard](bool ok, const QJsonObject& data, const QString& error) {
                   if (!guard) return;
                   if (!ok) {
                       guard->probe_result_->setText("探测失败：" + error);
                       return;
                   }
                   const QJsonObject probe = data.value("probe").toObject();
                   QStringList lines;
                   lines << QString("%1 · %2 s · %3 Mbps")
                                .arg(probe.value("format").toString("?"))
                                .arg(probe.value("duration_sec").toDouble(), 0, 'f', 2)
                                .arg(probe.value("bitrate").toDouble() / 1e6, 0, 'f', 3);
                   for (const auto& value : probe.value("streams").toArray()) {
                       const QJsonObject stream = value.toObject();
                       if (stream.value("type").toString() == "video") {
                           lines << QString("#%1 video %2 %3x%4 %5 fps %6 bps")
                                        .arg(stream.value("index").toInt())
                                        .arg(stream.value("codec").toString())
                                        .arg(stream.value("width").toInt())
                                        .arg(stream.value("height").toInt())
                                        .arg(stream.value("fps").toDouble(), 0, 'f', 3)
                                        .arg(stream.value("bitrate").toDouble(), 0, 'f', 0);
                       } else if (stream.value("type").toString() == "audio") {
                           lines << QString("#%1 audio %2 %3 Hz %4 ch %5 bps")
                                        .arg(stream.value("index").toInt())
                                        .arg(stream.value("codec").toString())
                                        .arg(stream.value("sample_rate").toInt())
                                        .arg(stream.value("channels").toInt())
                                        .arg(stream.value("bitrate").toDouble(), 0, 'f', 0);
                       }
                   }
                   guard->probe_result_->setText(lines.join('\n'));
               });
}

void TaskDialog::syncEncodeFields() {
    const bool transcode = encode_mode_->currentData().toString() == "transcode";
    video_bitrate_->setEnabled(transcode);
    audio_bitrate_->setEnabled(transcode);
    preset_->setEnabled(transcode);
    gop_->setEnabled(transcode);
}

QJsonArray TaskDialog::extraOutputs() const {
    QJsonArray array;
    for (const QString& line : extras_->toPlainText().split('\n')) {
        const QString text = line.trimmed();
        if (text.isEmpty()) continue;
        const int colon = text.lastIndexOf(':');
        if (colon <= 0) continue;
        bool ok = false;
        const int port = text.mid(colon + 1).trimmed().toInt(&ok);
        const QString group = text.left(colon).trimmed();
        if (!ok || group.isEmpty() || port < 1 || port > 65535) continue;
        array.append(QJsonObject{{"group", group}, {"port", port}});
    }
    return array;
}

QJsonObject TaskDialog::body() const {
    const QJsonArray extra = extraOutputs();
    const QJsonObject source{
        {"uri", input_->text().trimmed()},
        {"loop", loop_->isChecked()},
    };
    const QJsonObject output{
        {"group", group_->text().trimmed()},
        {"port", port_->value()},
        {"nic", nic_->currentData().toString()},
        {"ttl", ttl_->value()},
        {"dscp", dscp_->value()},
        {"loopback", loopback_->isChecked()},
        {"pkt_size", packet_size_->value()},
        {"extra", extra},
    };
    const QJsonObject mux{
        {"format", "mpegts"},
        {"muxrate", muxrate_->value()},
        {"cbr", true},
    };
    const QJsonObject restart{
        {"auto", auto_restart_->isChecked()},
        {"max_retries", max_retries_->value()},
        {"backoff_ms", backoff_->value()},
    };
    const QJsonObject encode{
        {"mode", encode_mode_->currentData().toString()},
        {"video_bitrate", video_bitrate_->value()},
        {"audio_bitrate", audio_bitrate_->value()},
        {"preset", preset_->currentText()},
        {"gop", gop_->value()},
    };
    return {
        {"name", name_->text().trimmed()},
        {"source", source},
        {"output", output},
        {"mux", mux},
        {"encode", encode},
        {"restart", restart},
        {"realtime", realtime_->isChecked()},
        {"speed", speed_->value()},
        {"buffer_kb", buffer_kb_->value()},
        {"record", record_->isChecked()},
    };
}

void TaskDialog::accept() {
    if (input_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "任务", "请填写输入路径或 URL。");
        return;
    }
    if (group_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "任务", "请填写组播或目的地址。");
        return;
    }
    if (packet_size_->value() % 188 != 0) {
        QMessageBox::warning(this, "任务", "UDP 包大小必须是 188 的倍数。");
        return;
    }
    int nonempty_lines = 0;
    for (const QString& line : extras_->toPlainText().split('\n')) {
        if (!line.trimmed().isEmpty()) ++nonempty_lines;
    }
    const QJsonArray extra = extraOutputs();
    if (extra.size() != nonempty_lines) {
        QMessageBox::warning(this, "任务", "额外目的地格式必须是 group:port，每行一个。");
        return;
    }
    if (extra.size() > 4) {
        QMessageBox::warning(this, "任务", "最多 4 个额外目的地。");
        return;
    }
    QDialog::accept();
}
