#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMap>
#include <QString>

class ApiClient;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private:
    void buildUi();
    void connectServer();
    void manualConnect();
    bool startLocalDaemon();
    void refreshHealth();
    void refreshTasks();
    void refreshNics();
    void updateTable();
    void updateSelection();
    void updateCards();

    QString selectedTaskId() const;
    QJsonObject selectedTask() const;
    void setStatus(const QString& text, bool error = false);
    void showError(const QString& title, const QString& error);

    void newTask();
    void editTask();
    void startTask();
    void stopTask();
    void deleteTask();
    void openLogs();
    void openMonitor();
    void openVlc();

    ApiClient* api_;
    QLineEdit* server_url_;
    QLineEdit* token_;
    QPushButton* connect_button_;
    QLabel* status_label_;
    QLabel* tasks_card_;
    QLabel* running_card_;
    QLabel* bitrate_card_;
    QLabel* auth_card_;
    QTableWidget* table_;

    QPushButton* new_button_;
    QPushButton* edit_button_;
    QPushButton* start_button_;
    QPushButton* stop_button_;
    QPushButton* delete_button_;
    QPushButton* logs_button_;
    QPushButton* monitor_button_;
    QPushButton* vlc_button_;
    QPushButton* refresh_button_;

    QJsonArray tasks_;
    QJsonArray nics_;
    QMap<QString, QJsonObject> task_map_;
    QTimer* poll_timer_;
    int poll_tick_ = 0;
    bool connected_ = false;
    bool auth_hint_shown_ = false;
    bool local_start_attempted_ = false;
};
