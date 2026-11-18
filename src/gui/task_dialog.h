#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class ApiClient;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

class TaskDialog : public QDialog {
    Q_OBJECT

public:
    TaskDialog(ApiClient* api, const QJsonArray& nics, QWidget* parent = nullptr);

    void setTask(const QJsonObject& task);
    QJsonObject body() const;

protected:
    void accept() override;

private:
    void populateNics(const QString& selected);
    void browseServerMedia();
    void browseLocalFile();
    void probeInput();
    QJsonArray extraOutputs() const;
    void syncEncodeFields();

    ApiClient* api_;
    QJsonArray nics_;
    QLineEdit* name_;
    QLineEdit* input_;
    QCheckBox* loop_;
    QCheckBox* realtime_;
    QLineEdit* group_;
    QSpinBox* port_;
    QComboBox* nic_;
    QSpinBox* ttl_;
    QSpinBox* dscp_;
    QSpinBox* muxrate_;
    QSpinBox* packet_size_;
    QSpinBox* buffer_kb_;
    QDoubleSpinBox* speed_;
    QCheckBox* loopback_;
    QPlainTextEdit* extras_;
    QCheckBox* record_;
    QComboBox* encode_mode_;
    QSpinBox* video_bitrate_;
    QSpinBox* audio_bitrate_;
    QComboBox* preset_;
    QSpinBox* gop_;
    QCheckBox* auto_restart_;
    QSpinBox* max_retries_;
    QSpinBox* backoff_;
    QLabel* probe_result_;
};
