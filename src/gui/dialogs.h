#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class ApiClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QComboBox;

class MediaBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit MediaBrowserDialog(ApiClient* api, QWidget* parent = nullptr);
    QString selectedPath() const { return selected_path_; }

private:
    void load(const QString& path);
    void chooseCurrent();

    ApiClient* api_;
    QLabel* path_label_;
    QListWidget* list_;
    QPushButton* up_button_;
    QPushButton* select_button_;
    QString current_path_;
    QString parent_path_;
    QString selected_path_;
};

class LogsDialog : public QDialog {
    Q_OBJECT

public:
    LogsDialog(ApiClient* api, const QString& task_id, const QString& title,
               QWidget* parent = nullptr);

private:
    void refresh();

    ApiClient* api_;
    QString task_id_;
    QPlainTextEdit* text_;
    QTimer* timer_;
};

class MonitorDialog : public QDialog {
    Q_OBJECT

public:
    MonitorDialog(ApiClient* api, const QJsonArray& nics, QWidget* parent = nullptr);

private:
    void startMonitor();
    void stopMonitor();
    void refresh();
    void setStatus(const QJsonObject& monitor);
    void setRunning(bool running);

    ApiClient* api_;
    QLineEdit* group_;
    QSpinBox* port_;
    QComboBox* iface_;
    QSpinBox* duration_;
    QPushButton* start_button_;
    QPushButton* stop_button_;
    QLabel* status_;
    QLabel* bitrate_;
    QLabel* packets_;
    QLabel* bytes_;
    QLabel* elapsed_;
    QLabel* sync_errors_;
    QLabel* cc_errors_;
    QTimer* timer_;
};
