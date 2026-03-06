#include "hue_sidecar.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#include <QSslSocket>
#endif

#include "hue_schema.h"

namespace phicore::hue::ipc {

namespace {

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

constexpr int kButtonMultiPressWindowMs = 1300;
constexpr int kButtonLongPressRepeatWindowMs = 800;
constexpr int kDialResetDelayMs = 1500;
constexpr int kEventStreamFastRetryMs = 2000;
constexpr int kEventStreamFastRetryAttempts = 5;

QString channelBindingKey(const QString &deviceExternalId, const QString &channelExternalId)
{
    return deviceExternalId + QLatin1Char('|') + channelExternalId;
}

qint64 parseHueTimestampMs(const QString &isoText)
{
    if (isoText.isEmpty())
        return 0;

    QDateTime dt = QDateTime::fromString(isoText, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(isoText, Qt::ISODate);
    if (!dt.isValid())
        return 0;
    if (dt.timeSpec() != Qt::UTC)
        dt = dt.toUTC();
    return dt.toMSecsSinceEpoch();
}

std::optional<double> scalarAsDouble(const v1::ScalarValue &value)
{
    if (const auto *d = std::get_if<double>(&value))
        return *d;
    if (const auto *i = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*i);
    if (const auto *b = std::get_if<bool>(&value))
        return *b ? 1.0 : 0.0;
    return std::nullopt;
}

std::optional<bool> scalarAsBool(const v1::ScalarValue &value)
{
    if (const auto *b = std::get_if<bool>(&value))
        return *b;
    if (const auto *i = std::get_if<std::int64_t>(&value))
        return *i != 0;
    if (const auto *d = std::get_if<double>(&value))
        return *d != 0.0;
    if (const auto *s = std::get_if<std::string>(&value)) {
        const QString text = QString::fromStdString(*s).trimmed().toLower();
        if (text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("on"))
            return true;
        if (text == QLatin1String("0") || text == QLatin1String("false") || text == QLatin1String("off"))
            return false;
    }
    return std::nullopt;
}

QString extractHueError(const QByteArray &payload)
{
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject())
        return {};

    const QJsonArray errors = doc.object().value(QStringLiteral("errors")).toArray();
    if (!errors.isEmpty()) {
        const QJsonObject err = errors.first().toObject();
        const QString description = err.value(QStringLiteral("description")).toString();
        if (!description.isEmpty())
            return description;
    }

    return {};
}

int readInt(const QJsonObject &obj, const QString &key, int fallback)
{
    if (!obj.contains(key))
        return fallback;
    bool ok = false;
    const int value = obj.value(key).toVariant().toInt(&ok);
    return ok ? value : fallback;
}

v1::ButtonEventCode mapHueButtonEvent(const QString &eventRaw)
{
    const QString event = eventRaw.trimmed().toLower();
    if (event == QLatin1String("initial_press"))
        return v1::ButtonEventCode::InitialPress;
    if (event == QLatin1String("long_press"))
        return v1::ButtonEventCode::LongPress;
    if (event == QLatin1String("repeat"))
        return v1::ButtonEventCode::Repeat;
    if (event == QLatin1String("short_release"))
        return v1::ButtonEventCode::ShortPressRelease;
    if (event == QLatin1String("long_release"))
        return v1::ButtonEventCode::LongPressRelease;
    return v1::ButtonEventCode::None;
}

std::optional<std::int64_t> parseConnectivityStatus(const QJsonObject &resourceObj)
{
    const QString status = resourceObj.value(QStringLiteral("status")).toString().trimmed().toLower();
    if (status == QLatin1String("connected"))
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Connected);
    if (status == QLatin1String("disconnected"))
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Disconnected);
    if (status.contains(QStringLiteral("issue"))
        || status.contains(QStringLiteral("limited"))
        || status.contains(QStringLiteral("degraded"))) {
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Limited);
    }
    if (!status.isEmpty())
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Unknown);
    return std::nullopt;
}

QString hueEffectNameForDeviceEffect(v1::DeviceEffect effect)
{
    switch (effect) {
    case v1::DeviceEffect::Candle:
        return QStringLiteral("candle");
    case v1::DeviceEffect::Fireplace:
        return QStringLiteral("fire");
    case v1::DeviceEffect::Sparkle:
        return QStringLiteral("sparkle");
    case v1::DeviceEffect::ColorLoop:
        return QStringLiteral("colorloop");
    case v1::DeviceEffect::Relax:
        return QStringLiteral("sunset");
    case v1::DeviceEffect::Concentrate:
        return QStringLiteral("enchant");
    case v1::DeviceEffect::Alarm:
        return QStringLiteral("prism");
    default:
        break;
    }
    return {};
}

QJsonObject parseJsonObject(const std::string &json)
{
    const QByteArray bytes = QByteArray::fromStdString(json).trimmed();
    if (bytes.isEmpty())
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject())
        return {};
    return doc.object();
}

QString effectMetaValue(const v1::DeviceEffectDescriptor &descriptor, const QString &key)
{
    const QJsonObject meta = parseJsonObject(descriptor.metaJson);
    return meta.value(key).toString().trimmed();
}

} // namespace

HueAdapterInstance::HueAdapterInstance() = default;

bool HueAdapterInstance::start()
{
    if (!m_requestNetwork)
        m_requestNetwork = std::make_unique<QNetworkAccessManager>();
    if (!m_eventStreamNetwork)
        m_eventStreamNetwork = std::make_unique<QNetworkAccessManager>();
    if (!m_http)
        m_http = std::make_unique<HttpClient>(m_requestNetwork.get());

    m_runtimeConfigured = false;
    m_nextPollDueMs = 0;
    m_nextEventStreamRetryDueMs = 0;
    m_eventStreamRetryCount = 0;
    m_buttonMultiPress.clear();
    m_buttonLastEventCode.clear();
    m_buttonLastEventTs.clear();
    m_lastDialValueByDevice.clear();
    m_dialResetDueMs.clear();
    m_devices.clear();
    m_lightResourceByDevice.clear();
    m_knownRooms.clear();
    m_knownGroups.clear();
    m_knownScenes.clear();
    stopEventStream();
    setConnectionState(false);

    if (!m_tickTimer) {
        m_tickTimer = std::make_unique<QTimer>();
        m_tickTimer->setInterval(250);
        m_tickTimer->setSingleShot(false);
        QObject::connect(m_tickTimer.get(), &QTimer::timeout, [this]() {
            tick();
        });
    }
    if (!m_tickTimer->isActive())
        m_tickTimer->start();

    if (hasConfig()) {
        applyRuntimeConfig(config());
        m_runtimeConfigured = true;
        startEventStream();
    }

    return true;
}

