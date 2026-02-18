// adapter/api/channelinfo.h
#pragma once

#include <QJsonObject>
#include <QVariant>
#include <QString>
#include <QList>

#include "types.h"
#include "adapterconfig.h"

namespace phicore {

// Descriptor + runtime state for a single channel
struct Channel {
    // adapter
    QString             name;
    QString             id;
    ChannelKind         kind      = ChannelKind::Unknown;
    ChannelDataType     dataType  = ChannelDataType::Unknown;
    ChannelFlags        flags     = ChannelFlag::ChannelFlagNone;
    QString             unit;
    double              minValue  = 0.0;
    double              maxValue  = 0.0;
    double              stepValue = 0.0;
    QJsonObject         meta;
    QList<AdapterConfigOption> choices;

    // runtime state
    QVariant            lastValue;
    qint64              lastUpdateMs = 0;
    bool                hasValue = false;
};

using ChannelList = QList<Channel>;

} // namespace phicore

Q_DECLARE_METATYPE(phicore::Channel)
