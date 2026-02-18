// adapters/api/adapterconfig.h
#pragma once

#include <QString>
#include <QVariant>
#include <QList>
#include <QJsonObject>

#include "types.h"

namespace phicore {

// AdapterInterface-provided configuration/state that plugins can inspect/update.
struct Adapter {
    // connection / auth settings supplied by the user
    QString      name;        // display name for this instance
    QString      host;        // hostname / FQDN
    QString      ip;          // resolved IPv4 / IPv6 (optional)
    quint16      port;        // port
    QString      user;        // username / login
    QString      pw;          // password
    QString      token;       // token, app key

    // adapter-specific metadata
    QString      plugin;      // plugin type, e.g. "hue", "z2m", "matter" - must be unique
    QString      id;          // adapters own id
    QJsonObject  meta;        // additional adapter configuration, updates, Tls, etc
    AdapterFlags flags;       // individual flags
};

using AdapterList = QList<Adapter>;

// Option entry for Select fields.
struct AdapterConfigOption {
    QString value;      // machine-readable value
    QString label;      // human-readable display text (english)
};

using AdapterConfigOptionList = QList<AdapterConfigOption>;

struct AdapterConfigField {
    QString key;                      // e.g. "host", "username", "appKey"
    AdapterConfigFieldType type = AdapterConfigFieldType::String;

    QString label;                    // UI label (english)
    QString description;              // short help text (optional, english)
    QString actionId;                 // optional adapter action id
    QString actionLabel;              // optional action button label

    // UI hints
    QString  placeholder;             // optional placeholder text (english)
    QVariant defaultValue;            // optional default

    AdapterConfigOptionList options;  // used for Select
    QJsonObject meta;                 // optional metadata for UI hints
    AdapterConfigFieldFlags flags = AdapterConfigFieldFlag::None; // UI behavior flags
};

using AdapterConfigFieldList = QList<AdapterConfigField>;

struct AdapterConfigSchema {
    AdapterConfigFieldList fields;

    // Optional grouping
    QString title;                    // e.g. "Hue Bridge Configuration"
    QString description;              // general description
};

// Describes an action that can be triggered either on the factory (before an
// adapter instance exists) or on a running adapter instance.
struct AdapterActionDescriptor {
    QString id;           // stable identifier, e.g. "probe"
    QString label;        // human-readable button/text
    QString description;  // optional helper text (english)
    bool hasForm = false; // action requires form input
    bool danger = false;  // destructive or risky action
    int cooldownMs = 0;   // optional cooldown for repeated triggers
    QJsonObject confirm;  // optional confirmation dialog (title/body/etc.)
    QJsonObject meta;     // optional adapter-specific metadata
};

using AdapterActionDescriptorList = QList<AdapterActionDescriptor>;

// High-level capabilities for an adapter plugin type.
struct AdapterCapabilities {
    AdapterRequirements required;  // hard requirements (must be provided)
    AdapterRequirements optional;  // optional fields (UI may show them)
    AdapterFlags flags = AdapterFlag::AdapterFlagNone; // adapter-level flags (cloud, polling, etc.)
    AdapterActionDescriptorList factoryActions;  // actions available pre-create
    AdapterActionDescriptorList instanceActions; // actions on running adapter
    QJsonObject defaults;          // optional default values (host, port, etc.)
};

} // namespace phicore