void HueAdapterInstance::stop()
{
    m_runtimeConfigured = false;
    if (m_tickTimer && m_tickTimer->isActive())
        m_tickTimer->stop();
    stopEventStream();
    m_buttonMultiPress.clear();
    m_buttonLastEventCode.clear();
    m_buttonLastEventTs.clear();
    m_lastDialValueByDevice.clear();
    m_dialResetDueMs.clear();
    m_devices.clear();
    m_lightResourceByDevice.clear();
    m_knownRooms.clear();
    m_knownGroups.clear();
    m_knownScenes.clear();
    setConnectionState(false);
}

void HueAdapterInstance::tick()
{
    if (!m_runtimeConfigured)
        return;

    const std::int64_t now = nowMs();
    processPendingButtonAggregates(now);
    pumpEventStream(now);
    processPendingButtonAggregates(now);
    processPendingDialResets(now);

    if (!m_eventStreamReply && now >= m_nextEventStreamRetryDueMs)
        startEventStream();

    if (m_nextPollDueMs > now)
        return;

    QString error;
    const bool ok = pollBridge(&error);
    if (!ok) {
        setConnectionState(false);
        if (!error.isEmpty()) {
            std::cerr << "hue-ipc poll failed: " << error.toStdString() << '\n';
            sendError(error.toStdString());
        }
        m_nextPollDueMs = now + std::max(1000, m_retryIntervalMs);
        return;
    }

    const int pollInterval = m_eventStreamActive
        ? std::max(m_pollIntervalMs, 60000)
        : m_pollIntervalMs;
    m_nextPollDueMs = now + std::max(1000, pollInterval);
}

void HueAdapterInstance::onConnected()
{
    std::cerr << "hue-ipc connected" << '\n';
    if (m_runtimeConfigured)
        startEventStream();
}

void HueAdapterInstance::onDisconnected()
{
    m_runtimeConfigured = false;
    if (m_tickTimer && m_tickTimer->isActive())
        m_tickTimer->stop();
    stopEventStream();
    m_buttonMultiPress.clear();
    m_buttonLastEventCode.clear();
    m_buttonLastEventTs.clear();
    m_lastDialValueByDevice.clear();
    m_dialResetDueMs.clear();
    m_devices.clear();
    m_lightResourceByDevice.clear();
    m_knownRooms.clear();
    m_knownGroups.clear();
    m_knownScenes.clear();
    setConnectionState(false);
    std::cerr << "hue-ipc disconnected" << '\n';
}

void HueAdapterInstance::onConfigChanged(const sdk::ConfigChangedRequest &request)
{
    applyRuntimeConfig(request);
    m_runtimeConfigured = true;
    m_nextPollDueMs = 0;
    m_nextEventStreamRetryDueMs = 0;
    m_eventStreamRetryCount = 0;
    stopEventStream();
    startEventStream();

    std::cerr << "hue-ipc config.changed adapterId=" << request.adapterId
              << " externalId=" << request.adapter.externalId
              << " ip=" << m_settings.ip.toStdString()
              << " port=" << m_settings.port
              << " useTls=" << (m_settings.useTls ? "true" : "false")
              << '\n';
}

void HueAdapterInstance::onChannelInvoke(const sdk::ChannelInvokeRequest &request)
{
    submitCmdResult(handleChannelInvoke(request), "channel.invoke");
}

phicore::adapter::v1::CmdResponse HueAdapterInstance::handleChannelInvoke(const sdk::ChannelInvokeRequest &request)
{
    if (!m_runtimeConfigured)
        return failureResponse(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not configured"));

    const QString deviceExternalId = QString::fromStdString(request.deviceExternalId);
    const QString channelExternalId = QString::fromStdString(request.channelExternalId);

    QString lightId = m_lightResourceByDevice.value(deviceExternalId);
    if (lightId.isEmpty()) {
        QString refreshError;
        pollBridge(&refreshError);
        lightId = m_lightResourceByDevice.value(deviceExternalId);
    }

    if (lightId.isEmpty()) {
        return failureResponse(request.cmdId,
                               CmdStatus::InvalidArgument,
                               QStringLiteral("No Hue light resource for device"));
    }

    QString payloadError;
    const QByteArray payload = buildLightCommandPayload(channelExternalId, request, &payloadError);
    if (payload.isEmpty())
        return failureResponse(request.cmdId, CmdStatus::InvalidArgument, payloadError);

    QString asyncError;
    if (!m_http->putJsonAsync(m_settings,
                             QStringLiteral("/clip/v2/resource/light/%1").arg(lightId),
                             payload,
                             true,
                             &asyncError)) {
        const QString error = asyncError.isEmpty() ? QStringLiteral("Hue command could not be sent") : asyncError;
        return failureResponse(request.cmdId, CmdStatus::TemporarilyOffline, error);
    }

    CmdResponse resp = successResponse(request.cmdId);
    if (request.hasScalarValue)
        resp.finalValue = request.value;

    v1::Utf8String sendError;
    if (channelExternalId == QLatin1String("on") && request.hasScalarValue) {
        const auto on = scalarAsBool(request.value);
        if (on.has_value())
            sendChannelStateUpdated(request.deviceExternalId, request.channelExternalId, *on, nowMs(), &sendError);
    } else if ((channelExternalId == QLatin1String("bri") || channelExternalId == QLatin1String("ct"))
               && request.hasScalarValue) {
        const auto value = scalarAsDouble(request.value);
        if (value.has_value()) {
            if (channelExternalId == QLatin1String("ct"))
                sendChannelStateUpdated(request.deviceExternalId,
                                        request.channelExternalId,
                                        static_cast<std::int64_t>(std::llround(*value)),
                                        nowMs(),
                                        &sendError);
            else {
                const double brightness = std::clamp(*value, 0.0, 100.0);
                sendChannelStateUpdated(request.deviceExternalId,
                                        request.channelExternalId,
                                        brightness,
                                        nowMs(),
                                        &sendError);
                sendChannelStateUpdated(request.deviceExternalId,
                                        "on",
                                        brightness > 0.0,
                                        nowMs(),
                                        &sendError);
            }
        }
    }

    return resp;
}

void HueAdapterInstance::onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request)
{
    submitActionResult(handleAdapterActionInvoke(request), "adapter.action.invoke");
}

