#pragma once

#include <QString>
#include <QtGlobal>
#include <QMetaType>

namespace phicore {

enum class DeviceSoftwareUpdateStatus : quint8 {
    Unknown = 0,       // No information yet
    UpToDate,          // Device firmware matches the latest known release
    UpdateAvailable,   // New firmware is available
    Downloading,       // Update is currently being downloaded
    Installing,        // Update has been pushed to the device and is installing
    RebootRequired,    // Device must reboot to finalize the update
    Failed             // Last update attempt failed
};
Q_ENUM_NS(DeviceSoftwareUpdateStatus)

struct DeviceSoftwareUpdate {
    DeviceSoftwareUpdateStatus status = DeviceSoftwareUpdateStatus::Unknown;
    QString statusRaw;
    QString currentVersion;
    QString targetVersion;
    QString releaseNotesUrl;
    QString message;
    QString payloadId;            // optional identifier/reference from the bridge
    qint64 timestampMs = 0;      // optional UTC ms when the status was observed
};

} // namespace phicore

Q_DECLARE_METATYPE(phicore::DeviceSoftwareUpdate)
