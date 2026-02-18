// adapters/api/phitypes.h
#pragma once

#include <QFlags>
#include <QVariant>
#include <QVariantList>
#include <QString>

namespace phicore {

#if !defined(PHICORE_Q_NAMESPACE_DEFINED) || defined(Q_MOC_RUN)
#ifndef PHICORE_Q_NAMESPACE_DEFINED
#define PHICORE_Q_NAMESPACE_DEFINED
#endif
Q_NAMESPACE
#endif

// ============================================================================
// COMMAND
// ============================================================================
// Global unique command identifier for the lifetime of the process.
// phi-core assigns CmdId values; adapters simply echo them back.
using CmdId = quint64;

enum class CmdStatus : quint8 {
    Success            = 0,    // Command accepted/executed
    Failure            = 1,    // Generic failure (e.g. bridge returned an error)
    Timeout            = 2,    // No response from device/bridge
    NotSupported       = 3,    // Channel or operation not supported
    InvalidArgument    = 4,    // Provided parameter outside valid range
    Busy               = 5,    // Device/bridge is currently busy
    TemporarilyOffline = 6,    // Device/bridge is unreachable
    NotAuthorized      = 7,    // Authentication/permission denied
    NotImplemented     = 8,    // Funktion is not implemented
    InternalError      = 255   // Unexpected adapter-side internal error
};
Q_ENUM_NS(CmdStatus)


// Generic execution response for a single command on a single channel.
//
// IMPORTANT:
//  • Contains NO persistent channel value (state changes come from channelStateUpdated)
//  • Describes only the execution result of the command
//
// Example:
//      CmdResponse r;
//      r.id         = cmdId;
//      r.status     = CmdStatus::Success;
//      r.finalValue = 42;                 // optional applied/clamped value
//      r.error      = "Clamped to range 0–255";
//      r.tsMs       = currentTimestampMs();
struct CmdResponse {
    CmdId     id     = 0;                  // 0 = untracked command
    CmdStatus status = CmdStatus::Success; // Execution result
    QString   error;                       // Optional diagnostic message
    QVariantList errorParams;              // Optional placeholders (%1, %2, ...)
    QString   errorCtx;                    // Optional context hint
    QVariant  finalValue;                  // Optional applied/clamped value
    qint64    tsMs   = 0;                  // Optional timestamp (ms since epoch)
};

// Result type for adapter actions.
enum class ActionResultType : quint8 {
    None    = 0,
    Boolean = 1,
    Integer = 2,
    Float   = 3,
    String  = 4,
    StringList = 5
};
Q_ENUM_NS(ActionResultType)

// Generic response for adapter-level actions.
struct ActionResponse {
    CmdId     id     = 0;                  // 0 = untracked command
    CmdStatus status = CmdStatus::Success; // Execution result
    QString   error;                       // Optional diagnostic message
    QVariantList errorParams;              // Optional placeholders (%1, %2, ...)
    QString   errorCtx;                    // Optional context hint
    ActionResultType resultType = ActionResultType::None;
    QVariant  resultValue;                 // Optional result payload
    qint64    tsMs   = 0;                  // Optional timestamp (ms since epoch)
};

// ============================================================================
// DEVICE CLASSES
// ============================================================================
enum class DeviceClass : quint8 {
    Unknown     = 0,
    Light       = 1,
    Switch      = 2,
    Sensor      = 3,
    Button      = 4,
    Plug        = 5,
    Cover       = 6,
    Thermostat  = 7,
    Gateway     = 8,
    MediaPlayer = 9,
    Heater      = 10,
    Gate        = 11,
    Valve       = 12,
    // ...
};
Q_ENUM_NS(DeviceClass)

// ============================================================================
// DEVICE EFFECTS (adapter quick actions)
// ============================================================================
enum class DeviceEffect : quint16 {
    None = 0,
    Candle,
    Fireplace,
    Sparkle,
    ColorLoop,
    Alarm,
    Relax,
    Concentrate,
    CustomVendor, // vendor-specific effect exposed via meta
};

// ============================================================================
// BUTTON EVENTS (for ChannelKind::ButtonEvent)
// ============================================================================
// Canonical, adapter-independent button / remote events used by channels with
// ChannelKind::ButtonEvent and ChannelDataType::Int. Adapters are responsible
// for mapping their native representations (e.g. Hue "buttonevent" codes,
// Zigbee2MQTT action strings, etc.) to these values.
enum class ButtonEventCode : quint8 {
    None               = 0,   // No event / unknown