phicore::adapter::v1::ActionResponse HueAdapterInstance::handleAdapterActionInvoke(
    const sdk::AdapterActionInvokeRequest &request)
{
    const QString actionId = QString::fromStdString(request.actionId);
    if (actionId == QLatin1String("startDeviceDiscovery"))
        return invokeStartDeviceDiscovery(request);

    ActionResponse resp;
    resp.id = request.cmdId;
    resp.status = CmdStatus::NotImplemented;
    resp.error = "Unsupported adapter action";
    resp.tsMs = nowMs();
    return resp;
}

void HueAdapterInstance::onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request)
{
    submitCmdResult(handleDeviceNameUpdate(request), "device.name.update");
}

phicore::adapter::v1::CmdResponse HueAdapterInstance::handleDeviceNameUpdate(
    const sdk::DeviceNameUpdateRequest &request)
{
    if (request.deviceExternalId.empty())
        return failureResponse(request.cmdId, CmdStatus::InvalidArgument, QStringLiteral("deviceExternalId missing"));
    if (request.name.empty())
        return failureResponse(request.cmdId, CmdStatus::InvalidArgument, QStringLiteral("name missing"));

    const QString deviceExternalId = QString::fromStdString(request.deviceExternalId);

    QJsonObject metadata;
    metadata.insert(QStringLiteral("name"), QString::fromStdString(request.name));
    QJsonObject payload;
    payload.insert(QStringLiteral("metadata"), metadata);

    const HttpResult result = m_http->putJson(m_settings,
                                             QStringLiteral("/clip/v2/resource/device/%1").arg(deviceExternalId),
                                             QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                             true,
                                             10000);
    if (!result.ok) {
        QString error = extractHueError(result.payload);
        if (error.isEmpty())
            error = result.error.isEmpty() ? QStringLiteral("Rename request failed") : result.error;
        return failureResponse(request.cmdId, CmdStatus::Failure, error);
    }

    auto it = m_devices.find(deviceExternalId);
    if (it != m_devices.end()) {
        it->device.name = request.name;
        v1::Utf8String sendError;
        sendDeviceUpdated(it->device, it->channels, &sendError);
    }

    return successResponse(request.cmdId);
}

void HueAdapterInstance::onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request)
{
    submitCmdResult(handleDeviceEffectInvoke(request), "device.effect.invoke");
}

phicore::adapter::v1::CmdResponse HueAdapterInstance::handleDeviceEffectInvoke(
    const sdk::DeviceEffectInvokeRequest &request)
{
    if (request.deviceExternalId.empty()) {
        return failureResponse(request.cmdId,
                               CmdStatus::InvalidArgument,
                               QStringLiteral("deviceExternalId missing"));
    }

    const QString deviceExternalId = QString::fromStdString(request.deviceExternalId);
    QString lightId = m_lightResourceByDevice.value(deviceExternalId);
    if (lightId.isEmpty()) {
        QString refreshError;
        pollBridge(&refreshError);
        lightId = m_lightResourceByDevice.value(deviceExternalId);
    }
    if (lightId.isEmpty()) {
        return failureResponse(request.cmdId,
                               CmdStatus::InvalidArgument,
                               QStringLiteral("No Hue light resource for device"));
    }

    const DeviceEntry *deviceEntry = nullptr;
    auto deviceIt = m_devices.constFind(deviceExternalId);
    if (deviceIt != m_devices.cend())
        deviceEntry = &deviceIt.value();

    const QString effectId = QString::fromStdString(request.effectId).trimmed();
    const v1::DeviceEffect requestedEffect = request.effect;

    const v1::DeviceEffectDescriptor *descriptor = nullptr;
    if (deviceEntry) {
        for (const v1::DeviceEffectDescriptor &desc : deviceEntry->device.effects) {
            if (!effectId.isEmpty() && desc.id == effectId.toStdString()) {
                descriptor = &desc;
                break;
            }
            if (!descriptor
                && requestedEffect != v1::DeviceEffect::None
                && desc.effect == requestedEffect) {
                descriptor = &desc;
            }
        }
    }

    QString hueEffectName = descriptor ? effectMetaValue(*descriptor, QStringLiteral("hueEffect")) : QString{};
    if (hueEffectName.isEmpty() && !effectId.isEmpty())
        hueEffectName = effectId;
    if (hueEffectName.isEmpty())
        hueEffectName = hueEffectNameForDeviceEffect(requestedEffect);
    if (hueEffectName.isEmpty()) {
        return failureResponse(request.cmdId,
                               CmdStatus::InvalidArgument,
                               QStringLiteral("Unsupported effect for this device"));
    }

    QString category = descriptor
        ? effectMetaValue(*descriptor, QStringLiteral("hueEffectCategory"))
        : QStringLiteral("effects");
    if (category.isEmpty())
        category = QStringLiteral("effects");

    const QJsonObject paramsObj = parseJsonObject(request.paramsJson);

    QJsonObject payload;
    if (category == QLatin1String("timed_effects")) {
        QJsonObject timed;
        timed.insert(QStringLiteral("effect"), hueEffectName);
        if (paramsObj.contains(QStringLiteral("duration")))
            timed.insert(QStringLiteral("duration"), paramsObj.value(QStringLiteral("duration")));
        payload.insert(QStringLiteral("timed_effects"), timed);
    } else {
        QJsonObject effectsObj;
        effectsObj.insert(QStringLiteral("effect"), hueEffectName);
        payload.insert(QStringLiteral("effects"), effectsObj);
    }

    QString asyncError;
    if (!m_http->putJsonAsync(m_settings,
                             QStringLiteral("/clip/v2/resource/light/%1").arg(lightId),
                             QJsonDocument(payload).toJson(QJsonDocument::Compact),
                             true,
                             &asyncError)) {
        const QString error = asyncError.isEmpty()
            ? QStringLiteral("Hue effect request could not be sent")
            : asyncError;
        return failureResponse(request.cmdId, CmdStatus::TemporarilyOffline, error);
    }

    CmdResponse resp = successResponse(request.cmdId);
    resp.finalValue = hueEffectName.toStdString();
    return resp;
}

void HueAdapterInstance::onSceneInvoke(const sdk::SceneInvokeRequest &request)
{
    submitCmdResult(handleSceneInvoke(request), "scene.invoke");
}

