#include "gui/api_client.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace {

QString NormalizeBaseUrl(QString url) {
    url = url.trimmed();
    if (url.isEmpty()) url = "http://127.0.0.1:8787";
    if (!url.startsWith("http://", Qt::CaseInsensitive) &&
        !url.startsWith("https://", Qt::CaseInsensitive)) {
        url.prepend("http://");
    }
    while (url.endsWith('/')) url.chop(1);
    return url;
}

}  // namespace

ApiClient::ApiClient(QObject* parent) : QObject(parent) {}

void ApiClient::setBaseUrl(const QString& base_url) { base_url_ = NormalizeBaseUrl(base_url); }

void ApiClient::get(const QString& path, Callback callback) {
    send("GET", path, nullptr, std::move(callback));
}

void ApiClient::post(const QString& path, const QJsonObject& body, Callback callback) {
    send("POST", path, &body, std::move(callback));
}

void ApiClient::patch(const QString& path, const QJsonObject& body, Callback callback) {
    send("PATCH", path, &body, std::move(callback));
}

void ApiClient::del(const QString& path, Callback callback) {
    send("DELETE", path, nullptr, std::move(callback));
}

void ApiClient::send(const QByteArray& method, const QString& path, const QJsonObject* body,
                     Callback callback) {
    QString suffix = path;
    if (!suffix.startsWith('/')) suffix.prepend('/');

    QNetworkRequest request(QUrl(base_url_ + suffix));
    request.setTransferTimeout(15000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!token_.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    }

    QNetworkReply* reply = nullptr;
    if (method == "GET") {
        reply = manager_.get(request);
    } else if (method == "POST") {
        const QByteArray payload =
            body ? QJsonDocument(*body).toJson(QJsonDocument::Compact) : QByteArray();
        reply = manager_.post(request, payload);
    } else if (method == "PATCH") {
        const QByteArray payload =
            body ? QJsonDocument(*body).toJson(QJsonDocument::Compact) : QByteArray();
        reply = manager_.sendCustomRequest(request, "PATCH", payload);
    } else if (method == "DELETE") {
        reply = manager_.deleteResource(request);
    } else {
        if (callback) callback(false, {}, "unsupported HTTP method");
        return;
    }

    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        const QNetworkReply::NetworkError network_error = reply->error();
        const QString network_text = reply->errorString();
        reply->deleteLater();

        QJsonObject data;
        QString parse_error;
        if (!payload.isEmpty()) {
            QJsonParseError error{};
            const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
            if (error.error != QJsonParseError::NoError) {
                parse_error = error.errorString();
            } else if (document.isObject()) {
                data = document.object();
            }
        }

        const bool ok =
            network_error == QNetworkReply::NoError && status >= 200 && status < 300;
        QString error;
        if (!ok) {
            if (!data.value("error").toString().isEmpty()) {
                error = data.value("error").toString();
            } else if (status == 401) {
                error = "需要访问 token（401）";
            } else if (!network_text.isEmpty()) {
                error = network_text;
            } else if (!parse_error.isEmpty()) {
                error = "响应解析失败：" + parse_error;
            } else {
                error = "HTTP " + QString::number(status);
            }
        }
        if (callback) callback(ok, data, error);
    });
}
