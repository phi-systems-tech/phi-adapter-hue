// adapters/api/deviceinfo.h
#pragma once

#include <QString>
#include <QJsonObject>
#include <QList>

#include "types.h"

namespace phicore {
struct DeviceEffectDescriptor {
    DeviceEffect effect = DeviceEffect::None;
    QString      id;           // optional vendor-specific identifier
    QString      label;        // display label
    QString      description;  // optional help text
    bool         requiresParams = false;
    QJsonObject  meta;         // arbitrary vendor details / parameter schema
};

using DeviceEffectDescriptorList = QList<DeviceEffectDescriptor>;

// AdapterInterface-facing descriptor + runtime hints for a single device.
struct Device {
    QString     name;            // AdapterInterface-provided label / default name
    DeviceClass deviceClass  = DeviceClass::Unknown;
    DeviceFlags flags        = DeviceFlag::DeviceFlagNone;
    QString     id;              // stable ID in adapter domain (light id, ...)
    QString     manufacturer;
    QString     firmware;
    QString     model;
    QJsonObject meta;            // Raw metadata from device, e.g. icon hints, additional info, ...
    DeviceEffectDescriptorList effects; // Supported vendor-independent effects
};

using DeviceList = QList<Device>;

} // namespace phicore
