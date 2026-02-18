#pragma once

#include <QString>
#include <QJsonObject>
#include <QList>

#include "discoveryquery.h"

namespace phicore::discovery {

struct Discovery
{
    QString       pluginType;
    QString       discoveredId;
    QString       label;
    QString       hostname;
    QString       ip;
    quint16       port = 0;
    DiscoveryKind kind = DiscoveryKind::Mdns;
    QString       serviceType;
    QString       signal;
    QJsonObject   meta;
};

using DiscoveryList = QList<Discovery>;

} // namespace phicore::discovery
