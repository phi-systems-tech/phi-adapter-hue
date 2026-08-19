#pragma once

#include <functional>

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

    /**
     * @brief Install a "should I give up" predicate for the blocking requests.
     *
     * Polled while a request waits, so a shutdown does not have to sit out the
     * full request timeout (finding F-33). Callers pass
     * `[this] { return stopRequested(); }`; the SDK sets that flag from the host
     * thread, which is the only way this thread learns about the stop while it is
     * parked in a nested event loop.
     */
    void setCancelProbe(std::function<bool()> probe);

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

    bool cancelled() const;

    QNetworkAccessManager *m_manager = nullptr;
    std::function<bool()> m_cancelProbe;
};

} // namespace phicore::hue::ipc
