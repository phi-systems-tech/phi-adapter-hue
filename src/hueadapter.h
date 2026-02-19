// adapters/plugins/hue/hueadapter.h
#pragma once

#include <QPointer>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QHash>
#include <QPointF>
#include <QSet>
#include <QQueue>
#include <QStringList>

#include "adapterinterface.h"
#include "scene.h"

namespace phicore {

class HueAdapter : public AdapterInterface
{
    Q_OBJECT
public:
    explicit HueAdapter(QObject *parent = nullptr);
    ~HueAdapter() override;

protected:
    bool start(QString &errorString) override;
    void stop() override;
    void adapterConfigUpdated() override;
    void updateStaticConfig(const QJsonObject &config) override;

protected slots:
    void requestFullSync() override;
    void updateChannelState(const QString &deviceExternalId, const QString &channelExternalId,
        const QVariant &value, CmdId cmdId) override;

private slots:
    void onPairingTimeout();
    void onNetworkReplyFinished(QNetworkReply *reply);
    void onPollTimeout();
    void onEventStreamReadyRead();
    void onEventStreamFinished();
    void onEventSyncTimeout();

private:
    QUrl baseUrl() const;
    QUrl v2ResourceUrl(const QString &resourcePath) const;
    QUrl eventStreamUrl() const;
    void setConnected(bool connected);

    // Hue API v2 bootstrap: fetch static resource snapshots (devices,
    // lights, sensors, etc.) via /clip/v2/resource/... so that we no
    // longer depend on legacy v1 snapshots for device lists.
    void fetchV2ResourcesSnapshot();
    void requestV2ResourceSnapshot(const QString &resourceType, int delayMs = 0);
    void fetchV2DeviceResource(const QString &deviceId);

    void startEventStream();
    void stopEventStream();
    void handleEventStreamData(const QByteArray &jsonData);
    void handleEventStreamEventObject(const QJsonObject &eventObj, qint64 nowMs);
    void handleV2LightResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2MotionResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2TamperResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2TemperatureResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2LightLevelResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2DevicePowerResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2DeviceSoftwareUpdateResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2ZigbeeDeviceDiscoveryResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2RelativeRotaryResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2ButtonResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2ZigbeeConnectivityResource(const QJsonObject &resObj, qint64 nowMs);
    void handleV2RoomResource(const QJsonObject &resObj);
    void handleV2ZoneResource(const QJsonObject &resObj);
    void handleV2SceneResource(const QJsonObject &resObj);
    void handleV2DeleteEvent(const QJsonArray &dataArr);
    void updateDeviceEffectsFromLight(const QString &deviceExtId, const QJsonObject &lightObj);
    void scheduleV2SnapshotRefresh(const QString &reason);
    void performScheduledV2SnapshotRefresh();
    void scheduleRenameVerification(const QString &deviceExtId, int attempt = 0);
    void cancelRenameVerification(const QString &deviceExtId);
    void completeRenameVerification(const QString &deviceExtId);
    void finishRenameCommand(const QString &deviceExtId, bool success, const QString &error = QString());
    QString lightResourceIdForDevice(const QString &deviceExternalId) const;
    bool updateDeviceConnectivityMeta(const QString &deviceExtId, const QJsonObject &resObj, qint64 tsMs);
    bool updateDeviceSoftwareUpdateMeta(const QString &deviceExtId, const QJsonObject &resObj, qint64 tsMs);
    bool updateZigbeeDeviceDiscoveryMeta(const QString &deviceExternalId, const QJsonObject &resObj, qint64 tsMs);

    // Gamut handling: per-light xy gamut triangle used to clamp outgoing
    // sRGB colors to what the physical device can reproduce.
    struct HueGamut {
        QPointF p1;
        QPointF p2;
        QPointF p3;
        bool isValid() const noexcept
        {
            return p1 != p2 && p2 != p3 && p3 != p1;
        }
    };

    void updateGamutForLight(const QString &lightId, const QJsonObject &controlObj);
    void clampColorToGamut(const QString &lightId, double &x, double &y) const;
    void scheduleDialReset(const QString &deviceExtId);
    void updateDeviceName(const QString &deviceExternalId, const QString &name, CmdId cmdId) override;
    void invokeDeviceEffect(const QString &deviceExternalId,
                            DeviceEffect effect,
                            const QString &effectId,
                            const QJsonObject &params,
                            CmdId cmdId) override;
    void invokeScene(const QString &sceneExternalId,
                     const QString &groupExternalId,
                     const QString &action,
                     CmdId cmdId) override;
    void invokeAdapterAction(const QString &actionId, const QJsonObject &params, CmdId cmdId) override;
    void handleShortPressRelease(const QString &deviceExtId, const QString &channelExtId, qint64 eventTs);
    void finalizePendingShortPress(const QString &key);
    QString deviceExternalIdFromV2Resource(const QJsonObject &resObj) const;
    QString deviceExtIdForResource(const QString &resourceType, const QString &resourceId) const;
    QString zigbeeDeviceDiscoveryOwnerDevice() const;
    QString zigbeeDeviceDiscoveryResourceId() const;
    void handleV2ResourceSnapshot(const QString &resourceType, const QJsonObject &root);
    void finalizeV2SnapshotIfReady();
    void buildDevicesFromV2Snapshots();
    void buildRoomsFromV2Snapshot();
    void buildGroupsFromV2Snapshot();
    bool startV2DeviceFetch(const QString &deviceId);
    void startNextQueuedV2DeviceFetch();
    bool sendV2ResourceUpdate(const QString &resourceType, const QString &resourceId, const QJsonObject &payload);
    void refreshConfig();
    bool ensureHostAvailable() const;
    void applyProductNumberMapping(Device &device, const QJsonObject &productObj);

