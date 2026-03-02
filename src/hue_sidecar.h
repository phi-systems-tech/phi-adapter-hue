#pragma once

#include <cstdint>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QString>

#include "hue_http.h"
#include "hue_model.h"
#include "phi/adapter/sdk/sidecar.h"

namespace phicore::hue::ipc {

class HueSidecar final : public phicore::adapter::sdk::AdapterSidecar
{
public:
    HueSidecar();

    void tick();

protected:
    void onConnected() override;
    void onDisconnected() override;
    void onBootstrap(const phicore::adapter::sdk::BootstrapRequest &request) override;
    void onConfigChanged(const phicore::adapter::sdk::ConfigChangedRequest &request) override;

    phicore::adapter::v1::CmdResponse onChannelInvoke(
        const phicore::adapter::sdk::ChannelInvokeRequest &request) override;
    phicore::adapter::v1::ActionResponse onAdapterActionInvoke(
        const phicore::adapter::sdk::AdapterActionInvokeRequest &request) override;
    phicore::adapter::v1::CmdResponse onDeviceNameUpdate(
        const phicore::adapter::sdk::DeviceNameUpdateRequest &request) override;
    phicore::adapter::v1::CmdResponse onDeviceEffectInvoke(
        const phicore::adapter::sdk::DeviceEffectInvokeRequest &request) override;
    phicore::adapter::v1::CmdResponse onSceneInvoke(
        const phicore::adapter::sdk::SceneInvokeRequest &request) override;

    phicore::adapter::v1::Utf8String displayName() const override;
    phicore::adapter::v1::Utf8String description() const override;
    phicore::adapter::v1::Utf8String iconSvg() const override;
    phicore::adapter::v1::Utf8String apiVersion() const override;
    int timeoutMs() const override;
    phicore::adapter::v1::AdapterCapabilities capabilities() const override;
    phicore::adapter::v1::JsonText configSchemaJson() const override;

private:
    using CmdResponse = phicore::adapter::v1::CmdResponse;
    using ActionResponse = phicore::adapter::v1::ActionResponse;
    using CmdStatus = phicore::adapter::v1::CmdStatus;

    static std::int64_t nowMs();

    void applyRuntimeConfig(const phicore::adapter::sdk::ConfigChangedRequest &request);
    void readIntervalsFromMeta();

    bool pollBridge(QString *error = nullptr);
    bool fetchResourceArray(const QString &resourceType, QJsonArray *outData, QString *error = nullptr);
    bool publishSnapshot(const Snapshot &snapshot, QString *error = nullptr);
    void setConnectionState(bool connected);
    void startEventStream();
    void stopEventStream();
    void pumpEventStream(std::int64_t nowMs);
    void processEventStreamPayload(const QByteArray &jsonData, std::int64_t nowMs);
    void processEventStreamEventObject(const QJsonObject &eventObj, std::int64_t nowMs);
    void handleRelativeRotaryEvent(const QJsonObject &resourceObj, std::int64_t nowMs);
    void handleButtonEvent(const QJsonObject &resourceObj, std::int64_t nowMs);
    void processPendingButtonAggregates(std::int64_t nowMs);
    void finalizePendingShortPress(const QString &bindingKey);
    void processPendingDialResets(std::int64_t nowMs);
    QString deviceExternalIdFromResource(const QJsonObject &resourceObj) const;
    QString resolveButtonChannel(const QString &deviceExternalId,
                                 const QString &buttonResourceId,
                                 const QJsonObject &resourceObj) const;
    void rebuildButtonResourceMap(const QJsonArray &buttonData);

    ActionResponse invokeProbe(const phicore::adapter::sdk::AdapterActionInvokeRequest &request);
    ActionResponse invokeStartDeviceDiscovery(const phicore::adapter::sdk::AdapterActionInvokeRequest &request);

    CmdResponse failureResponse(std::uint64_t cmdId, CmdStatus status, const QString &error) const;
    CmdResponse successResponse(std::uint64_t cmdId) const;

    QNetworkAccessManager m_requestNetwork;
    QNetworkAccessManager m_eventStreamNetwork;
    HttpClient m_http;

    phicore::adapter::v1::Adapter m_adapterInfo;
    ConnectionSettings m_settings;
    QJsonObject m_meta;

    bool m_connected = false;
    bool m_runtimeConfigured = false;
    bool m_eventStreamActive = false;

    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    std::int64_t m_nextPollDueMs = 0;
    std::int64_t m_nextEventStreamRetryDueMs = 0;
    int m_eventStreamRetryCount = 0;
    QNetworkReply *m_eventStreamReply = nullptr;
    QByteArray m_eventStreamLineBuffer;
    QByteArray m_eventStreamDataBuffer;

    struct ButtonMultiPressTracker {
        int count = 0;
        std::int64_t lastEventTs = 0;
        std::int64_t lastSeenMs = 0;
        std::int64_t dueMs = 0;
        QString deviceExternalId;
        QString channelExternalId;
    };

    QHash<QString, DeviceEntry> m_devices;
    QHash<QString, QString> m_lightResourceByDevice;
    QHash<QString, QString> m_buttonResourceToChannel;
    QHash<QString, ButtonMultiPressTracker> m_buttonMultiPress;
    QHash<QString, int> m_buttonLastEventCode;
    QHash<QString, std::int64_t> m_buttonLastEventTs;
    QHash<QString, int> m_lastDialValueByDevice;
    QHash<QString, std::int64_t> m_dialResetDueMs;
    QString m_discoveryResourceId;
    QSet<QString> m_knownRooms;
    QSet<QString> m_knownGroups;
};

} // namespace phicore::hue::ipc
