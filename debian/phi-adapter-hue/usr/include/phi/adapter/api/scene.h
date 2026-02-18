// adapters/api/scene.h
#pragma once

#include <QString>
#include <QJsonObject>

#include "types.h"

namespace phicore {

// Lightweight scene descriptor emitted by adapters. Core will map these
// to persisted SceneRecord entries (roomId/groupId resolved via registries).
struct Scene {
    QString     id;           // adapter-level scene identifier
    QString     name;
    QString     description;
    QString     scopeId;      // adapter-specific scope id (room/group/zone/etc.)
    QString     scopeType;    // normalized: "room" | "group" | adapter-specific string
    QString     avatarColor;
    QString     image;        // bytearray (if adapter supports it) or URL
    QString     presetTag;
    SceneState  state = SceneState::Unknown;
    SceneFlags  flags = SceneFlag::SceneFlagNone;
    QJsonObject meta;
};

} // namespace phicore