phicore::adapter::v1::CmdResponse HueAdapterInstance::handleSceneInvoke(const sdk::SceneInvokeRequest &request)
{
    if (request.sceneExternalId.empty())
        return failureResponse(request.cmdId, CmdStatus::InvalidArgument, QStringLiteral("sceneExternalId missing"));

    const QString action = QString::fromStdString(request.action).trimmed().toLower();
    QString recallAction = QStringLiteral("active");
    if (action == QLatin1String("deactivate"))
        recallAction = QStringLiteral("inactive");
    else if (action == QLatin1String("dynamic"))
        recallAction = QStringLiteral("dynamic_palette");

    QJsonObject recall;
    recall.insert(QStringLiteral("action"), recallAction);

    const QString groupExternalId = QString::fromStdString(request.groupExternalId).trimmed();
    if (!groupExternalId.isEmpty()) {
        QJsonObject target;
        target.insert(QStringLiteral("rid"), groupExternalId);
        target.insert(QStringLiteral("rtype"), QStringLiteral("zone"));
        recall.insert(QStringLiteral("target"), target);
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("recall"), recall);

    const QString sceneExternalId = QString::fromStdString(request.sceneExternalId);
    const HttpResult result = m_http->putJson(m_settings,
                                             QStringLiteral("/clip/v2/resource/scene/%1").arg(sceneExternalId),
                                             QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                             true,
                                             10000);
    if (!result.ok) {
        QString error = extractHueError(result.payload);
        if (error.isEmpty())
            error = result.error.isEmpty() ? QStringLiteral("Scene invocation failed") : result.error;
        return failureResponse(request.cmdId, CmdStatus::Failure, error);
    }

    return successResponse(request.cmdId);
}

std::int64_t HueAdapterInstance::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void HueAdapterInstance::applyRuntimeConfig(const sdk::ConfigChangedRequest &request)
{
    const v1::Adapter &adapter = request.adapter;
    m_adapterInfo = adapter;

    m_meta = QJsonObject{};
    const QByteArray metaBytes = QByteArray::fromStdString(adapter.metaJson);
    if (!metaBytes.trimmed().isEmpty()) {
        const QJsonDocument metaDoc = QJsonDocument::fromJson(metaBytes);
        if (metaDoc.isObject())
            m_meta = metaDoc.object();
    }

    m_settings.host = QString::fromStdString(adapter.host).trimmed();
    m_settings.ip = QString::fromStdString(adapter.ip).trimmed();
    m_settings.port = static_cast<int>(adapter.port);
    m_settings.appKey = QString::fromStdString(adapter.token).trimmed();

    if (m_meta.contains(QStringLiteral("host")))
        m_settings.host = m_meta.value(QStringLiteral("host")).toString().trimmed();
    if (m_meta.contains(QStringLiteral("ip")))
        m_settings.ip = m_meta.value(QStringLiteral("ip")).toString().trimmed();
    if (m_meta.contains(QStringLiteral("port")))
        m_settings.port = m_meta.value(QStringLiteral("port")).toInt(m_settings.port);
    if (m_meta.contains(QStringLiteral("appKey")))
        m_settings.appKey = m_meta.value(QStringLiteral("appKey")).toString().trimmed();

    if (m_meta.contains(QStringLiteral("useTls"))) {
        m_settings.useTls = m_meta.value(QStringLiteral("useTls")).toBool(true);
    } else {
        const bool flagTls = v1::hasFlag(adapter.flags, v1::AdapterFlag::UseTls);
        if (flagTls)
            m_settings.useTls = true;
        else if (m_settings.port > 0)
            m_settings.useTls = (m_settings.port == 443);
        else
            m_settings.useTls = true;
    }

    if (m_settings.port <= 0)
        m_settings.port = m_settings.useTls ? 443 : 80;

    readIntervalsFromMeta();
}

void HueAdapterInstance::readIntervalsFromMeta()
{
    m_pollIntervalMs = std::clamp(readInt(m_meta, QStringLiteral("pollIntervalMs"), 5000), 1000, 600000);
    m_retryIntervalMs = std::clamp(readInt(m_meta, QStringLiteral("retryIntervalMs"), 10000), 1000, 600000);
}

void HueAdapterInstance::startEventStream()
{
    if (!m_runtimeConfigured || m_eventStreamReply)
        return;

    const QString host = HttpClient::effectiveHost(m_settings);
    if (host.isEmpty() || m_settings.appKey.trimmed().isEmpty()) {
        m_nextEventStreamRetryDueMs = nowMs() + std::max(1000, m_retryIntervalMs);
        return;
    }

    const bool useTls = m_settings.useTls;
    const int defaultPort = useTls ? 443 : 80;
    const int port = m_settings.port > 0 ? m_settings.port : defaultPort;

    QUrl url;
    url.setScheme(useTls ? QStringLiteral("https") : QStringLiteral("http"));
    url.setHost(host);
    url.setPort(port);
    url.setPath(QStringLiteral("/eventstream/clip/v2"));

    QNetworkRequest request(url);
    request.setRawHeader("Accept", "text/event-stream");
    request.setRawHeader("hue-application-key", m_settings.appKey.toUtf8());
    request.setRawHeader("User-Agent", "phi-adapter-hue-ipc/1.0");

#if QT_CONFIG(ssl)
    if (useTls) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(ssl);
    }
#endif

    m_eventStreamReply = m_eventStreamNetwork->get(request);
    if (!m_eventStreamReply) {
        m_nextEventStreamRetryDueMs = nowMs() + std::max(1000, m_retryIntervalMs);
    }
}

void HueAdapterInstance::stopEventStream()
{
    if (!m_eventStreamReply)
        return;

    m_eventStreamReply->abort();
    m_eventStreamReply->deleteLater();
    m_eventStreamReply = nullptr;
    m_eventStreamLineBuffer.clear();
    m_eventStreamDataBuffer.clear();
    m_eventStreamActive = false;
}

