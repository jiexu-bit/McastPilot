#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

#include <functional>

class ApiClient : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(bool ok, const QJsonObject& data, const QString& error)>;

    explicit ApiClient(QObject* parent = nullptr);

    void setBaseUrl(const QString& base_url);
    QString baseUrl() const { return base_url_; }

    void setToken(const QString& token) { token_ = token; }
    QString token() const { return token_; }

    void get(const QString& path, Callback callback);
    void post(const QString& path, const QJsonObject& body, Callback callback);
    void patch(const QString& path, const QJsonObject& body, Callback callback);
    void del(const QString& path, Callback callback);

private:
    void send(const QByteArray& method, const QString& path, const QJsonObject* body,
              Callback callback);

    QNetworkAccessManager manager_;
    QString base_url_ = "http://127.0.0.1:8787";
    QString token_;
};