    QNetworkAccessManager   *m_nam = nullptr;
    QTimer                   m_pairingTimer;
    QTimer                   m_pollTimer;
    QTimer                   m_eventSyncTimer;
    QTimer                   m_eventStreamRetryTimer;
    QTimer                   m_v2ResyncTimer;
    QTimer                   m_v2DeviceFetchTimer;
    QNetworkReply           *m_eventStreamReply = nullptr;
    QByteArray               m_eventStreamLineBuffer;
    QByteArray               m_eventStreamDataBuffer;
    int                      m_eventStreamRetryCount = 0;
    int                      m_eventStreamRetryIntervalMs = 10000;
    bool                     m_connected = false;
    bool                     m_stopping  = false;
    bool                     m_supportsV2Events = false;
    int                      m_eventStreamErrorSuppressCount = 0;
    bool                     m_ignoreEventStreamError = false;
    QHash<QString, HueGamut> m_gamutByLightId;
    // Timers used to reset dial channels back to 0 shortly after a rotary
    // event so the channel represents a transient delta rather than a
    // latched value.
    QHash<QString, QTimer *> m_dialResetTimers;
    QHash<QString, int>      m_lastDialValueByDevice;
    QHash<QString, int>      m_pendingRenameVerifications;
    QSet<QString>            m_activeRenameFetches;
    QHash<QString, QTimer *> m_renameVerifyTimers;
    QHash<QString, CmdId>    m_pendingRenameCommands;

    // Hue v2 device id (owner.rid) to logical externalId used in core. For
    // now we simply use the v2 device id as externalId, but this indirection
    // keeps the door open for custom schemes.
    QHash<QString, QString>  m_deviceIdToExternalId;

    // Aggregated v2 bootstrap state: we build Device/Channel sets
    // from /clip/v2/resource/... snapshots before emitting deviceUpdated().
    QHash<QString, Device>         m_v2Devices;
    QHash<QString, Device>         m_v2DeviceInfoCache;
    QHash<QString, ChannelList>    m_v2DeviceChannels;
    QHash<QString, QString>        m_v2ResourceToDevice;
    QHash<QString, QString>        m_deviceToLightResource;
    QHash<QString, phicore::ConnectivityStatus> m_pendingConnectivityStatus;
    QHash<QString, QJsonObject>    m_pendingDeviceSoftwareUpdates;
    QSet<QString>                  m_pendingDiscoveryDeviceUpdates;
    QHash<QString, QStringList>    m_v2RoomMemberships;
    QSet<QString>                  m_knownDeviceExternalIds;
    QString                        m_pendingV2ResyncReason;
    QHash<QString, QJsonArray>     m_v2SnapshotByType;
    int                            m_v2SnapshotPending = 0;
    bool                           m_v2BootstrapDone   = false;
    bool                           m_v2SnapshotFailedThisCycle = false;
    QSet<QString>                  m_pendingV2DeviceFetch;
    QSet<QString>                  m_pendingV2ResourceTypes;
    QHash<QString, int>            m_v2ResourceRetryCount;
    QHash<QString, QString>        m_buttonResourceToChannel;
    QSet<QString>                  m_failedV2DeviceFetch;
    QQueue<QString>                m_v2DeviceFetchQueue;
    struct HueChannelBinding {
        QString resourceId;
        QString resourceType;
    };
    QHash<QString, HueChannelBinding> m_channelBindings;
    QHash<QString, Scene>   m_v2Scenes;
    bool                           m_sceneSnapshotDirty = false;
    QJsonObject                    m_staticConfig;
    QHash<QString, QString>        m_modelIdToProductNumber;
    QSet<QString>                  m_iconBlacklist;

    struct ButtonMultiPressTracker {
        int count = 0;
        qint64 lastTs = 0;
        QTimer *timer = nullptr;
        QString deviceExtId;
        QString channelExtId;
    };
    QHash<QString, ButtonMultiPressTracker> m_buttonMultiPress;
};

} // namespace phicore
