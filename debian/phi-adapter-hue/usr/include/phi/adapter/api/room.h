// adapters/api/roominfo.h
#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QList>

#include "types.h"

namespace phicore {

struct Room {
    QString     externalId;      // adapter-specific identifier
    QString     name;
    QString     zone;
    QStringList deviceExternalIds;
    QJsonObject meta;
};

using RoomList = QList<Room>;

} // namespace phicore