void HueAdapterInstance::pumpEventStream(std::int64_t now)
{
    if (!m_eventStreamReply)
        return;

    const QByteArray chunk = m_eventStreamReply->readAll();
    if (!chunk.isEmpty()) {
        setConnectionState(true);
        m_eventStreamActive = true;
        m_eventStreamRetryCount = 0;
        m_nextEventStreamRetryDueMs = now + std::max(1000, m_retryIntervalMs);
        m_eventStreamLineBuffer.append(chunk);

        while (true) {
            const int newline = m_eventStreamLineBuffer.indexOf('\n');
            if (newline < 0)
                break;

            QByteArray line = m_eventStreamLineBuffer.left(newline);
            m_eventStreamLineBuffer.remove(0, newline + 1);
            if (!line.isEmpty() && line.endsWith('\r'))
                line.chop(1);

            if (line.isEmpty()) {
                if (!m_eventStreamDataBuffer.isEmpty()) {
                    processEventStreamPayload(m_eventStreamDataBuffer, now);
                    m_eventStreamDataBuffer.clear();
                }
                continue;
            }

            if (line.startsWith("data:")) {
                QByteArray payload = line.mid(5);
                if (!payload.isEmpty() && payload.at(0) == ' ')
                    payload.remove(0, 1);
                if (!m_eventStreamDataBuffer.isEmpty())
                    m_eventStreamDataBuffer.append('\n');
                m_eventStreamDataBuffer.append(payload);
            }
        }
    }

    if (!m_eventStreamReply->isFinished())
        return;

    const bool hasError = m_eventStreamReply->error() != QNetworkReply::NoError;
    if (hasError) {
        std::cerr << "hue-ipc eventstream error: "
                  << m_eventStreamReply->errorString().toStdString()
                  << '\n';
    } else {
        std::cerr << "hue-ipc eventstream finished" << '\n';
    }

    m_eventStreamReply->deleteLater();
    m_eventStreamReply = nullptr;
    m_eventStreamLineBuffer.clear();
    m_eventStreamDataBuffer.clear();
    m_eventStreamActive = false;

    if (hasError)
        setConnectionState(false);

    int retryDelayMs = m_retryIntervalMs;
    if (m_eventStreamRetryCount < kEventStreamFastRetryAttempts) {
        ++m_eventStreamRetryCount;
        retryDelayMs = kEventStreamFastRetryMs;
    }
    m_nextEventStreamRetryDueMs = now + std::max(1000, retryDelayMs);
}

void HueAdapterInstance::processEventStreamPayload(const QByteArray &jsonData, std::int64_t nowMs)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonData, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return;

    if (doc.isArray()) {
        for (const QJsonValue &entry : doc.array()) {
            if (!entry.isObject())
                continue;
            processEventStreamEventObject(entry.toObject(), nowMs);
        }
        return;
    }

    if (doc.isObject())
        processEventStreamEventObject(doc.object(), nowMs);
}

void HueAdapterInstance::processEventStreamEventObject(const QJsonObject &eventObj, std::int64_t nowMs)
{
    const QString eventType = eventObj.value(QStringLiteral("type")).toString();
    if (eventType == QLatin1String("delete")) {
        m_nextPollDueMs = 0;
        return;
    }

    const QJsonArray dataArray = eventObj.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &entry : dataArray) {
        if (!entry.isObject())
            continue;
        const QJsonObject resourceObj = entry.toObject();
        const QString resourceType = resourceObj.value(QStringLiteral("type")).toString();

        if (resourceType == QLatin1String("relative_rotary")) {
            handleRelativeRotaryEvent(resourceObj, nowMs);
            continue;
        }
        if (resourceType == QLatin1String("button")) {
            handleButtonEvent(resourceObj, nowMs);
            continue;
        }
        if (resourceType == QLatin1String("zigbee_connectivity")) {
            const QString deviceExternalId = deviceExternalIdFromResource(resourceObj);
            if (deviceExternalId.isEmpty())
                continue;
            const std::optional<std::int64_t> status = parseConnectivityStatus(resourceObj);
            if (!status.has_value())
                continue;
            v1::Utf8String sendError;
            sendChannelStateUpdated(deviceExternalId.toStdString(),
                                    "zigbee_status",
                                    *status,
                                    nowMs,
                                    &sendError);
            continue;
        }

        if (resourceType == QLatin1String("light")
            || resourceType == QLatin1String("motion")
            || resourceType == QLatin1String("tamper")
            || resourceType == QLatin1String("temperature")
            || resourceType == QLatin1String("light_level")
            || resourceType == QLatin1String("device_power")
            || resourceType == QLatin1String("scene")
            || resourceType == QLatin1String("room")
            || resourceType == QLatin1String("zone")
            || resourceType == QLatin1String("device")) {
            m_nextPollDueMs = 0;
        }
    }
}

QString HueAdapterInstance::deviceExternalIdFromResource(const QJsonObject &resourceObj) const
{
    const QJsonObject ownerObj = resourceObj.value(QStringLiteral("owner")).toObject();
    if (ownerObj.value(QStringLiteral("rtype")).toString() != QLatin1String("device"))
        return {};
    return ownerObj.value(QStringLiteral("rid")).toString().trimmed();
}

QString HueAdapterInstance::resolveButtonChannel(const QString &deviceExternalId,
                                         const QString &buttonResourceId,
                                         const QJsonObject &resourceObj) const
{
    QString channelExternalId = m_buttonResourceToChannel.value(buttonResourceId);
    const auto deviceIt = m_devices.constFind(deviceExternalId);
    const v1::ChannelList *channels = (deviceIt != m_devices.cend()) ? &deviceIt->channels : nullptr;

    const QJsonObject metadataObj = resourceObj.value(QStringLiteral("metadata")).toObject();
    const int controlId = metadataObj.value(QStringLiteral("control_id")).toInt(0);

    if (channelExternalId.isEmpty() && controlId > 0 && channels) {
        const QString candidate = QStringLiteral("button%1").arg(controlId);
        for (const v1::Channel &channel : *channels) {
            if (QString::fromStdString(channel.externalId) == candidate) {
                channelExternalId = candidate;
                break;
            }
        }
    }

    if (channelExternalId.isEmpty() && channels) {
        QString firstButtonN;
        for (const v1::Channel &channel : *channels) {
            const QString id = QString::fromStdString(channel.externalId);
            if (id == QLatin1String("button")) {
                channelExternalId = id;
                break;
            }
            if (firstButtonN.isEmpty() && id.startsWith(QStringLiteral("button")))
                firstButtonN = id;
        }
        if (channelExternalId.isEmpty() && !firstButtonN.isEmpty())
            channelExternalId = firstButtonN;
    }

    if (channelExternalId.isEmpty())
        channelExternalId = QStringLiteral("button");
    return channelExternalId;
}

