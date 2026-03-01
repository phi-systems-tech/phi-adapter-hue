#pragma once

#include <QHash>
#include <QJsonArray>
#include <QString>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::hue::ipc {

struct DeviceState {
    QString lightResourceId;
    bool hasOn = false;
    bool on = false;
    bool hasBrightness = false;
    double brightness = 0.0;
    bool hasColorTemperature = false;
    int colorTemperatureMired = 0;
    bool hasColorXy = false;
    double colorX = 0.0;
    double colorY = 0.0;
};

struct DeviceEntry {
    phicore::adapter::v1::Device device;
    phicore::adapter::v1::ChannelList channels;
    DeviceState state;
};

struct Snapshot {
    QHash<QString, DeviceEntry> devices;
    phicore::adapter::v1::RoomList rooms;
    phicore::adapter::v1::GroupList groups;
    phicore::adapter::v1::SceneList scenes;
};

Snapshot buildSnapshot(const QJsonArray &deviceData,
                       const QJsonArray &lightData,
                       const QJsonArray &motionData,
                       const QJsonArray &tamperData,
                       const QJsonArray &temperatureData,
                       const QJsonArray &lightLevelData,
                       const QJsonArray &devicePowerData,
                       const QJsonArray &buttonData,
                       const QJsonArray &zigbeeConnectivityData,
                       const QJsonArray &roomData,
                       const QJsonArray &zoneData,
                       const QJsonArray &sceneData);

QByteArray buildLightCommandPayload(const QString &channelExternalId,
                                    const phicore::adapter::sdk::ChannelInvokeRequest &request,
                                    QString *error = nullptr);

void rgbToXy(double r01, double g01, double b01, double *x, double *y);

} // namespace phicore::hue::ipc
