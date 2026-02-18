#pragma once

#include <QtGlobal>
#include <QString>
#include <QStringList>
#include <QJsonObject>

namespace phicore::discovery {

// The transport/protocol used by a discovery provider.
enum class DiscoveryKind : quint8 {
    Mdns,     // mDNS / DNS-SD (Bonjour/Avahi/Qt backend)
    Ssdp,     // SSDP / UPnP
    NetScan,  // IP scan / port probe (optional, power-user)
    Manual    // User-provided host/port (no discovery)
};

// A single discovery "interest" emitted by an AdapterFactory.
// Example: Hue wants Mdns serviceType "_hue._tcp".
struct DiscoveryQuery
{
    QString pluginType;      // Must match adapter factory pluginType.
    DiscoveryKind kind = DiscoveryKind::Mdns;

    // mDNS / DNS-SD:
    // Example: "_hue._tcp" (without ".local" is fine; provider may normalize).
    QString mdnsServiceType;

    // SSDP:
    // Example ST: "upnp:rootdevice" or vendor-specific URN.
    QString ssdpSt;

    // Optional default port for manual + hints.
    int defaultPort = 0;

    // Arbitrary hints for provider or adapter verification.
    // Examples:
    //  - expected TXT keys
    //  - required HTTP paths for verification
    //  - vendor/model constraints
    QJsonObject hints;

    bool isValid() const
    {
        if (pluginType.isEmpty())
            return false;

        switch (kind) {
        case DiscoveryKind::Mdns:
            return !mdnsServiceType.isEmpty();
        case DiscoveryKind::Ssdp:
            return !ssdpSt.isEmpty();
        case DiscoveryKind::NetScan:
        case DiscoveryKind::Manual:
            return true;
        }

        return false;
    }
};

using DiscoveryQueryList = QList<DiscoveryQuery>;

} // namespace phicore::discovery