void HueAdapterInstance::handleRelativeRotaryEvent(const QJsonObject &resourceObj, std::int64_t nowMs)
{
    const QString deviceExternalId = deviceExternalIdFromResource(resourceObj);
    if (deviceExternalId.isEmpty())
        return;

    const QJsonObject rrObj = resourceObj.value(QStringLiteral("relative_rotary")).toObject();
    const QJsonObject lastEventObj = rrObj.value(QStringLiteral("last_event")).toObject();
    const QJsonObject reportObj = rrObj.value(QStringLiteral("rotary_report")).toObject();

    QJsonObject rotationObj = lastEventObj.value(QStringLiteral("rotation")).toObject();
    if (!rotationObj.contains(QStringLiteral("steps")))
        rotationObj = reportObj.value(QStringLiteral("rotation")).toObject();
    if (!rotationObj.contains(QStringLiteral("steps")))
        return;

    int steps = rotationObj.value(QStringLiteral("steps")).toInt(0);
    if (steps == 0)
        return;

    const QString direction = rotationObj.value(QStringLiteral("direction")).toString().trimmed().toLower();
    if (direction == QLatin1String("counter_clock_wise")
        || direction == QLatin1String("counter_clockwise")
        || direction == QLatin1String("ccw")) {
        steps = -std::abs(steps);
    } else if (direction == QLatin1String("clock_wise")
               || direction == QLatin1String("clockwise")
               || direction == QLatin1String("cw")) {
        steps = std::abs(steps);
    } else {
        return;
    }

    std::int64_t eventTs = nowMs;
    const std::int64_t reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("updated")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    v1::Utf8String sendError;
    sendChannelStateUpdated(deviceExternalId.toStdString(),
                            "dial",
                            static_cast<std::int64_t>(steps),
                            eventTs,
                            &sendError);
    m_lastDialValueByDevice.insert(deviceExternalId, steps);
    m_dialResetDueMs.insert(deviceExternalId, nowMs + kDialResetDelayMs);
}

void HueAdapterInstance::handleButtonEvent(const QJsonObject &resourceObj, std::int64_t nowMs)
{
    const QString deviceExternalId = deviceExternalIdFromResource(resourceObj);
    if (deviceExternalId.isEmpty())
        return;

    const QString buttonResourceId = resourceObj.value(QStringLiteral("id")).toString();
    const QJsonObject buttonObj = resourceObj.value(QStringLiteral("button")).toObject();
    QString eventName = buttonObj.value(QStringLiteral("last_event")).toString();
    if (eventName.isEmpty()) {
        const QJsonObject reportObj = buttonObj.value(QStringLiteral("button_report")).toObject();
        eventName = reportObj.value(QStringLiteral("event")).toString();
    }
    if (eventName.isEmpty())
        return;

    std::int64_t eventTs = nowMs;
    const QJsonObject reportObj = buttonObj.value(QStringLiteral("button_report")).toObject();
    const std::int64_t reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("updated")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    const v1::ButtonEventCode code = mapHueButtonEvent(eventName);
    if (code == v1::ButtonEventCode::None)
        return;

    const QString channelExternalId = resolveButtonChannel(deviceExternalId, buttonResourceId, resourceObj);
    if (!buttonResourceId.isEmpty())
        m_buttonResourceToChannel.insert(buttonResourceId, channelExternalId);

    const QString bindingKey = channelBindingKey(deviceExternalId, channelExternalId);

    if (code == v1::ButtonEventCode::ShortPressRelease) {
        ButtonMultiPressTracker &tracker = m_buttonMultiPress[bindingKey];
        tracker.deviceExternalId = deviceExternalId;
        tracker.channelExternalId = channelExternalId;
        tracker.lastEventTs = eventTs;
        tracker.lastSeenMs = nowMs;
        tracker.count += 1;
        tracker.dueMs = nowMs + kButtonMultiPressWindowMs;
        m_buttonLastEventCode.remove(bindingKey);
        m_buttonLastEventTs.remove(bindingKey);
        return;
    }

    v1::Utf8String sendError;
    if (code == v1::ButtonEventCode::Repeat) {
        const int prevCode = m_buttonLastEventCode.value(bindingKey, 0);
        const std::int64_t prevTs = m_buttonLastEventTs.value(bindingKey, 0);
        const bool hasRecentLongState =
            (prevCode == static_cast<int>(v1::ButtonEventCode::LongPress)
             || prevCode == static_cast<int>(v1::ButtonEventCode::Repeat))
            && prevTs > 0
            && (eventTs - prevTs) <= kButtonLongPressRepeatWindowMs;
        if (!hasRecentLongState) {
            sendChannelStateUpdated(deviceExternalId.toStdString(),
                                    channelExternalId.toStdString(),
                                    static_cast<std::int64_t>(v1::ButtonEventCode::LongPress),
                                    eventTs,
                                    &sendError);
            m_buttonLastEventCode.insert(bindingKey, static_cast<int>(v1::ButtonEventCode::LongPress));
            m_buttonLastEventTs.insert(bindingKey, eventTs);
        }
    }

    sendChannelStateUpdated(deviceExternalId.toStdString(),
                            channelExternalId.toStdString(),
                            static_cast<std::int64_t>(code),
                            eventTs,
                            &sendError);

    if (code == v1::ButtonEventCode::LongPressRelease) {
        m_buttonLastEventCode.remove(bindingKey);
        m_buttonLastEventTs.remove(bindingKey);
    } else {
        m_buttonLastEventCode.insert(bindingKey, static_cast<int>(code));
        m_buttonLastEventTs.insert(bindingKey, eventTs);
    }
}

void HueAdapterInstance::processPendingButtonAggregates(std::int64_t nowMs)
{
    QStringList dueKeys;
    for (auto it = m_buttonMultiPress.cbegin(); it != m_buttonMultiPress.cend(); ++it) {
        if (it->count > 0 && it->dueMs > 0 && nowMs >= it->dueMs)
            dueKeys.push_back(it.key());
    }
    for (const QString &bindingKey : dueKeys)
        finalizePendingShortPress(bindingKey);
}

void HueAdapterInstance::finalizePendingShortPress(const QString &bindingKey)
{
    auto it = m_buttonMultiPress.find(bindingKey);
    if (it == m_buttonMultiPress.end())
        return;

    ButtonMultiPressTracker &tracker = it.value();
    if (tracker.count <= 0) {
        tracker.lastEventTs = 0;
        tracker.lastSeenMs = 0;
        tracker.dueMs = 0;
        return;
    }

    const int count = tracker.count;
    tracker.count = 0;
    const std::int64_t ts = tracker.lastEventTs > 0 ? tracker.lastEventTs : nowMs();
    tracker.lastEventTs = 0;
    tracker.lastSeenMs = 0;
    tracker.dueMs = 0;

    if (tracker.deviceExternalId.isEmpty() || tracker.channelExternalId.isEmpty())
        return;

    v1::ButtonEventCode code = v1::ButtonEventCode::None;
    if (count == 1) {
        code = v1::ButtonEventCode::ShortPressRelease;
    } else if (count == 2) {
        code = v1::ButtonEventCode::DoublePress;
    } else if (count == 3) {
        code = v1::ButtonEventCode::TriplePress;
    } else if (count == 4) {
        code = v1::ButtonEventCode::QuadruplePress;
    } else {
        code = v1::ButtonEventCode::QuintuplePress;
    }

    v1::Utf8String sendError;
    sendChannelStateUpdated(tracker.deviceExternalId.toStdString(),
                            tracker.channelExternalId.toStdString(),
                            static_cast<std::int64_t>(code),
                            ts,
                            &sendError);
}

