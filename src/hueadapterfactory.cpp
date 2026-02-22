// adapters/plugins/hue/hueadapterfactory.cpp
#include "hueadapterfactory.h"
#include "hueadapter.h"

#include <QEventLoop>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#include <QSslSocket>
#endif

namespace phicore::adapter {

static const QByteArray kHueIconSvg = QByteArrayLiteral(
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Hue text logotype\">\n"
    "  <defs>\n"
    "    <linearGradient id=\"hueTextGradient\" x1=\"0\" y1=\"12\" x2=\"24\" y2=\"12\" gradientUnits=\"userSpaceOnUse\">\n"
    "      <stop offset=\"0%\" stop-color=\"#FF5F6D\"/>\n"
    "      <stop offset=\"25%\" stop-color=\"#FFC371\"/>\n"
    "      <stop offset=\"50%\" stop-color=\"#47E9A0\"/>\n"
    "      <stop offset=\"75%\" stop-color=\"#40C2FF\"/>\n"
    "      <stop offset=\"100%\" stop-color=\"#A659FF\"/>\n"
    "    </linearGradient>\n"
    "  </defs>\n"
    "  <text x=\"12\" y=\"16\" text-anchor=\"middle\" font-family=\"'Geist', 'Inter', 'Arial', sans-serif\" font-weight=\"600\" font-size=\"11\" fill=\"url(#hueTextGradient)\">hue</text>\n"
    "</svg>\n"
);

QByteArray HueAdapterFactory::icon() const
{
    return kHueIconSvg;
}

AdapterCapabilities HueAdapterFactory::capabilities() const
{
    AdapterCapabilities caps;
    caps.required = AdapterRequirement::Host
        | AdapterRequirement::ManualConfirm
        | AdapterRequirement::UsesRetryInterval; // link button
    // AppKey is usually created during pairing, but UI may still show a field.
    caps.optional = AdapterRequirement::SupportsTls
        | AdapterRequirement::AppKey
        | AdapterRequirement::Port;
    caps.flags |= AdapterFlag::AdapterFlagSupportsDiscovery;
    caps.flags |= AdapterFlag::AdapterFlagSupportsProbe;  // factory implements probe action
    caps.flags |= AdapterFlag::AdapterFlagSupportsRename; // device renames supported
    caps.defaults.insert(QStringLiteral("host"), QStringLiteral("philips-hue.local"));
    caps.defaults.insert(QStringLiteral("port"), 443);
    caps.defaults.insert(QStringLiteral("useTls"), true);
    caps.defaults.insert(QStringLiteral("retryIntervalMs"), 10000);
    AdapterActionDescriptor discoveryAction;
    discoveryAction.id          = QStringLiteral("startDeviceDiscovery");
    discoveryAction.label       = QStringLiteral("Search for Hue devices");
    discoveryAction.description = QStringLiteral("Trigger the bridge to enter device discovery mode.");
    caps.instanceActions.push_back(discoveryAction);
    AdapterActionDescriptor probeAction;
    probeAction.id = QStringLiteral("probe");
    probeAction.label = QStringLiteral("Test connection");
    probeAction.description = QStringLiteral("Reachability & credentials check");
    caps.factoryActions.push_back(probeAction);
    return caps;
}

discovery::DiscoveryList HueAdapterFactory::discover() const
{
    // First minimal version: no automatic discovery.
    // Later you can implement SSDP/UPnP or "discovery.meethue.com" lookup here.
    return {};
}

discovery::DiscoveryQueryList HueAdapterFactory::discoveryQueries() const
{
    discovery::DiscoveryQuery mdns;
    mdns.pluginType      = pluginType();
    mdns.kind            = discovery::DiscoveryKind::Mdns;
    mdns.mdnsServiceType = QStringLiteral("_hue._tcp");
    mdns.defaultPort     = 443;

    discovery::DiscoveryQuery ssdp;
    ssdp.pluginType = pluginType();
    ssdp.kind       = discovery::DiscoveryKind::Ssdp;
    ssdp.ssdpSt     = QStringLiteral("urn:schemas-upnp-org:device:Basic:1");
    ssdp.defaultPort = 80;

    return { mdns, ssdp };
}

AdapterConfigSchema HueAdapterFactory::configSchema(const Adapter &info) const
{
    AdapterConfigSchema schema;
    schema.title       = QStringLiteral("Philips Hue Bridge");
    schema.description = QStringLiteral("Configure connection to a Philips Hue bridge.");

    // Host
    {
        AdapterConfigField f;
        f.key         = QStringLiteral("host");
        f.type        = AdapterConfigFieldType::Hostname;
        f.label       = QStringLiteral("Bridge Host");
        f.description = QStringLiteral("IP address or hostname of the Hue bridge.");
        f.flags       = AdapterConfigFieldFlag::Required;
        f.placeholder = QStringLiteral("192.168.1.50");
        if (!info.host.isEmpty())
            f.defaultValue = info.host;
        schema.fields.push_back(f);
    }

    // Port (optional)
    {
        AdapterConfigField f;
        f.key         = QStringLiteral("port");
        f.type        = AdapterConfigFieldType::Port;
        f.label       = QStringLiteral("Port");
        f.description = QStringLiteral("TCP port for the Hue API (80 or 443).");
        f.defaultValue = info.port > 0 ? info.port : 443;
        schema.fields.push_back(f);
    }

    // Use TLS
    {
        AdapterConfigField f;
        f.key         = QStringLiteral("useTls");
        f.type        = AdapterConfigFieldType::Boolean;
        f.label       = QStringLiteral("Use HTTPS");
        f.description = QStringLiteral("Use HTTPS when talking to the Hue API.");
        if (info.flags.testFlag(AdapterFlag::AdapterFlagUseTls))
            f.defaultValue = true;
        schema.fields.push_back(f);
    }

    // AppKey (can be filled after pairing)
    {
        AdapterConfigField f;
        f.key         = QStringLiteral("appKey");
        f.type        = AdapterConfigFieldType::Password;
        f.label       = QStringLiteral("Application Key");
        f.description = QStringLiteral("Hue API application key (created by link button pairing).");
        f.flags       = AdapterConfigFieldFlag::Secret;
        schema.fields.push_back(f);
    }

    // Retry interval for eventstream reconnects
    {
        AdapterConfigField f;
        f.key         = QStringLiteral("retryIntervalMs");
        f.type        = AdapterConfigFieldType::Integer;
        f.label       = QStringLiteral("Retry interval");
        f.description = QStringLiteral("Reconnect interval while the bridge is offline.");
        f.defaultValue = 10000;
        schema.fields.push_back(f);
    }

    return schema;
}

ActionResponse HueAdapterFactory::invokeFactoryAction(const QString &actionId,
                                                      Adapter &infoInOut,
                                                      const QJsonObject &params) const
{
    ActionResponse resp;
    QString errorString;
    Q_UNUSED(params);
    if (actionId != QStringLiteral("probe")) {
        resp.status = CmdStatus::NotImplemented;
        resp.error = QStringLiteral("Unsupported factory action: %1").arg(actionId);
        return resp;
    }

    if (infoInOut.host.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("Host must not be empty.");
        return resp;
    }

    const bool useTls = infoInOut.flags.testFlag(AdapterFlag::AdapterFlagUseTls);
    if (infoInOut.port == 0) {
        infoInOut.port = useTls ? 443 : 80;
    }

    QUrl baseUrl;
    baseUrl.setScheme(useTls ? QStringLiteral("https") : QStringLiteral("http"));
    baseUrl.setHost(infoInOut.host);
    baseUrl.setPort(infoInOut.port);

    QNetworkAccessManager nam;

    auto makeRequest = [&](const QString &path) -> QNetworkRequest {
        QUrl url = baseUrl;
        if (!path.isEmpty()) {
            if (path.startsWith(QLatin1Char('/')))
                url.setPath(path);
            else
                url.setPath(QLatin1Char('/') + path);
        }
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("phi-core/1.0"));
        req.setRawHeader("Accept", "application/json");
#if QT_CONFIG(ssl)
        if (useTls) {
            QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
            req.setSslConfiguration(ssl);
        }
#endif
        return req;
    };

