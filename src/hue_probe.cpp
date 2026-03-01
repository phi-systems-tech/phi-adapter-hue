#include "hue_probe.h"

#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace phicore::hue::ipc {

namespace {

QString extractHueErrorDescription(const QByteArray &payload)
{
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isArray())
        return {};

    const QJsonArray arr = doc.array();
    for (const QJsonValue &value : arr) {
        if (!value.isObject())
            continue;
        const QJsonObject errObj = value.toObject().value(QStringLiteral("error")).toObject();
        if (errObj.isEmpty())
            continue;

        const int type = errObj.value(QStringLiteral("type")).toInt();
        const QString description = errObj.value(QStringLiteral("description")).toString();
        if (type == 101)
            return QStringLiteral("Press the link button on the Hue bridge, then retry.");
        if (!description.isEmpty())
            return description;
    }

    return {};
}

bool parseApiCreateUserResponse(const QByteArray &payload,
                                QString *appKey,
                                QString *clientKey,
                                QString *error)
{
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isArray()) {
        if (error)
            *error = QStringLiteral("Unexpected response from Hue bridge");
        return false;
    }

    const QJsonArray arr = doc.array();
    for (const QJsonValue &value : arr) {
        if (!value.isObject())
            continue;
        const QJsonObject entry = value.toObject();

        const QJsonObject errObj = entry.value(QStringLiteral("error")).toObject();
        if (!errObj.isEmpty()) {
            const int type = errObj.value(QStringLiteral("type")).toInt();
            const QString description = errObj.value(QStringLiteral("description")).toString();
            if (error) {
                if (type == 101)
                    *error = QStringLiteral("Press the link button on the Hue bridge, then retry.");
                else if (!description.isEmpty())
                    *error = description;
                else
                    *error = QStringLiteral("Hue bridge rejected the request");
            }
            return false;
        }

        const QJsonObject successObj = entry.value(QStringLiteral("success")).toObject();
        if (successObj.isEmpty())
            continue;

        if (appKey)
            *appKey = successObj.value(QStringLiteral("username")).toString().trimmed();
        if (clientKey)
            *clientKey = successObj.value(QStringLiteral("clientkey")).toString().trimmed();
        return appKey ? !appKey->isEmpty() : true;
    }

    if (error)
        *error = QStringLiteral("Hue bridge returned no success entry");
    return false;
}

} // namespace

ProbeResult runProbe(HttpClient &http, const ConnectionSettings &settings, int timeoutMs)
{
    ProbeResult out;

    if (HttpClient::effectiveHost(settings).isEmpty()) {
        out.error = QStringLiteral("Host must not be empty");
        return out;
    }

    ConnectionSettings probeSettings = settings;
    if (probeSettings.port <= 0)
        probeSettings.port = probeSettings.useTls ? 443 : 80;

    if (!probeSettings.appKey.trimmed().isEmpty()) {
        const HttpResult bridge = http.get(probeSettings,
                                           QStringLiteral("/clip/v2/resource/bridge"),
                                           true,
                                           QByteArrayLiteral("application/json"),
                                           timeoutMs);
        if (bridge.ok) {
            out.ok = true;
            out.message = QStringLiteral("Bridge reachable and credentials valid");
            out.appKey = probeSettings.appKey;
            return out;
        }

        const QString hueError = extractHueErrorDescription(bridge.payload);
        out.error = hueError.isEmpty() ? bridge.error : hueError;
        if (out.error.isEmpty())
            out.error = QStringLiteral("Hue bridge rejected the application key");
        return out;
    }

    QJsonObject payload;
    const QString localHost = QHostInfo::localHostName().left(20);
    payload.insert(QStringLiteral("devicetype"),
                   QStringLiteral("phi-core#%1").arg(localHost.isEmpty() ? QStringLiteral("adapter") : localHost));
    payload.insert(QStringLiteral("generateclientkey"), true);

    const HttpResult createUser = http.postJson(probeSettings,
                                                QStringLiteral("/api"),
                                                QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                                false,
                                                timeoutMs);
    if (!createUser.ok) {
        out.error = extractHueErrorDescription(createUser.payload);
        if (out.error.isEmpty())
            out.error = createUser.error.isEmpty()
                ? QStringLiteral("Failed to create Hue application key")
                : createUser.error;
        return out;
    }

    QString createdAppKey;
    QString createdClientKey;
    QString parseError;
    if (!parseApiCreateUserResponse(createUser.payload, &createdAppKey, &createdClientKey, &parseError)) {
        out.error = parseError;
        return out;
    }

    out.ok = true;
    out.appKey = createdAppKey;
    out.message = QStringLiteral("Pairing successful");

    if (!createdClientKey.isEmpty())
        out.metaPatch.insert(QStringLiteral("clientKey"), createdClientKey);

    return out;
}

} // namespace phicore::hue::ipc