    // Discrete presses (typically fired on release)
    InitialPress       = 1,   // Immediate notification when button is pressed
    DoublePress        = 2,
    TriplePress        = 3,
    QuadruplePress     = 4,
    QuintuplePress     = 5,

    // Long press semantics
    LongPress          = 10,  // Long press detected (optional, not always used)
    LongPressRelease   = 11,  // Long press released
    ShortPressRelease  = 12,  // Short press released

    // Repeated events while a button is held down (dimmer style)
    Repeat             = 20
};
Q_ENUM_NS(ButtonEventCode)

// ============================================================================
// NORMALIZED ENUMS (for ChannelDataType::Enum choices)
// ============================================================================

enum class RockerMode : quint8 {
    Unknown       = 0,
    SingleRocker  = 1,
    DualRocker    = 2,
    SinglePush    = 3,
    DualPush      = 4
};
Q_ENUM_NS(RockerMode)

enum class SensitivityLevel : quint8 {
    Unknown  = 0,
    Low      = 1,
    Medium   = 2,
    High     = 3,
    VeryHigh = 4,
    Max      = 5
};
Q_ENUM_NS(SensitivityLevel)

// ============================================================================
// COLOR CAPABILITIES (for ChannelKind::ColorRGB)
// ============================================================================
// Canonical description of what a ColorRGB channel can represent. This is
// exposed through the channel JSON payload as "colorCapabilities" so that
// UI clients can render suitable controls without adapter-specific logic.
//
// Example JSON shape (Hue style, CIE 1931 xy gamut triangle):
//
//   "colorCapabilities": {
//     "space": "cie1931_xy",
//     "gamut": [
//       [ 0.6915, 0.3083 ],   // primary 1
//       [ 0.17,   0.7    ],   // primary 2
//       [ 0.1532, 0.0475 ]    // primary 3
//     ]
//   }
//
// Adapters are responsible for mapping their native color capabilities into
// this canonical description. Clients MUST treat this as optional and fall
// back to a full sRGB representation if it is not present.

// ============================================================================
// CHANNEL KINDS
// ============================================================================
enum class ChannelKind : quint16 {
    Unknown          = 0,

    // Binary
    PowerOnOff       = 1,
    ButtonEvent      = 2,  // Stateless button / remote events (short/long press, etc.)

    // Lighting
    //
    // Canonical semantics in phi-core:
    //  - Brightness is always expressed as a normalized percentage in [0, 100].
    //    Adapters are responsible for mapping their native ranges (e.g. 0–254)
    //    into this canonical range when talking to core.
    //  - ColorTemperature uses mired (micro reciprocal Kelvin) as canonical
    //    unit inside core and automations. UI clients are expected to convert
    //    to/from Kelvin when presenting values to users.
    //  - ColorRGB uses the canonical color type from phicolor.h (sRGB, 0–1).
    Brightness       = 10,
    ColorTemperature = 11,
    ColorRGB         = 12,
    ColorTemperaturePreset = 13,

    // MediaPlayer
    Volume           = 30,
    Mute             = 31,
    HdmiInput        = 32,
    PlayPause        = 33,

    // Environmental Sensors
    Temperature      = 50,
    Humidity         = 51,
    Illuminance      = 52,  // Ambient light in lux, ChannelDataType::Int
    Motion           = 53,
    Battery          = 54,
    CO2              = 55,
    RelativeRotation      = 56,  // Relative rotary encoder steps (e.g. dial), signed int: >0 clockwise, <0 counter-clockwise
    ConnectivityStatus    = 57,  // Wireless link status (connected/disconnected/limited, enum value)
    DeviceSoftwareUpdate  = 58,  // Firmware/update status information
    SignalStrength        = 59,  // Wireless signal strength (RSSI in dBm)
    Power                 = 60,  // Electrical power in W, ChannelDataType::Float
    Voltage               = 61,  // Voltage in V, ChannelDataType::Float
    Current               = 62,  // Electrical current in A, ChannelDataType::Float
    Energy                = 63,  // Energy usage in kWh, ChannelDataType::Float
    LinkQuality           = 64,  // Link quality in %, ChannelDataType::Float
    Duration              = 65,  // Duration in seconds, ChannelDataType::Int/Float
    Contact               = 66,  // Contact sensor (open/closed), ChannelDataType::Bool
    Tamper                = 67,  // Tamper/sabotage detection, ChannelDataType::Bool
    AmbientLightLevel     = 68,  // Ambient light level class (e.g. dark/dim/bright), ChannelDataType::Enum