void HueAdapterInstance::processPendingDialResets(std::int64_t nowMs)
{
    QStringList dueDevices;
    for (auto it = m_dialResetDueMs.cbegin(); it != m_dialResetDueMs.cend(); ++it) {
        if (nowMs >= it.value())
            dueDevices.push_back(it.key());
    }

    for (const QString &deviceExternalId : dueDevices) {
        if (m_lastDialValueByDevice.value(deviceExternalId, 0) == 0) {
            m_dialResetDueMs.remove(deviceExternalId);
            continue;
        }
        v1::Utf8String sendError;
        sendChannelStateUpdated(deviceExternalId.toStdString(), "dial", static_cast<std::int64_t>(0), nowMs, &sendError);
        m_lastDialValueByDevice.insert(deviceExternalId, 0);
        m_dialResetDueMs.remove(deviceExternalId);
    }
}

void HueAdapterInstance::rebuildButtonResourceMap(const QJsonArray &buttonData)
{
    struct ButtonResource {
        QString resourceId;
        int controlId = 1;
    };

    QHash<QString, std::vector<ButtonResource>> byDevice;
    for (const QJsonValue &entry : buttonData) {
        if (!entry.isObject())
            continue;
        const QJsonObject obj = entry.toObject();
        const QString deviceExternalId = deviceExternalIdFromResource(obj);
        if (deviceExternalId.isEmpty())
            continue;

        ButtonResource resource;
        resource.resourceId = obj.value(QStringLiteral("id")).toString().trimmed();
        const QJsonObject metadataObj = obj.value(QStringLiteral("metadata")).toObject();
        const int controlId = metadataObj.value(QStringLiteral("control_id")).toInt(0);
        resource.controlId = controlId > 0 ? controlId : 1;
        byDevice[deviceExternalId].push_back(resource);
    }

    m_buttonResourceToChannel.clear();
    for (auto it = byDevice.cbegin(); it != byDevice.cend(); ++it) {
        const bool singleButton = it->size() <= 1;
        for (const ButtonResource &resource : it.value()) {
            if (resource.resourceId.isEmpty())
                continue;
            const QString channelId = singleButton
                ? QStringLiteral("button")
                : QStringLiteral("button%1").arg(resource.controlId);
            m_buttonResourceToChannel.insert(resource.resourceId, channelId);
        }
    }
}

bool HueAdapterInstance::pollBridge(QString *error)
{
    if (HttpClient::effectiveHost(m_settings).isEmpty()) {
        if (error)
            *error = QStringLiteral("Bridge host is empty");
        return false;
    }
    if (m_settings.appKey.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("Hue application key missing");
        return false;
    }

    QJsonArray deviceData;
    QJsonArray lightData;
    QJsonArray motionData;
    QJsonArray tamperData;
    QJsonArray temperatureData;
    QJsonArray lightLevelData;
    QJsonArray devicePowerData;
    QJsonArray buttonData;
    QJsonArray relativeRotaryData;
    QJsonArray zigbeeConnectivityData;
    QJsonArray roomData;
    QJsonArray zoneData;
    QJsonArray sceneData;

    if (!fetchResourceArray(QStringLiteral("device"), &deviceData, error))
        return false;
    if (!fetchResourceArray(QStringLiteral("light"), &lightData, error))
        return false;
    if (!fetchResourceArray(QStringLiteral("motion"), &motionData, nullptr))
        motionData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("tamper"), &tamperData, nullptr))
        tamperData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("temperature"), &temperatureData, nullptr))
        temperatureData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("light_level"), &lightLevelData, nullptr))
        lightLevelData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("device_power"), &devicePowerData, nullptr))
        devicePowerData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("button"), &buttonData, nullptr))
        buttonData = QJsonArray{};
    rebuildButtonResourceMap(buttonData);
    if (!fetchResourceArray(QStringLiteral("relative_rotary"), &relativeRotaryData, nullptr))
        relativeRotaryData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("zigbee_connectivity"), &zigbeeConnectivityData, nullptr))
        zigbeeConnectivityData = QJsonArray{};
    if (!fetchResourceArray(QStringLiteral("room"), &roomData, error))
        return false;
    if (!fetchResourceArray(QStringLiteral("zone"), &zoneData, error))
        return false;
    if (!fetchResourceArray(QStringLiteral("scene"), &sceneData, error))
        return false;

    QJsonArray discoveryData;
    QString discoveryError;
    if (fetchResourceArray(QStringLiteral("zigbee_device_discovery"), &discoveryData, &discoveryError)) {
        m_discoveryResourceId.clear();
        for (const QJsonValue &entry : discoveryData) {
            if (!entry.isObject())
                continue;
            const QString id = entry.toObject().value(QStringLiteral("id")).toString().trimmed();
            if (!id.isEmpty()) {
                m_discoveryResourceId = id;
                break;
            }
        }
    }

    const Snapshot snapshot = buildSnapshot(deviceData,
                                            lightData,
                                            motionData,
                                            tamperData,
                                            temperatureData,
                                            lightLevelData,
                                            devicePowerData,
                                            buttonData,
                                            relativeRotaryData,
                                            zigbeeConnectivityData,
                                            roomData,
                                            zoneData,
                                            sceneData);
    if (!publishSnapshot(snapshot, error))
        return false;

    setConnectionState(true);
    v1::Utf8String sendError;
    sendFullSyncCompleted(&sendError);
    return true;
}

