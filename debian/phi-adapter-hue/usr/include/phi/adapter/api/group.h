// adapters/api/groupinfo.h
#pragma once

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QList>

#include "types.h"

namespace phicore {

struct Group {
    QString     id;                 // adapter-specific identifier
    QString     name;
    QString     zone;
    QStringList deviceExternalIds;
    QJsonObject meta;
};

using GroupList = QList<Group>;

} // namespace phicore