    // --------------------------------------------------------------------
    // POOL / WATER QUALITY SENSORS
    // --------------------------------------------------------------------

    // pH value (0.00–14.00)
    PhValue          = 200,

    // ORP / RedOx / Chlorine potential, measured in mV
    // Typically 200–800 mV, depending on chlorine level.
    OrpValue         = 201,

    // Salt level (ppm)
    SaltPpm          = 202,

    // Electrical conductivity (µS/cm oder mS/cm)
    Conductivity     = 203,

    // TDS = Total Dissolved Solids (ppm)
    TdsValue         = 204,

    // Specific Gravity (SG, 1.000–1.035 etc.)
    SpecificGravity  = 205,

    // Water Hardness (dH, ppm CaCO3)
    WaterHardness    = 206,

    // Free Chlorine (ppm)
    FreeChlorine     = 207,

    // Filter pressure (bar)
    FilterPressure   = 208,

    // Flow sensor (L/min)
    WaterFlow        = 209,

    // --------------------------------------------------------------------
    // MISC
    // --------------------------------------------------------------------
    SceneTrigger     = 300,
};
Q_ENUM_NS(ChannelKind)

// ============================================================================
// CHANNEL DATA TYPE
// ============================================================================
enum class ChannelDataType : quint8 {
    Unknown = 0,
    Bool    = 1,  // Canonical: true/false for binary channels (PowerOnOff, Motion, ...)
    Int     = 2,  // Canonical: integer with semantics depending on ChannelKind
    Float   = 3,  // Canonical: floating point with semantics depending on ChannelKind
    String  = 4,
    Color   = 5,  // Canonical: phicore::Color (sRGB, components in [0, 1]) - see phicolor.h
    Enum    = 6
};
Q_ENUM_NS(ChannelDataType)

// ============================================================================
// CONNECTIVITY STATUS
// ============================================================================
enum class ConnectivityStatus : quint8 {
    Unknown = 0,
    Connected,
    Limited,
    Disconnected
};
Q_ENUM_NS(ConnectivityStatus)

// ============================================================================
// CHANNEL FLAGS
// ============================================================================
enum class ChannelFlag : quint32 {
    ChannelFlagNone       = 0x00000000,
    ChannelFlagReadable   = 0x00000001,
    ChannelFlagWritable   = 0x00000002,
    ChannelFlagReportable = 0x00000004,  // publishes updates
    ChannelFlagRetained   = 0x00000008,  // remembers last value
    ChannelFlagInactive   = 0x00000010,  // hide from UI, still usable in automations
    ChannelFlagNoTrigger  = 0x00000020,  // channel should not be used as an automation trigger
    ChannelFlagSuppress   = 0x00000040   // adapter should not expose this channel
};
Q_DECLARE_FLAGS(ChannelFlags, ChannelFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(ChannelFlags)
Q_FLAG_NS(ChannelFlag)

inline constexpr ChannelFlags ChannelFlagDefaultWrite =
    ChannelFlag::ChannelFlagReadable
    | ChannelFlag::ChannelFlagWritable
    | ChannelFlag::ChannelFlagReportable
    | ChannelFlag::ChannelFlagRetained;

inline constexpr ChannelFlags ChannelFlagDefaultRead =
    ChannelFlag::ChannelFlagReadable
    | ChannelFlag::ChannelFlagReportable
    | ChannelFlag::ChannelFlagRetained;

enum class DeviceFlag : quint32 {
    DeviceFlagNone        = 0x00000000,
    DeviceFlagWireless    = 0x00000001,
    DeviceFlagBattery     = 0x00000002,
    DeviceFlagFlushable   = 0x00000004,
    DeviceFlagBLE         = 0x00000008
};
Q_DECLARE_FLAGS(DeviceFlags, DeviceFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(DeviceFlags)
Q_FLAG_NS(DeviceFlag)

enum class SceneState : quint8 {
    Unknown      = 0,
    Inactive     = 1,
    ActiveStatic = 2,
    ActiveDynamic= 3
};
Q_ENUM_NS(SceneState)

enum class SceneAction : quint8 {
    Activate   = 0,
    Deactivate = 1,
    Dynamic    = 2
};
Q_ENUM_NS(SceneAction)

enum class SceneFlag : quint32 {
    SceneFlagNone               = 0x00000000,
    SceneFlagOriginAdapter      = 0x00000001,
    SceneFlagSupportsDynamic    = 0x00000002,
    SceneFlagSupportsDeactivate = 0x00000004
};
Q_DECLARE_FLAGS(SceneFlags, SceneFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(SceneFlags)
Q_ENUM_NS(SceneFlag)

// ============================================================================
// ADAPTER REQUIREMENTS / CAPABILITIES
// ============================================================================

enum class AdapterFlag : quint32 {
    AdapterFlagNone              = 0x00000000,
    AdapterFlagUseTls            = 0x00000001,
    AdapterFlagCloudServices     = 0x00000002,
    AdapterFlagEnableLogs        = 0x00000004,
    AdapterFlagRequiresPolling   = 0x00000008,
    AdapterFlagSupportsDiscovery = 0x00000010,
    AdapterFlagSupportsProbe     = 0x00000020,
    AdapterFlagSupportsRename    = 0x00000040
};
Q_DECLARE_FLAGS(AdapterFlags, AdapterFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(AdapterFlags)
Q_FLAG_NS(AdapterFlag)

enum class AdapterConfigFieldType : quint8 {
    String   = 0,   // single-line text
    Password = 1,   // password / secret, masked
    Integer  = 2,   // integer number
    Boolean  = 3,   // checkbox / switch
    Hostname = 4,   // hostname or IP
    Port     = 5,   // TCP/UDP port
    QrCode   = 6,   // QR code content (string)
    Select   = 7,   // dropdown with options
    Action   = 8    // action-only button (no input)
};
Q_ENUM_NS(AdapterConfigFieldType)

enum class AdapterConfigFieldFlag : quint8 {
    None         = 0x00,
    Required     = 0x01,
    Secret       = 0x02,
    ReadOnly     = 0x04,
    Transient    = 0x08,
    Multi        = 0x10,
    InstanceOnly = 0x20
};
Q_DECLARE_FLAGS(AdapterConfigFieldFlags, AdapterConfigFieldFlag)
Q_DECLARE_OPERATORS_FOR_FLAGS(AdapterConfigFieldFlags)
Q_FLAG_NS(AdapterConfigFieldFlag)

enum class AdapterRequirement : quint32 {
    None          = 0x00000000,
    Host          = 0x00000001, // needs IP/hostname
    Port          = 0x00000002, // needs TCP port
    Username      = 0x00000004, // username / login
    Password      = 0x00000008, // password / secret
    AppKey        = 0x00000010, // app key / developer key
    Token         = 0x00000020, // bearer / API token
    QrCode        = 0x00000040, // QR code scan input
    SupportsTls   = 0x00000080, // if adapter supports SSL/TLS/certificate
    ManualConfirm = 0x00000100, // For bridges that require a physical button press (Hue, etc.)
    UsesRetryInterval = 0x00000200 // Adapter defines retryIntervalMs for reconnect attempts
};
Q_DECLARE_FLAGS(AdapterRequirements, AdapterRequirement)
Q_DECLARE_OPERATORS_FOR_FLAGS(AdapterRequirements)
Q_FLAG_NS(AdapterRequirement)

} // namespace phicore

Q_DECLARE_METATYPE(phicore::DeviceEffect)
Q_DECLARE_METATYPE(phicore::CmdResponse)
Q_DECLARE_METATYPE(phicore::ActionResponse)
Q_DECLARE_METATYPE(phicore::ConnectivityStatus)