bool HueAdapterInstance::fetchResourceArray(const QString &resourceType, QJsonArray *outData, QString *error)
{
    if (!outData)
        return false;

    const HttpResult result = m_http->get(m_settings,
                                         QStringLiteral("/clip/v2/resource/%1").arg(resourceType),
                                         true,
                                         QByteArrayLiteral("application/json"),
                                         10000);
    if (!result.ok) {
        QString message = extractHueError(result.payload);
        if (message.isEmpty())
            message = result.error;
        if (message.isEmpty())
            message = QStringLiteral("Failed to fetch Hue resource %1").arg(resourceType);
        if (error)
            *error = message;
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(result.payload);
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("Hue %1 response is not a JSON object").arg(resourceType);
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonValue data = root.value(QStringLiteral("data"));
    if (!data.isArray()) {
        if (error)
            *error = QStringLiteral("Hue %1 response has no data array").arg(resourceType);
        return false;
    }

    *outData = data.toArray();
    return true;
}

bool HueAdapterInstance::publishSnapshot(const Snapshot &snapshot, QString *error)
{
    v1::Utf8String sendError;

    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        if (snapshot.devices.contains(it.key()))
            continue;
        if (!sendDeviceRemoved(it.key().toStdString(), &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
        m_lightResourceByDevice.remove(it.key());
    }

    const std::int64_t ts = nowMs();

    QHash<QString, QString> nextLightByDevice;
    for (auto it = snapshot.devices.cbegin(); it != snapshot.devices.cend(); ++it) {
        const QString deviceExternalId = it.key();
        const DeviceEntry &entry = it.value();

        if (!sendDeviceUpdated(entry.device, entry.channels, &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }

        for (const v1::Channel &channel : entry.channels) {
            if (!channel.hasValue)
                continue;
            if (!sendChannelStateUpdated(entry.device.externalId,
                                         channel.externalId,
                                         channel.lastValue,
                                         ts,
                                         &sendError)) {
                if (error)
                    *error = QString::fromStdString(sendError);
                return false;
            }
        }

        if (!entry.state.lightResourceId.isEmpty())
            nextLightByDevice.insert(deviceExternalId, entry.state.lightResourceId);
    }

    QSet<QString> nextRooms;
    for (const v1::Room &room : snapshot.rooms) {
        const QString roomId = QString::fromStdString(room.externalId);
        if (roomId.isEmpty())
            continue;
        nextRooms.insert(roomId);
        if (!sendRoomUpdated(room, &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
    }
    for (const QString &oldRoom : std::as_const(m_knownRooms)) {
        if (nextRooms.contains(oldRoom))
            continue;
        if (!sendRoomRemoved(oldRoom.toStdString(), &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
    }

    QSet<QString> nextGroups;
    for (const v1::Group &group : snapshot.groups) {
        const QString groupId = QString::fromStdString(group.externalId);
        if (groupId.isEmpty())
            continue;
        nextGroups.insert(groupId);
        if (!sendGroupUpdated(group, &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
    }
    for (const QString &oldGroup : std::as_const(m_knownGroups)) {
        if (nextGroups.contains(oldGroup))
            continue;
        if (!sendGroupRemoved(oldGroup.toStdString(), &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
    }

    QSet<QString> nextScenes;
    for (const v1::Scene &scene : snapshot.scenes) {
        const QString sceneId = QString::fromStdString(scene.externalId);
        if (sceneId.isEmpty())
            continue;
        nextScenes.insert(sceneId);
        if (!sendSceneUpdated(scene, &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
    }
    for (const QString &oldScene : std::as_const(m_knownScenes)) {
        if (nextScenes.contains(oldScene))
            continue;
        if (!sendSceneRemoved(oldScene.toStdString(), &sendError)) {
            if (error)
                *error = QString::fromStdString(sendError);
            return false;
        }
    }

    m_devices = snapshot.devices;
    m_lightResourceByDevice = nextLightByDevice;
    m_knownRooms = nextRooms;
    m_knownGroups = nextGroups;
    m_knownScenes = nextScenes;
    return true;
}

void HueAdapterInstance::setConnectionState(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    v1::Utf8String error;
    if (!sendConnectionStateChanged(connected, &error)) {
        std::cerr << "hue-ipc failed to send connectionStateChanged: " << error << '\n';
    }
}

phicore::adapter::v1::ActionResponse HueAdapterInstance::invokeStartDeviceDiscovery(const sdk::AdapterActionInvokeRequest &request)
{
    ActionResponse response;
    response.id = request.cmdId;
    response.tsMs = nowMs();

    if (m_discoveryResourceId.isEmpty()) {
        response.status = CmdStatus::Failure;
        response.error = "Discovery resource not ready yet";
        return response;
    }

    QJsonObject actionObj;
    actionObj.insert(QStringLiteral("type"), QStringLiteral("search"));
    actionObj.insert(QStringLiteral("action_type"), QStringLiteral("search"));

    QJsonObject payload;
    payload.insert(QStringLiteral("state"), QStringLiteral("start"));
    payload.insert(QStringLiteral("action"), actionObj);

    QString sendError;
    if (!m_http->putJsonAsync(m_settings,
                             QStringLiteral("/clip/v2/resource/zigbee_device_discovery/%1").arg(m_discoveryResourceId),
                             QJsonDocument(payload).toJson(QJsonDocument::Compact),
                             true,
                             &sendError)) {
        const QString message = sendError.isEmpty()
            ? QStringLiteral("Failed to start Hue Zigbee discovery")
            : sendError;
        response.status = CmdStatus::Failure;
        response.error = message.toStdString();
        return response;
    }

    response.status = CmdStatus::Success;
    response.resultType = v1::ActionResultType::String;
    response.resultValue = std::string("Hue Zigbee discovery started");
    return response;
}

void HueAdapterInstance::submitCmdResult(CmdResponse response, const char *context)
{
    v1::Utf8String err;
    if (!sendResult(response, &err))
        std::cerr << "failed to send " << context << " result: " << err << '\n';
}

void HueAdapterInstance::submitActionResult(ActionResponse response, const char *context)
{
    v1::Utf8String err;
    if (!sendResult(response, &err))
        std::cerr << "failed to send " << context << " result: " << err << '\n';
}

phicore::adapter::v1::CmdResponse HueAdapterInstance::failureResponse(std::uint64_t cmdId, CmdStatus status, const QString &error) const
{
    CmdResponse response;
    response.id = cmdId;
    response.status = status;
    response.error = error.toStdString();
    response.tsMs = nowMs();
    return response;
}

phicore::adapter::v1::CmdResponse HueAdapterInstance::successResponse(std::uint64_t cmdId) const
{
    CmdResponse response;
    response.id = cmdId;
    response.status = CmdStatus::Success;
    response.tsMs = nowMs();
    return response;
}

} // namespace phicore::hue::ipc
