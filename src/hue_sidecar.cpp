#include "hue_sidecar.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "hue_probe.h"
#include "hue_schema.h"

namespace phicore::hue::ipc {

namespace {

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

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

} // namespace

HueSidecar::HueSidecar()
    : m_http(&m_network)
{
}

void HueSidecar::tick()
{
    if (!m_hasBootstrap)
        return;

    const std::int64_t now = nowMs();
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

    m_nextPollDueMs = now + std::max(1000, m_pollIntervalMs);
}

void HueSidecar::onConnected()
{
    std::cerr << "hue-ipc connected" << '\n';
}

void HueSidecar::onDisconnected()
{
    setConnectionState(false);
    std::cerr << "hue-ipc disconnected" << '\n';
}

void HueSidecar::onBootstrap(const sdk::BootstrapRequest &request)
{
    AdapterSidecar::onBootstrap(request);
    applyBootstrapAdapter(request.adapter);
    m_hasBootstrap = true;
    m_nextPollDueMs = 0;

    std::cerr << "hue-ipc bootstrap adapterId=" << request.adapterId
              << " externalId=" << request.adapter.externalId
              << " host=" << m_settings.host.toStdString()
              << " port=" << m_settings.port
              << " useTls=" << (m_settings.useTls ? "true" : "false")
              << '\n';
}

phicore::adapter::v1::CmdResponse HueSidecar::onChannelInvoke(const sdk::ChannelInvokeRequest &request)
{
    if (!m_hasBootstrap)
        return failureResponse(request.cmdId, CmdStatus::TemporarilyOffline, QStringLiteral("Adapter not bootstrapped"));

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
    if (!m_http.putJsonAsync(m_settings,
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

phicore::adapter::v1::ActionResponse HueSidecar::onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request)
{
    const QString actionId = QString::fromStdString(request.actionId);
    if (actionId == QLatin1String("probe"))
        return invokeProbe(request);
    if (actionId == QLatin1String("startDeviceDiscovery"))
        return invokeStartDeviceDiscovery(request);

    ActionResponse resp;
    resp.id = request.cmdId;
    resp.status = CmdStatus::NotImplemented;
    resp.error = "Unsupported adapter action";
    resp.tsMs = nowMs();
    return resp;
}

phicore::adapter::v1::CmdResponse HueSidecar::onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request)
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

    const HttpResult result = m_http.putJson(m_settings,
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

phicore::adapter::v1::CmdResponse HueSidecar::onSceneInvoke(const sdk::SceneInvokeRequest &request)
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
    const HttpResult result = m_http.putJson(m_settings,
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

phicore::adapter::v1::Utf8String HueSidecar::displayName() const
{
    return phicore::hue::ipc::displayName();
}

phicore::adapter::v1::Utf8String HueSidecar::description() const
{
    return phicore::hue::ipc::description();
}

phicore::adapter::v1::Utf8String HueSidecar::iconSvg() const
{
    return phicore::hue::ipc::iconSvg();
}

phicore::adapter::v1::Utf8String HueSidecar::apiVersion() const
{
    return "1.0.0";
}

int HueSidecar::timeoutMs() const
{
    return 10000;
}

phicore::adapter::v1::AdapterCapabilities HueSidecar::capabilities() const
{
    return phicore::hue::ipc::capabilities();
}

phicore::adapter::v1::JsonText HueSidecar::configSchemaJson() const
{
    return phicore::hue::ipc::configSchemaJson();
}

std::int64_t HueSidecar::nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void HueSidecar::applyBootstrapAdapter(const v1::Adapter &adapter)
{
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

void HueSidecar::readIntervalsFromMeta()
{
    m_pollIntervalMs = std::clamp(readInt(m_meta, QStringLiteral("pollIntervalMs"), 5000), 1000, 600000);
    m_retryIntervalMs = std::clamp(readInt(m_meta, QStringLiteral("retryIntervalMs"), 10000), 1000, 600000);
}

bool HueSidecar::pollBridge(QString *error)
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

bool HueSidecar::fetchResourceArray(const QString &resourceType, QJsonArray *outData, QString *error)
{
    if (!outData)
        return false;

    const HttpResult result = m_http.get(m_settings,
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

bool HueSidecar::publishSnapshot(const Snapshot &snapshot, QString *error)
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

    if (!sendScenesUpdated(snapshot.scenes, &sendError)) {
        if (error)
            *error = QString::fromStdString(sendError);
        return false;
    }

    m_devices = snapshot.devices;
    m_lightResourceByDevice = nextLightByDevice;
    m_knownRooms = nextRooms;
    m_knownGroups = nextGroups;
    return true;
}

void HueSidecar::setConnectionState(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    v1::Utf8String error;
    if (!sendConnectionStateChanged(connected, &error)) {
        std::cerr << "hue-ipc failed to send connectionStateChanged: " << error << '\n';
    }
}

phicore::adapter::v1::ActionResponse HueSidecar::invokeProbe(const sdk::AdapterActionInvokeRequest &request)
{
    ActionResponse response;
    response.id = request.cmdId;
    response.tsMs = nowMs();

    ConnectionSettings settings = m_settings;
    if (!request.paramsJson.empty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(request.paramsJson));
        if (doc.isObject()) {
            const QJsonObject params = doc.object();
            if (params.contains(QStringLiteral("host")))
                settings.host = params.value(QStringLiteral("host")).toString().trimmed();
            if (params.contains(QStringLiteral("ip")))
                settings.ip = params.value(QStringLiteral("ip")).toString().trimmed();
            if (params.contains(QStringLiteral("port")))
                settings.port = params.value(QStringLiteral("port")).toInt(settings.port);
            if (params.contains(QStringLiteral("useTls")))
                settings.useTls = params.value(QStringLiteral("useTls")).toBool(settings.useTls);
            if (params.contains(QStringLiteral("appKey")))
                settings.appKey = params.value(QStringLiteral("appKey")).toString().trimmed();
        }
    }

    const ProbeResult probe = runProbe(m_http, settings, 10000);
    if (!probe.ok) {
        response.status = CmdStatus::Failure;
        response.error = probe.error.toStdString();
        response.resultType = v1::ActionResultType::None;
        return response;
    }

    if (!probe.metaPatch.isEmpty()) {
        v1::Utf8String sendError;
        const QByteArray patch = QJsonDocument(probe.metaPatch).toJson(QJsonDocument::Compact);
        if (!sendAdapterMetaUpdated(patch.toStdString(), &sendError)) {
            std::cerr << "hue-ipc failed to send adapterMetaUpdated(probe): " << sendError << '\n';
        }
    }

    response.status = CmdStatus::Success;
    if (!probe.appKey.isEmpty()) {
        response.resultType = v1::ActionResultType::String;
        response.resultValue = probe.appKey.toStdString();
    } else if (!probe.message.isEmpty()) {
        response.resultType = v1::ActionResultType::String;
        response.resultValue = probe.message.toStdString();
    } else {
        response.resultType = v1::ActionResultType::None;
    }

    return response;
}

phicore::adapter::v1::ActionResponse HueSidecar::invokeStartDeviceDiscovery(const sdk::AdapterActionInvokeRequest &request)
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
    if (!m_http.putJsonAsync(m_settings,
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

phicore::adapter::v1::CmdResponse HueSidecar::failureResponse(std::uint64_t cmdId, CmdStatus status, const QString &error) const
{
    CmdResponse response;
    response.id = cmdId;
    response.status = status;
    response.error = error.toStdString();
    response.tsMs = nowMs();
    return response;
}

phicore::adapter::v1::CmdResponse HueSidecar::successResponse(std::uint64_t cmdId) const
{
    CmdResponse response;
    response.id = cmdId;
    response.status = CmdStatus::Success;
    response.tsMs = nowMs();
    return response;
}

} // namespace phicore::hue::ipc