    auto performRequest = [&](const QString &path, const QByteArray &body, bool isPost, QByteArray &response) -> bool {
        QNetworkRequest req = makeRequest(path);
        if (isPost)
            req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

        QNetworkReply *reply = isPost ? nam.post(req, body) : nam.get(req);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        bool timedOut = false;

        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            loop.quit();
        });

        timer.start(10000);
        loop.exec();

        if (timedOut) {
            reply->abort();
            reply->deleteLater();
            errorString = QObject::tr("Request to Hue bridge timed out.");
            return false;
        }

        if (reply->error() != QNetworkReply::NoError) {
            errorString = reply->errorString();
            reply->deleteLater();
            return false;
        }

        response = reply->readAll();
        reply->deleteLater();
        return true;
    };

    auto failureResponse = [&](CmdStatus status = CmdStatus::Failure) {
        ActionResponse out;
        out.status = status;
        out.error = errorString;
        return out;
    };

    auto applyBridgeConfig = [&](const QJsonObject &obj) {
        const QString bridgeId = obj.value(QStringLiteral("bridgeid")).toString();
        if (!bridgeId.isEmpty())
            infoInOut.id = bridgeId;

        const QString name = obj.value(QStringLiteral("name")).toString();
        if (!name.isEmpty() && infoInOut.name.isEmpty())
            infoInOut.name = name;

        if (obj.contains(QStringLiteral("mac")))
            infoInOut.meta.insert(QStringLiteral("mac"), obj.value(QStringLiteral("mac")));
        if (obj.contains(QStringLiteral("modelid")))
            infoInOut.meta.insert(QStringLiteral("modelId"), obj.value(QStringLiteral("modelid")));
        if (obj.contains(QStringLiteral("swversion")))
            infoInOut.meta.insert(QStringLiteral("swVersion"), obj.value(QStringLiteral("swversion")));
    };

    auto fetchBridgeConfig = [&](const QString &username, QString *customError = nullptr) -> bool {
        if (username.isEmpty())
            return true;

        QByteArray data;
        const QString path = QStringLiteral("/api/%1/config").arg(username);
        if (!performRequest(path, QByteArray(), false, data)) {
            if (customError)
                *customError = errorString;
            return false;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (doc.isArray()) {
            QString desc;
            const QJsonArray arr = doc.array();
            if (!arr.isEmpty()) {
                const QJsonObject entry = arr.first().toObject();
                const QJsonObject errObj = entry.value(QStringLiteral("error")).toObject();
                desc = errObj.value(QStringLiteral("description")).toString();
            }
            if (desc.isEmpty())
                desc = QObject::tr("Hue bridge rejected the request.");
            if (customError)
                *customError = desc;
            else
                errorString = desc;
            return false;
        }

        if (!doc.isObject()) {
            const QString msg = QObject::tr("Unexpected response from Hue bridge.");
            if (customError)
                *customError = msg;
            else
                errorString = msg;
            return false;
        }

        applyBridgeConfig(doc.object());
        return true;
    };

    if (!infoInOut.token.isEmpty()) {
        if (!fetchBridgeConfig(infoInOut.token))
            return failureResponse();
        resp.status = CmdStatus::Success;
        return resp;
    }

    QJsonObject payload;
    const QString localName = QHostInfo::localHostName();
    const QString deviceType = QStringLiteral("phi-core#%1")
        .arg(localName.left(20).isEmpty() ? QStringLiteral("adapter") : localName.left(20));
    payload.insert(QStringLiteral("devicetype"), deviceType);
    payload.insert(QStringLiteral("generateclientkey"), true);

    QByteArray response;
    if (!performRequest(QStringLiteral("/api"),
                        QJsonDocument(payload).toJson(QJsonDocument::Compact),
                        true,
                        response)) {
        return failureResponse();
    }

    const QJsonDocument doc = QJsonDocument::fromJson(response);
    if (!doc.isArray()) {
        errorString = QObject::tr("Unexpected response from Hue bridge.");
        return failureResponse();
    }

    const QJsonArray arr = doc.array();
    for (const QJsonValue &value : arr) {
        const QJsonObject entry = value.toObject();
        if (entry.contains(QStringLiteral("error"))) {
            const QJsonObject err = entry.value(QStringLiteral("error")).toObject();
            const int type = err.value(QStringLiteral("type")).toInt();
            const QString desc = err.value(QStringLiteral("description")).toString();
            if (type == 101) {
                errorString = QObject::tr("Press the link button on the Hue bridge, then retry.");
            } else {
                errorString = desc.isEmpty()
                    ? QObject::tr("Hue bridge rejected the request.")
                    : desc;
            }
            return failureResponse();
        }

        if (entry.contains(QStringLiteral("success"))) {
            const QJsonObject success = entry.value(QStringLiteral("success")).toObject();
            const QString username = success.value(QStringLiteral("username")).toString();
            if (!username.isEmpty())
                infoInOut.token = username;
            const QString clientKey = success.value(QStringLiteral("clientkey")).toString();
            if (!clientKey.isEmpty())
                infoInOut.meta.insert(QStringLiteral("clientKey"), clientKey);

            // Try to read bridge metadata, ignore failures to keep pairing flow smooth.
            if (!infoInOut.token.isEmpty())
                fetchBridgeConfig(infoInOut.token);

            resp.status = CmdStatus::Success;
            return resp;
        }
    }

    errorString = QObject::tr("Hue bridge returned an unexpected payload.");
    return failureResponse();
}

AdapterInterface *HueAdapterFactory::create(QObject *parent)
{
    return new HueAdapter(parent);
}

} // namespace phicore::adapter
