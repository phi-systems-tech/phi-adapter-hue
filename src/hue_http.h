#pragma once

#include <QString>
#include <QByteArray>

class QNetworkAccessManager;
class QNetworkRequest;

namespace phicore::hue::ipc {

struct ConnectionSettings {
    QString host;
    QString ip;
    int port = 0;
    bool useTls = true;
    QString appKey;
};

struct HttpResult {
    bool ok = false;
    int statusCode = 0;
    QByteArray payload;
    QString error;
};

class HttpClient
{
public:
    explicit HttpClient(QNetworkAccessManager *manager);

    HttpResult get(const ConnectionSettings &settings,
                   const QString &path,
                   bool includeAppKey = true,
                   const QByteArray &accept = QByteArrayLiteral("application/json"),
                   int timeoutMs = 10000) const;

    HttpResult postJson(const ConnectionSettings &settings,
                        const QString &path,
                        const QByteArray &payload,
                        bool includeAppKey,
                        int timeoutMs = 10000) const;

    HttpResult putJson(const ConnectionSettings &settings,
                       const QString &path,
                       const QByteArray &payload,
                       bool includeAppKey = true,
                       int timeoutMs = 10000) const;

    bool putJsonAsync(const ConnectionSettings &settings,
                      const QString &path,
                      const QByteArray &payload,
                      bool includeAppKey = true,
                      QString *error = nullptr) const;

    static QString effectiveHost(const ConnectionSettings &settings);

private:
    bool buildRequest(const ConnectionSettings &settings,
                      const QString &path,
                      bool includeAppKey,
                      const QByteArray &accept,
                      bool hasJsonBody,
                      QNetworkRequest *request,
                      QString *error = nullptr) const;

    HttpResult request(const ConnectionSettings &settings,
                       const QByteArray &method,
                       const QString &path,
                       const QByteArray &payload,
                       bool includeAppKey,
                       const QByteArray &accept,
                       int timeoutMs) const;

    QNetworkAccessManager *m_manager = nullptr;
};

} // namespace phicore::hue::ipc
