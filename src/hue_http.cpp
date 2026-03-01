#include "hue_http.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#include <QSslSocket>
#endif

namespace phicore::hue::ipc {

HttpClient::HttpClient(QNetworkAccessManager *manager)
    : m_manager(manager)
{
}

QString HttpClient::effectiveHost(const ConnectionSettings &settings)
{
    const QString host = settings.host.trimmed();
    if (!host.isEmpty())
        return host;
    return settings.ip.trimmed();
}

HttpResult HttpClient::get(const ConnectionSettings &settings,
                           const QString &path,
                           bool includeAppKey,
                           const QByteArray &accept,
                           int timeoutMs) const
{
    return request(settings, QByteArrayLiteral("GET"), path, {}, includeAppKey, accept, timeoutMs);
}

HttpResult HttpClient::postJson(const ConnectionSettings &settings,
                                const QString &path,
                                const QByteArray &payload,
                                bool includeAppKey,
                                int timeoutMs) const
{
    return request(settings,
                   QByteArrayLiteral("POST"),
                   path,
                   payload,
                   includeAppKey,
                   QByteArrayLiteral("application/json"),
                   timeoutMs);
}

HttpResult HttpClient::putJson(const ConnectionSettings &settings,
                               const QString &path,
                               const QByteArray &payload,
                               bool includeAppKey,
                               int timeoutMs) const
{
    return request(settings,
                   QByteArrayLiteral("PUT"),
                   path,
                   payload,
                   includeAppKey,
                   QByteArrayLiteral("application/json"),
                   timeoutMs);
}

bool HttpClient::putJsonAsync(const ConnectionSettings &settings,
                              const QString &path,
                              const QByteArray &payload,
                              bool includeAppKey,
                              QString *error) const
{
    if (!m_manager) {
        if (error)
            *error = QStringLiteral("Network manager unavailable");
        return false;
    }

    QNetworkRequest request;
    if (!buildRequest(settings,
                      path,
                      includeAppKey,
                      QByteArrayLiteral("application/json"),
                      true,
                      &request,
                      error)) {
        return false;
    }

    QNetworkReply *reply = m_manager->sendCustomRequest(request, QByteArrayLiteral("PUT"), payload);
    if (!reply) {
        if (error)
            *error = QStringLiteral("Failed to create network request");
        return false;
    }

    QObject::connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
    if (error)
        error->clear();
    return true;
}

bool HttpClient::buildRequest(const ConnectionSettings &settings,
                              const QString &path,
                              bool includeAppKey,
                              const QByteArray &accept,
                              bool hasJsonBody,
                              QNetworkRequest *request,
                              QString *error) const
{
    if (!request) {
        if (error)
            *error = QStringLiteral("Request object is null");
        return false;
    }

    const QString host = effectiveHost(settings);
    if (host.isEmpty()) {
        if (error)
            *error = QStringLiteral("Bridge host is empty");
        return false;
    }

    const bool useTls = settings.useTls;
    const int defaultPort = useTls ? 443 : 80;
    const int port = settings.port > 0 ? settings.port : defaultPort;

    QUrl url;
    url.setScheme(useTls ? QStringLiteral("https") : QStringLiteral("http"));
    url.setHost(host);
    url.setPort(port);
    if (path.startsWith(QLatin1Char('/')))
        url.setPath(path);
    else
        url.setPath(QStringLiteral("/") + path);

    QNetworkRequest out(url);
    out.setRawHeader("Accept", accept);
    out.setRawHeader("User-Agent", "phi-adapter-hue-ipc/1.0");
    if (hasJsonBody)
        out.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (includeAppKey && !settings.appKey.isEmpty())
        out.setRawHeader("hue-application-key", settings.appKey.toUtf8());

#if QT_CONFIG(ssl)
    if (useTls) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        out.setSslConfiguration(ssl);
    }
#endif

    *request = out;
    if (error)
        error->clear();
    return true;
}

HttpResult HttpClient::request(const ConnectionSettings &settings,
                               const QByteArray &method,
                               const QString &path,
                               const QByteArray &payload,
                               bool includeAppKey,
                               const QByteArray &accept,
                               int timeoutMs) const
{
    HttpResult result;

    if (!m_manager) {
        result.error = QStringLiteral("Network manager unavailable");
        return result;
    }

    QNetworkRequest requestObj;
    if (!buildRequest(settings,
                      path,
                      includeAppKey,
                      accept,
                      !payload.isEmpty(),
                      &requestObj,
                      &result.error)) {
        return result;
    }

    QNetworkReply *reply = nullptr;
    if (method == QByteArrayLiteral("GET")) {
        reply = m_manager->get(requestObj);
    } else if (method == QByteArrayLiteral("POST")) {
        reply = m_manager->post(requestObj, payload);
    } else {
        reply = m_manager->sendCustomRequest(requestObj, method, payload);
    }

    if (!reply) {
        result.error = QStringLiteral("Failed to create network request");
        return result;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });

    timer.start(timeoutMs > 0 ? timeoutMs : 10000);
    loop.exec();

    if (timedOut) {
        reply->abort();
        reply->deleteLater();
        result.error = QStringLiteral("Request timed out");
        return result;
    }

    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.payload = reply->readAll();

    if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
        reply->deleteLater();
        return result;
    }

    if (result.statusCode >= 200 && result.statusCode < 300) {
        result.ok = true;
    } else {
        result.error = QStringLiteral("HTTP %1").arg(result.statusCode);
    }

    reply->deleteLater();
    return result;
}

} // namespace phicore::hue::ipc
