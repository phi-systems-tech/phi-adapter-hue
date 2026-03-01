#pragma once

#include <cstdint>

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
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

    phicore::adapter::v1::CmdResponse onChannelInvoke(
        const phicore::adapter::sdk::ChannelInvokeRequest &request) override;
    phicore::adapter::v1::ActionResponse onAdapterActionInvoke(
        const phicore::adapter::sdk::AdapterActionInvokeRequest &request) override;
    phicore::adapter::v1::CmdResponse onDeviceNameUpdate(
        const phicore::adapter::sdk::DeviceNameUpdateRequest &request) override;
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

    void applyBootstrapAdapter(const phicore::adapter::v1::Adapter &adapter);
    void readIntervalsFromMeta();

    bool pollBridge(QString *error = nullptr);
    bool fetchResourceArray(const QString &resourceType, QJsonArray *outData, QString *error = nullptr);
    bool publishSnapshot(const Snapshot &snapshot, QString *error = nullptr);
    void setConnectionState(bool connected);

    ActionResponse invokeProbe(const phicore::adapter::sdk::AdapterActionInvokeRequest &request);
    ActionResponse invokeStartDeviceDiscovery(const phicore::adapter::sdk::AdapterActionInvokeRequest &request);

    CmdResponse failureResponse(std::uint64_t cmdId, CmdStatus status, const QString &error) const;
    CmdResponse successResponse(std::uint64_t cmdId) const;

    QNetworkAccessManager m_network;
    HttpClient m_http;

    phicore::adapter::v1::Adapter m_adapterInfo;
    ConnectionSettings m_settings;
    QJsonObject m_meta;

    bool m_connected = false;
    bool m_hasBootstrap = false;

    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    std::int64_t m_nextPollDueMs = 0;

    QHash<QString, DeviceEntry> m_devices;
    QHash<QString, QString> m_lightResourceByDevice;
    QString m_discoveryResourceId;
    QSet<QString> m_knownRooms;
    QSet<QString> m_knownGroups;
};

} // namespace phicore::hue::ipc
