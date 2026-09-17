#include "hue_model.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

#include "phi/adapter/v1/enum_names.h"
#include "phi/runtime/str.h"

namespace phicore::hue::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

namespace {

constexpr const char *kTypeDevice = "device";
constexpr const char *kTypeLight = "light";
constexpr const char *kTypeMotion = "motion";
constexpr const char *kTypeTamper = "tamper";
constexpr const char *kTypeTemperature = "temperature";
constexpr const char *kTypeLightLevel = "light_level";
constexpr const char *kTypeDevicePower = "device_power";
constexpr const char *kTypeButton = "button";
constexpr const char *kTypeRotary = "relative_rotary";
constexpr const char *kTypeConnectivity = "zigbee_connectivity";
constexpr const char *kTypeRoom = "room";
constexpr const char *kTypeZone = "zone";
constexpr const char *kTypeScene = "scene";
constexpr const char *kTypeDiscovery = "zigbee_device_discovery";

/// Offer one value of a contract enum as a choice, named by the contract.
template <typename Enum>
void addEnumChoice(v1::Channel &channel, const char *enumTypeName, Enum value)
{
    v1::AdapterConfigOption option;
    option.value = std::to_string(static_cast<int>(value));
    option.label = v1::enum_names::enumNameFor(enumTypeName, static_cast<int>(value), false);
    channel.choices.push_back(std::move(option));
}

std::string deviceName(const Json &deviceObj)
{
    const std::string metadataName = jsonString(jsonValue(deviceObj, "metadata"), "name");
    if (!metadataName.empty())
        return metadataName;
    const std::string productName = jsonString(jsonValue(deviceObj, "product_data"), "product_name");
    if (!productName.empty())
        return productName;
    return "Hue Device";
}

/// What kind of thing the bridge says a device is. The bridge itself is a
/// gateway, not a sensor - it was, because the only service it exposes is
/// its own zigbee connectivity.
v1::DeviceClass classFromProductData(const Json &product)
{
    const std::string archetype = str::toLower(jsonString(product, "product_archetype"));
    const std::string model = str::toUpper(jsonString(product, "model_id"));
    if (archetype == "bridge_v2" || model.rfind("BSB", 0) == 0)
        return v1::DeviceClass::Gateway;
    if (archetype == "plug")
        return v1::DeviceClass::Plug;
    return v1::DeviceClass::Unknown;
}

v1::Channel readChannel(const char *externalId, const char *name, v1::ChannelKind kind,
                        v1::ChannelDataType dataType)
{
    v1::Channel channel;
    channel.externalId = externalId;
    channel.name = name;
    channel.kind = kind;
    channel.dataType = dataType;
    channel.flags = v1::kChannelFlagDefaultRead;
    return channel;
}

template <typename T>
void setValue(v1::Channel &channel, const std::optional<T> &value)
{
    if (!value)
        return;
    channel.hasValue = true;
    channel.lastValue = *value;
}

v1::Channel makeOnChannel(bool value)
{
    v1::Channel channel = readChannel("on", "Power", v1::ChannelKind::PowerOnOff,
                                      v1::ChannelDataType::Bool);
    channel.flags = v1::kChannelFlagDefaultWrite;
    channel.hasValue = true;
    channel.lastValue = value;
    return channel;
}

v1::Channel makeBrightnessChannel(double value)
{
    v1::Channel channel = readChannel("bri", "Brightness", v1::ChannelKind::Brightness,
                                      v1::ChannelDataType::Float);
    channel.flags = v1::kChannelFlagDefaultWrite;
    channel.minValue = 0.0;
    channel.maxValue = 100.0;
    channel.stepValue = 0.1;
    channel.hasValue = true;
    channel.lastValue = std::clamp(value, 0.0, 100.0);
    return channel;
}

v1::Channel makeCtChannel(int value, int minValue, int maxValue)
{
    v1::Channel channel = readChannel("ct", "Color temperature", v1::ChannelKind::ColorTemperature,
                                      v1::ChannelDataType::Int);
    channel.flags = v1::kChannelFlagDefaultWrite;
    channel.unit = "mired";
    channel.minValue = minValue;
    channel.maxValue = maxValue;
    channel.stepValue = 1.0;
    channel.hasValue = true;
    channel.lastValue = static_cast<std::int64_t>(value);
    return channel;
}

v1::Channel makeColorChannel(const Json &colorObj)
{
    v1::Channel channel = readChannel("color", "Color", v1::ChannelKind::ColorRGB,
                                      v1::ChannelDataType::Color);
    channel.flags = v1::kChannelFlagDefaultWrite;

    Json meta = Json::object();
    const Json xy = jsonValue(colorObj, "xy");
    if (xy.is_object() && !xy.empty())
        meta["nativeValue"] = Json{{"space", "cie1931_xy"}, {"xy", xy}};
    const Json gamut = jsonValue(colorObj, "gamut");
    if (gamut.is_object()) {
        Json points = Json::array();
        for (const char *corner : {"red", "green", "blue"}) {
            const Json point = jsonValue(gamut, corner);
            if (point.is_object() && !point.empty())
                points.push_back(Json::array({jsonDouble(point, "x", 0.0), jsonDouble(point, "y", 0.0)}));
        }
        if (points.size() >= 3)
            meta["colorCapabilities"] = Json{{"space", "cie1931_xy"}, {"gamut", points}};
    }
    if (!meta.empty())
        channel.metaJson = dump(meta);
    return channel;
}

std::string effectLabel(std::string_view effect)
{
    std::string label(effect);
    for (char &c : label) {
        if (c == '_' || c == '-')
            c = ' ';
    }
    label = str::toLower(label);
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (i == 0 || label[i - 1] == ' ')
            label[i] = str::upperAscii(label[i]);
    }
    return label;
}

v1::DeviceEffect effectKind(std::string_view effect)
{
    const std::string lower = str::toLower(effect);
    if (lower == "candle")
        return v1::DeviceEffect::Candle;
    if (lower == "fire" || lower == "sunbeam")
        return v1::DeviceEffect::Fireplace;
    if (lower == "sparkle" || lower == "glisten" || lower == "opal" || lower == "prism"
        || lower == "underwater" || lower == "enchant" || lower == "cosmos")
        return v1::DeviceEffect::Sparkle;
    if (lower == "colorloop" || lower.find("palette") != std::string::npos)
        return v1::DeviceEffect::ColorLoop;
    if (lower == "sunrise" || lower == "sunset")
        return v1::DeviceEffect::Relax;
    return v1::DeviceEffect::CustomVendor;
}

void applyEffects(v1::Device &device, const Json &source)
{
    if (!source.is_object())
        return;
    std::set<std::string> seen;
    for (const v1::DeviceEffectDescriptor &existing : device.effects)
        seen.insert(str::toLower(existing.id));

    const auto addFrom = [&](const Json &values, const char *category) {
        if (!values.is_array())
            return;
        for (const Json &value : values) {
            if (!value.is_string())
                continue;
            const std::string effect = str::trimmed(value.get<std::string>());
            if (effect.empty())
                continue;
            const std::string key = str::toLower(effect);
            if (key == "no_effect" || seen.count(key))
                continue;
            seen.insert(key);
            v1::DeviceEffectDescriptor descriptor;
            descriptor.effect = effectKind(effect);
            descriptor.id = effect;
            descriptor.label = effectLabel(effect);
            descriptor.description = "Hue effect " + descriptor.label;
            descriptor.metaJson = dump(Json{{"hueEffect", effect}, {"hueEffectCategory", category}});
            device.effects.push_back(std::move(descriptor));
        }
    };
    addFrom(jsonValue(jsonValue(source, "effects"), "effect_values"), "effects");
    addFrom(jsonValue(jsonValue(jsonValue(source, "effects_v2"), "action"), "effect_values"),
            "effects");
    addFrom(jsonValue(jsonValue(source, "timed_effects"), "effect_values"), "timed_effects");
}

DeviceEntry &ensureDevice(Snapshot &snapshot, const std::string &deviceId,
                          v1::DeviceClass fallbackClass)
{
    auto it = snapshot.devices.find(deviceId);
    if (it == snapshot.devices.end()) {
        DeviceEntry placeholder;
        placeholder.device.externalId = deviceId;
        placeholder.device.name = "Hue Device";
        placeholder.device.deviceClass = fallbackClass;
        it = snapshot.devices.emplace(deviceId, std::move(placeholder)).first;
    } else if (fallbackClass != v1::DeviceClass::Unknown
               && it->second.device.deviceClass == v1::DeviceClass::Unknown) {
        it->second.device.deviceClass = fallbackClass;
    }
    return it->second;
}

void upsertChannel(DeviceEntry &entry, v1::Channel channel, const char *sourceType)
{
    entry.channelSource[channel.externalId] = sourceType;
    for (v1::Channel &existing : entry.channels) {
        if (existing.externalId == channel.externalId) {
            existing = std::move(channel);
            return;
        }
    }
    entry.channels.push_back(std::move(channel));
}

std::optional<bool> boolReport(const Json &resource, const char *objectKey, const char *valueKey,
                               const char *reportKey)
{
    const Json obj = jsonValue(resource, objectKey);
    if (obj.is_object() && obj.contains(valueKey) && obj.at(valueKey).is_boolean())
        return obj.at(valueKey).get<bool>();
    const Json report = jsonValue(obj, reportKey);
    if (report.is_object() && report.contains(valueKey) && report.at(valueKey).is_boolean())
        return report.at(valueKey).get<bool>();
    return std::nullopt;
}

std::optional<double> temperatureOf(const Json &resource)
{
    const Json obj = jsonValue(resource, "temperature");
    double raw = std::numeric_limits<double>::quiet_NaN();
    if (obj.is_object() && obj.contains("temperature") && obj.at("temperature").is_number())
        raw = obj.at("temperature").get<double>();
    else
        raw = jsonDouble(jsonValue(obj, "temperature_report"), "temperature", raw);
    if (!std::isfinite(raw))
        return std::nullopt;
    if (std::abs(raw) > 200.0)
        raw /= 100.0;
    return raw;
}

std::optional<std::int64_t> luxOf(const Json &resource)
{
    const Json light = jsonValue(resource, "light");
    const Json report = jsonValue(light, "light_level_report");
    for (const Json &obj : {report, light}) {
        if (!obj.is_object())
            continue;
        if (obj.contains("lux") && obj.at("lux").is_number())
            return static_cast<std::int64_t>(std::llround(obj.at("lux").get<double>()));
    }
    const auto toLux = [](int level) {
        return std::pow(10.0, (static_cast<double>(level) - 1.0) / 10000.0);
    };
    for (const Json &obj : {report, light}) {
        if (obj.is_object() && obj.contains("light_level") && obj.at("light_level").is_number())
            return static_cast<std::int64_t>(std::llround(toLux(obj.at("light_level").get<int>())));
    }
    return std::nullopt;
}

std::optional<std::int64_t> batteryOf(const Json &resource)
{
    const Json power = jsonValue(resource, "power_state");
    if (!power.is_object() || !power.contains("battery_level"))
        return std::nullopt;
    const int level = jsonInt(power, "battery_level", -1);
    if (level < 0)
        return std::nullopt;
    return static_cast<std::int64_t>(std::clamp(level, 0, 100));
}

std::optional<std::int64_t> sensitivityOf(const Json &resource)
{
    const Json obj = jsonValue(resource, "sensitivity");
    if (!obj.is_object() || !obj.contains("sensitivity"))
        return std::nullopt;
    switch (jsonInt(obj, "sensitivity", 0)) {
    case 1:
        return static_cast<std::int64_t>(v1::SensitivityLevel::Low);
    case 2:
        return static_cast<std::int64_t>(v1::SensitivityLevel::Medium);
    case 3:
        return static_cast<std::int64_t>(v1::SensitivityLevel::High);
    case 4:
        return static_cast<std::int64_t>(v1::SensitivityLevel::VeryHigh);
    default:
        return std::nullopt;
    }
}

void appendChannelKey(std::string &key, const v1::Channel &c)
{
    key += c.externalId + "|" + c.name + "|" + str::number(static_cast<int>(c.kind)) + "|"
        + str::number(static_cast<int>(c.dataType)) + "|"
        + str::number(static_cast<unsigned>(static_cast<std::uint32_t>(c.flags))) + "|" + c.unit
        + "|" + str::number(c.minValue) + "|" + str::number(c.maxValue) + "|"
        + str::number(c.stepValue) + "|" + c.metaJson + "|";
    for (const v1::AdapterConfigOption &option : c.choices)
        key += option.value + "=" + option.label + ",";
    key += ";";
}

} // namespace

const v1::Channel *DeviceEntry::channel(std::string_view id) const
{
    for (const v1::Channel &c : channels) {
        if (c.externalId == id)
            return &c;
    }
    return nullptr;
}

std::string DeviceEntry::definitionKey() const
{
    std::string key = device.externalId + "|" + device.name + "|"
        + str::number(static_cast<int>(device.deviceClass)) + "|"
        + str::number(static_cast<unsigned>(static_cast<std::uint32_t>(device.flags))) + "|"
        + device.manufacturer + "|" + device.model + "|" + device.firmware + "|" + device.metaJson
        + "|";
    for (const v1::DeviceEffectDescriptor &effect : device.effects)
        key += effect.id + "/" + effect.metaJson + ",";
    key += "#";
    std::vector<const v1::Channel *> sorted;
    for (const v1::Channel &c : channels)
        sorted.push_back(&c);
    std::sort(sorted.begin(), sorted.end(),
              [](const v1::Channel *a, const v1::Channel *b) { return a->externalId < b->externalId; });
    for (const v1::Channel *c : sorted)
        appendChannelKey(key, *c);
    return str::number(static_cast<unsigned long long>(std::hash<std::string>{}(key)));
}

const std::vector<std::string> &pollResourceTypes()
{
    static const std::vector<std::string> types = {
        kTypeDevice,   kTypeLight,        kTypeMotion, kTypeTamper, kTypeTemperature,
        kTypeLightLevel, kTypeDevicePower, kTypeButton, kTypeRotary, kTypeConnectivity,
        kTypeRoom,     kTypeZone,         kTypeScene,  kTypeDiscovery,
    };
    return types;
}

bool resourceTypeRequired(std::string_view type)
{
    return type == kTypeDevice || type == kTypeLight;
}

bool Resources::has(std::string_view type) const
{
    const auto it = byType.find(std::string(type));
    return it != byType.end() && it->second.is_array();
}

const Json &Resources::array(std::string_view type) const
{
    static const Json empty = Json::array();
    const auto it = byType.find(std::string(type));
    return (it != byType.end() && it->second.is_array()) ? it->second : empty;
}

std::vector<std::string> Resources::missing() const
{
    std::vector<std::string> out;
    for (const std::string &type : pollResourceTypes()) {
        if (!has(type))
            out.push_back(type);
    }
    return out;
}

std::string ownerDeviceId(const Json &resource)
{
    const Json owner = jsonValue(resource, "owner");
    if (jsonString(owner, "rtype") != "device")
        return {};
    return jsonString(owner, "rid");
}

std::string buttonChannelId(int controlId, bool singleButton)
{
    if (singleButton || controlId <= 0)
        return "button";
    return "button" + str::number(controlId);
}

Snapshot buildSnapshot(const Resources &resources)
{
    Snapshot snapshot;

    for (const Json &deviceObj : resources.array(kTypeDevice)) {
        if (!deviceObj.is_object())
            continue;
        const std::string deviceId = jsonString(deviceObj, "id");
        if (deviceId.empty())
            continue;
        DeviceEntry entry;
        entry.device.externalId = deviceId;
        entry.device.name = deviceName(deviceObj);
        const Json product = jsonValue(deviceObj, "product_data");
        entry.device.manufacturer = jsonString(product, "manufacturer_name", false);
        entry.device.model = jsonString(product, "model_id", false);
        entry.device.firmware = jsonString(product, "software_version", false);
        entry.device.deviceClass = classFromProductData(product);
        entry.device.metaJson = dump(deviceObj);
        applyEffects(entry.device, deviceObj);
        snapshot.devices.emplace(deviceId, std::move(entry));
    }

    for (const Json &lightObj : resources.array(kTypeLight)) {
        if (!lightObj.is_object())
            continue;
        const std::string lightId = jsonString(lightObj, "id");
        const std::string deviceId = ownerDeviceId(lightObj);
        if (lightId.empty() || deviceId.empty())
            continue;
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Light);
        entry.light.resourceId = lightId;

        const Json on = jsonValue(lightObj, "on");
        if (on.is_object() && on.contains("on") && on.at("on").is_boolean()) {
            entry.light.on = on.at("on").get<bool>();
            upsertChannel(entry, makeOnChannel(*entry.light.on), kTypeLight);
        }
        const Json dimming = jsonValue(lightObj, "dimming");
        if (dimming.is_object() && dimming.contains("brightness")) {
            entry.light.brightness = std::clamp(jsonDouble(dimming, "brightness", 0.0), 0.0, 100.0);
            upsertChannel(entry, makeBrightnessChannel(*entry.light.brightness), kTypeLight);
        }
        // A lamp that can do colour temperature has the channel whether or
        // not it is in that mode right now: in colour mode the bridge reports
        // `mirek` as null, and a channel that came and went with the mode
        // was removed and recreated by phi-core on every switch.
        const Json ct = jsonValue(lightObj, "color_temperature");
        if (ct.is_object() && (ct.contains("mirek_schema") || ct.contains("mirek"))) {
            const Json schema = jsonValue(ct, "mirek_schema");
            const int mired = ct.contains("mirek") && ct.at("mirek").is_number()
                ? ct.at("mirek").get<int>()
                : 0;
            v1::Channel channel = makeCtChannel(mired, jsonInt(schema, "mirek_minimum", 153),
                                                jsonInt(schema, "mirek_maximum", 500));
            if (mired > 0) {
                entry.light.mired = mired;
            } else {
                channel.hasValue = false;
                channel.lastValue = v1::ScalarValue{};
            }
            upsertChannel(entry, std::move(channel), kTypeLight);
        }
        const Json color = jsonValue(lightObj, "color");
        const Json xy = jsonValue(color, "xy");
        if (xy.is_object() && !xy.empty()) {
            entry.light.xy = std::make_pair(jsonDouble(xy, "x", 0.0), jsonDouble(xy, "y", 0.0));
            upsertChannel(entry, makeColorChannel(color), kTypeLight);
        }
        applyEffects(entry.device, lightObj);
    }

    // What a device *is* is decided by lights and buttons, before the
    // sensors and the battery it also has get their say.
    std::map<std::string, std::vector<int>> buttonsByDevice;
    for (const Json &obj : resources.array(kTypeButton)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        buttonsByDevice[deviceId].push_back(jsonInt(jsonValue(obj, "metadata"), "control_id", 0));
    }
    for (const auto &[deviceId, controls] : buttonsByDevice) {
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Button);
        const bool single = controls.size() == 1;
        for (const int controlId : controls) {
            const std::string channelId = buttonChannelId(controlId, single);
            v1::Channel channel = readChannel(channelId.c_str(),
                                              single ? "Button" : "",
                                              v1::ChannelKind::ButtonEvent,
                                              v1::ChannelDataType::Int);
            if (!single)
                channel.name = "Button " + str::number(controlId);
            upsertChannel(entry, std::move(channel), kTypeButton);
        }
    }

    // A dial: from the rotary resources, and from a device's own service list
    // when that resource type is late or absent.
    std::set<std::string> rotaryDevices;
    for (const Json &obj : resources.array(kTypeRotary)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (!deviceId.empty())
            rotaryDevices.insert(deviceId);
    }
    for (const Json &deviceObj : resources.array(kTypeDevice)) {
        const std::string deviceId = jsonString(deviceObj, "id");
        const Json services = jsonValue(deviceObj, "services");
        if (deviceId.empty() || !services.is_array())
            continue;
        for (const Json &service : services) {
            if (jsonString(service, "rtype") == kTypeRotary) {
                rotaryDevices.insert(deviceId);
                break;
            }
        }
    }
    for (const std::string &deviceId : rotaryDevices) {
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Button);
        upsertChannel(entry,
                      readChannel("dial", "Dial rotation", v1::ChannelKind::RelativeRotation,
                                  v1::ChannelDataType::Int),
                      kTypeRotary);
    }

    for (const Json &obj : resources.array(kTypeMotion)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Sensor);
        v1::Channel motion = readChannel("motion", "Motion", v1::ChannelKind::Motion,
                                         v1::ChannelDataType::Bool);
        setValue(motion, boolReport(obj, "motion", "motion", "motion_report"));
        upsertChannel(entry, std::move(motion), kTypeMotion);
        if (const auto sensitivity = sensitivityOf(obj)) {
            v1::Channel channel = readChannel("motion_sensitivity", "Motion sensitivity",
                                              v1::ChannelKind::MotionSensitivity,
                                              v1::ChannelDataType::Enum);
            channel.minValue = 1.0;
            channel.maxValue = 5.0;
            channel.stepValue = 1.0;
            channel.metaJson = dump(Json{{"enumName", "SensitivityLevel"}});
            for (const v1::SensitivityLevel level :
                 {v1::SensitivityLevel::Low, v1::SensitivityLevel::Medium, v1::SensitivityLevel::High,
                  v1::SensitivityLevel::VeryHigh, v1::SensitivityLevel::Max})
                addEnumChoice(channel, "SensitivityLevel", level);
            setValue(channel, sensitivity);
            upsertChannel(entry, std::move(channel), kTypeMotion);
        }
    }

    for (const Json &obj : resources.array(kTypeTamper)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Sensor);
        v1::Channel tamper = readChannel("tamper", "Tamper", v1::ChannelKind::Tamper,
                                         v1::ChannelDataType::Bool);
        setValue(tamper, boolReport(obj, "tamper", "tamper", "tamper_report"));
        upsertChannel(entry, std::move(tamper), kTypeTamper);
    }

    for (const Json &obj : resources.array(kTypeTemperature)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Sensor);
        v1::Channel channel = readChannel("temperature", "Temperature", v1::ChannelKind::Temperature,
                                          v1::ChannelDataType::Float);
        channel.unit = "C";
        setValue(channel, temperatureOf(obj));
        upsertChannel(entry, std::move(channel), kTypeTemperature);
    }

    for (const Json &obj : resources.array(kTypeLightLevel)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Sensor);
        v1::Channel channel = readChannel("illuminance", "Illuminance", v1::ChannelKind::Illuminance,
                                          v1::ChannelDataType::Int);
        channel.unit = "lx";
        setValue(channel, luxOf(obj));
        upsertChannel(entry, std::move(channel), kTypeLightLevel);
    }

    for (const Json &obj : resources.array(kTypeDevicePower)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Sensor);
        v1::Channel channel = readChannel("battery", "Battery", v1::ChannelKind::Battery,
                                          v1::ChannelDataType::Int);
        channel.minValue = 0.0;
        channel.maxValue = 100.0;
        channel.stepValue = 1.0;
        setValue(channel, batteryOf(obj));
        upsertChannel(entry, std::move(channel), kTypeDevicePower);
        entry.device.flags |= v1::DeviceFlag::Battery;
    }

    for (const Json &obj : resources.array(kTypeConnectivity)) {
        const std::string deviceId = ownerDeviceId(obj);
        if (deviceId.empty())
            continue;
        // Connectivity alone says nothing about what a device is.
        DeviceEntry &entry = ensureDevice(snapshot, deviceId, v1::DeviceClass::Unknown);
        v1::Channel channel = readChannel("zigbee_status", "Connectivity",
                                          v1::ChannelKind::ConnectivityStatus,
                                          v1::ChannelDataType::Enum);
        for (const v1::ConnectivityStatus status :
             {v1::ConnectivityStatus::Unknown, v1::ConnectivityStatus::Connected,
              v1::ConnectivityStatus::Limited, v1::ConnectivityStatus::Disconnected})
            addEnumChoice(channel, "ConnectivityStatus", status);
        if (const auto status = connectivityOf(obj)) {
            channel.hasValue = true;
            channel.lastValue = static_cast<std::int64_t>(*status);
        }
        upsertChannel(entry, std::move(channel), kTypeConnectivity);
    }

    // A room lists its members as devices; a zone lists them as the services
    // they group - seven `rtype:"light"` children and not one device. Reading
    // only the device children left every zone empty, which is what a group
    // with no members looked like from the outside. The bridge already says
    // who owns a service in each device's own `services` array, so the map is
    // free.
    std::map<std::string, std::string> deviceOfService;
    for (const Json &deviceObj : resources.array(kTypeDevice)) {
        const std::string deviceId = jsonString(deviceObj, "id");
        const Json services = jsonValue(deviceObj, "services");
        if (deviceId.empty() || !services.is_array())
            continue;
        for (const Json &service : services) {
            const std::string rid = jsonString(service, "rid");
            if (!rid.empty())
                deviceOfService[rid] = deviceId;
        }
    }

    std::map<std::string, std::vector<std::string>> memberships;
    const auto collectMembers = [&memberships, &deviceOfService](const Json &array) {
        for (const Json &obj : array) {
            const std::string id = jsonString(obj, "id");
            if (id.empty())
                continue;
            std::vector<std::string> members;
            std::set<std::string> seen;
            const Json children = jsonValue(obj, "children");
            if (children.is_array()) {
                for (const Json &child : children) {
                    const std::string rid = jsonString(child, "rid");
                    if (rid.empty())
                        continue;
                    std::string deviceId;
                    if (jsonString(child, "rtype") == kTypeDevice) {
                        deviceId = rid;
                    } else if (const auto owner = deviceOfService.find(rid);
                               owner != deviceOfService.end()) {
                        // Two services of one device in the same zone are one
                        // member, not two.
                        deviceId = owner->second;
                    }
                    if (deviceId.empty() || !seen.insert(deviceId).second)
                        continue;
                    members.push_back(std::move(deviceId));
                }
            }
            memberships[id] = std::move(members);
        }
    };
    collectMembers(resources.array(kTypeRoom));
    collectMembers(resources.array(kTypeZone));

    for (const Json &obj : resources.array(kTypeRoom)) {
        v1::Room room;
        room.externalId = jsonString(obj, "id");
        if (room.externalId.empty())
            continue;
        room.name = jsonString(jsonValue(obj, "metadata"), "name", false);
        room.zone = "room";
        room.metaJson = dump(obj);
        room.deviceExternalIds = memberships[room.externalId];
        snapshot.rooms.push_back(std::move(room));
    }
    for (const Json &obj : resources.array(kTypeZone)) {
        v1::Group group;
        group.externalId = jsonString(obj, "id");
        if (group.externalId.empty())
            continue;
        group.name = jsonString(jsonValue(obj, "metadata"), "name", false);
        group.zone = "zone";
        group.metaJson = dump(obj);
        group.deviceExternalIds = memberships[group.externalId];
        snapshot.groups.push_back(std::move(group));
    }
    for (const Json &obj : resources.array(kTypeScene)) {
        v1::Scene scene;
        scene.externalId = jsonString(obj, "id");
        if (scene.externalId.empty())
            continue;
        scene.name = jsonString(jsonValue(obj, "metadata"), "name", false);
        const Json group = jsonValue(obj, "group");
        scene.scopeExternalId = jsonString(group, "rid");
        scene.scopeType = jsonString(group, "rtype");
        scene.metaJson = dump(obj);
        snapshot.scenes.push_back(std::move(scene));
    }
    for (const Json &obj : resources.array(kTypeDiscovery)) {
        const std::string id = jsonString(obj, "id");
        if (!id.empty()) {
            snapshot.discoveryResourceId = id;
            break;
        }
    }
    return snapshot;
}

void carryOver(const Snapshot &previous, const std::vector<std::string> &missingTypes,
               Snapshot &next)
{
    if (missingTypes.empty())
        return;
    const std::set<std::string> missing(missingTypes.begin(), missingTypes.end());
    for (auto &[deviceId, entry] : next.devices) {
        const auto before = previous.devices.find(deviceId);
        if (before == previous.devices.end())
            continue;
        for (const v1::Channel &channel : before->second.channels) {
            const auto source = before->second.channelSource.find(channel.externalId);
            if (source == before->second.channelSource.end() || !missing.count(source->second))
                continue;
            if (entry.channel(channel.externalId) != nullptr)
                continue;
            v1::Channel kept = channel;
            // What was reported before stands; nothing new was learned.
            entry.channelSource[kept.externalId] = source->second;
            entry.channels.push_back(std::move(kept));
        }
        if (missing.count(kTypeDevicePower) && v1::hasFlag(before->second.device.flags, v1::DeviceFlag::Battery))
            entry.device.flags |= v1::DeviceFlag::Battery;
        // What the device is was decided by resources this poll may not
        // have seen; the last full answer stands.
        if (before->second.device.deviceClass != v1::DeviceClass::Unknown)
            entry.device.deviceClass = before->second.device.deviceClass;
    }
    if (missing.count(kTypeRoom))
        next.rooms = previous.rooms;
    if (missing.count(kTypeZone))
        next.groups = previous.groups;
    if (missing.count(kTypeScene))
        next.scenes = previous.scenes;
    if (missing.count(kTypeDiscovery))
        next.discoveryResourceId = previous.discoveryResourceId;
}

std::vector<ChannelReport> reportsFor(std::string_view resourceType, const Json &resource)
{
    std::vector<ChannelReport> out;
    if (resourceType == kTypeLight) {
        const Json on = jsonValue(resource, "on");
        if (on.is_object() && on.contains("on") && on.at("on").is_boolean())
            out.push_back({"on", on.at("on").get<bool>()});
        if (const auto brightness = brightnessOf(resource))
            out.push_back({"bri", *brightness});
        const Json ct = jsonValue(resource, "color_temperature");
        if (ct.is_object() && ct.contains("mirek") && ct.at("mirek").is_number()) {
            const int mired = ct.at("mirek").get<int>();
            if (mired > 0)
                out.push_back({"ct", static_cast<std::int64_t>(mired)});
        }
    } else if (resourceType == kTypeMotion) {
        if (const auto motion = boolReport(resource, "motion", "motion", "motion_report"))
            out.push_back({"motion", *motion});
        if (const auto sensitivity = sensitivityOf(resource))
            out.push_back({"motion_sensitivity", *sensitivity});
    } else if (resourceType == kTypeTamper) {
        if (const auto tamper = boolReport(resource, "tamper", "tamper", "tamper_report"))
            out.push_back({"tamper", *tamper});
    } else if (resourceType == kTypeTemperature) {
        if (const auto temperature = temperatureOf(resource))
            out.push_back({"temperature", *temperature});
    } else if (resourceType == kTypeLightLevel) {
        if (const auto lux = luxOf(resource))
            out.push_back({"illuminance", *lux});
    } else if (resourceType == kTypeDevicePower) {
        if (const auto battery = batteryOf(resource))
            out.push_back({"battery", *battery});
    } else if (resourceType == kTypeConnectivity) {
        if (const auto status = connectivityOf(resource))
            out.push_back({"zigbee_status", static_cast<std::int64_t>(*status)});
    }
    return out;
}

std::optional<std::pair<double, double>> xyOf(const Json &lightResource)
{
    const Json xy = jsonValue(jsonValue(lightResource, "color"), "xy");
    if (!xy.is_object() || !xy.contains("x") || !xy.contains("y"))
        return std::nullopt;
    return std::make_pair(jsonDouble(xy, "x", 0.0), jsonDouble(xy, "y", 0.0));
}

std::optional<double> brightnessOf(const Json &lightResource)
{
    const Json dimming = jsonValue(lightResource, "dimming");
    if (!dimming.is_object() || !dimming.contains("brightness") || !dimming.at("brightness").is_number())
        return std::nullopt;
    return std::clamp(dimming.at("brightness").get<double>(), 0.0, 100.0);
}

std::optional<v1::Color> colorOf(const LightState &light)
{
    if (!light.xy)
        return std::nullopt;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    const double brightness = light.brightness ? *light.brightness / 100.0 : 1.0;
    xyToRgb(light.xy->first, light.xy->second, brightness, &r, &g, &b);
    return v1::makeColor(r, g, b);
}

namespace {

std::optional<double> scalarAsDouble(const v1::ScalarValue &value)
{
    if (const auto *d = std::get_if<double>(&value))
        return *d;
    if (const auto *i = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*i);
    if (const auto *b = std::get_if<bool>(&value))
        return *b ? 1.0 : 0.0;
    if (const auto *s = std::get_if<std::string>(&value)) {
        bool ok = false;
        const double parsed = str::toDouble(str::trimmed(*s), &ok);
        if (ok)
            return parsed;
    }
    return std::nullopt;
}

std::optional<bool> scalarAsBool(const v1::ScalarValue &value)
{
    if (const auto *b = std::get_if<bool>(&value))
        return *b;
    if (const auto *i = std::get_if<std::int64_t>(&value))
        return *i != 0;
    if (const auto *d = std::get_if<double>(&value))
        return *d != 0.0;
    if (const auto *s = std::get_if<std::string>(&value)) {
        const std::string text = str::toLower(str::trimmed(*s));
        if (text == "1" || text == "true" || text == "on")
            return true;
        if (text == "0" || text == "false" || text == "off")
            return false;
    }
    return std::nullopt;
}

bool parseHexColor(std::string_view hex, double *r, double *g, double *b)
{
    std::string text = str::trimmed(hex);
    if (!text.empty() && text.front() == '#')
        text.erase(0, 1);
    if (text.size() != 6)
        return false;
    bool ok = false;
    const int value = str::toInt(text, &ok, 16);
    if (!ok)
        return false;
    *r = ((value >> 16) & 0xff) / 255.0;
    *g = ((value >> 8) & 0xff) / 255.0;
    *b = (value & 0xff) / 255.0;
    return true;
}

bool rgbOf(const CommandValue &value, double *r, double *g, double *b)
{
    if (const auto *text = std::get_if<std::string>(&value.scalar)) {
        if (parseHexColor(*text, r, g, b))
            return true;
    }
    Json obj = value.json;
    if (!obj.is_object()) {
        if (const auto *text = std::get_if<std::string>(&value.scalar))
            obj = parseObject(*text);
    }
    if (!obj.is_object())
        return false;
    if (obj.contains("hex") && obj.at("hex").is_string())
        return parseHexColor(obj.at("hex").get<std::string>(), r, g, b);
    if (!obj.contains("r") || !obj.contains("g") || !obj.contains("b"))
        return false;
    const double rr = jsonDouble(obj, "r", -1.0);
    const double gg = jsonDouble(obj, "g", -1.0);
    const double bb = jsonDouble(obj, "b", -1.0);
    if (rr < 0.0 || gg < 0.0 || bb < 0.0)
        return false;
    const bool looks255 = rr > 1.0 || gg > 1.0 || bb > 1.0;
    *r = std::clamp(looks255 ? rr / 255.0 : rr, 0.0, 1.0);
    *g = std::clamp(looks255 ? gg / 255.0 : gg, 0.0, 1.0);
    *b = std::clamp(looks255 ? bb / 255.0 : bb, 0.0, 1.0);
    return true;
}

} // namespace

std::string lightCommandBody(std::string_view channelId, const CommandValue &value,
                             std::string *error)
{
    const auto fail = [error](const char *message) {
        if (error)
            *error = message;
        return std::string();
    };
    Json body = Json::object();
    if (channelId == "on") {
        const auto on = scalarAsBool(value.scalar);
        if (!on)
            return fail("Expected boolean value");
        body["on"] = Json{{"on", *on}};
    } else if (channelId == "bri") {
        const auto number = scalarAsDouble(value.scalar);
        if (!number)
            return fail("Expected numeric brightness");
        // Setting a brightness also switches the lamp on: a bridge that is
        // told to dim to 40 while off does nothing, and the operator meant
        // something. Dimming to nothing switches it off rather than leaving
        // a lamp lit at zero.
        const double brightness = std::clamp(*number, 0.0, 100.0);
        body["on"] = Json{{"on", brightness > 0.0}};
        body["dimming"] = Json{{"brightness", brightness}};
    } else if (channelId == "ct") {
        const auto number = scalarAsDouble(value.scalar);
        if (!number)
            return fail("Expected numeric color temperature");
        body["color_temperature"] =
            Json{{"mirek", static_cast<int>(std::lround(std::clamp(*number, 100.0, 1000.0)))}};
    } else if (channelId == "color") {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        if (!rgbOf(value, &r, &g, &b))
            return fail("Invalid color payload");
        double x = 0.0;
        double y = 0.0;
        rgbToXy(r, g, b, &x, &y);
        body["color"] = Json{{"xy", Json{{"x", x}, {"y", y}}}};
    } else {
        return fail("Unsupported channel");
    }
    if (error)
        error->clear();
    return dump(body);
}

v1::ButtonEventCode buttonEventFor(std::string_view hueEvent)
{
    const std::string event = str::toLower(str::trimmed(hueEvent));
    if (event == "initial_press")
        return v1::ButtonEventCode::InitialPress;
    if (event == "long_press")
        return v1::ButtonEventCode::LongPress;
    if (event == "repeat")
        return v1::ButtonEventCode::Repeat;
    if (event == "short_release")
        return v1::ButtonEventCode::ShortPressRelease;
    if (event == "long_release")
        return v1::ButtonEventCode::LongPressRelease;
    return v1::ButtonEventCode::None;
}

std::optional<v1::ConnectivityStatus> connectivityOf(const Json &resource)
{
    const std::string status = str::toLower(jsonString(resource, "status"));
    if (status == "connected")
        return v1::ConnectivityStatus::Connected;
    if (status == "disconnected")
        return v1::ConnectivityStatus::Disconnected;
    if (status.find("issue") != std::string::npos || status.find("limited") != std::string::npos
        || status.find("degraded") != std::string::npos)
        return v1::ConnectivityStatus::Limited;
    if (!status.empty())
        return v1::ConnectivityStatus::Unknown;
    return std::nullopt;
}

std::int64_t hueTimestampMs(std::string_view text)
{
    // 2026-09-08T14:48:31.123Z, as the bridge writes it.
    const std::string value = str::trimmed(text);
    if (value.empty())
        return 0;
    std::tm tm{};
    const char *rest = ::strptime(value.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
    if (rest == nullptr)
        return 0;
    std::int64_t ms = static_cast<std::int64_t>(::timegm(&tm)) * 1000;
    if (*rest == '.') {
        ++rest;
        int digits = 0;
        int fraction = 0;
        while (*rest >= '0' && *rest <= '9') {
            if (digits < 3) {
                fraction = fraction * 10 + (*rest - '0');
                ++digits;
            }
            ++rest;
        }
        while (digits < 3) {
            fraction *= 10;
            ++digits;
        }
        ms += fraction;
    }
    if (*rest == '+' || *rest == '-') {
        const int sign = *rest == '+' ? 1 : -1;
        int hours = 0;
        int minutes = 0;
        if (std::sscanf(rest + 1, "%2d:%2d", &hours, &minutes) >= 1)
            ms -= sign * (hours * 60 + minutes) * 60000LL;
    }
    return ms;
}

int rotarySteps(const Json &resource, std::int64_t *reportTsMs)
{
    const Json rotary = jsonValue(resource, "relative_rotary");
    const Json report = jsonValue(rotary, "rotary_report");
    Json rotation = jsonValue(jsonValue(rotary, "last_event"), "rotation");
    if (!rotation.is_object() || !rotation.contains("steps"))
        rotation = jsonValue(report, "rotation");
    if (!rotation.is_object() || !rotation.contains("steps"))
        return 0;
    const int steps = std::abs(jsonInt(rotation, "steps", 0));
    if (steps == 0)
        return 0;
    if (reportTsMs)
        *reportTsMs = hueTimestampMs(jsonString(report, "updated"));
    const std::string direction = str::toLower(jsonString(rotation, "direction"));
    if (direction == "counter_clock_wise" || direction == "counter_clockwise" || direction == "ccw")
        return -steps;
    if (direction == "clock_wise" || direction == "clockwise" || direction == "cw")
        return steps;
    return 0;
}

void rgbToXy(double r01, double g01, double b01, double *x, double *y)
{
    const auto gamma = [](double value) {
        if (value <= 0.04045)
            return value / 12.92;
        return std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double r = gamma(std::clamp(r01, 0.0, 1.0));
    const double g = gamma(std::clamp(g01, 0.0, 1.0));
    const double b = gamma(std::clamp(b01, 0.0, 1.0));
    const double X = r * 0.664511 + g * 0.154324 + b * 0.162028;
    const double Y = r * 0.283881 + g * 0.668433 + b * 0.047685;
    const double Z = r * 0.000088 + g * 0.072310 + b * 0.986039;
    const double sum = X + Y + Z;
    if (sum <= 0.0) {
        *x = 0.0;
        *y = 0.0;
        return;
    }
    *x = std::clamp(X / sum, 0.0, 1.0);
    *y = std::clamp(Y / sum, 0.0, 1.0);
}

void xyToRgb(double x, double y, double brightness01, double *r01, double *g01, double *b01)
{
    const double safeY = std::max(y, 1e-6);
    const double Y = std::clamp(brightness01, 0.0, 1.0);
    const double X = (Y / safeY) * x;
    const double Z = (Y / safeY) * (1.0 - x - y);
    double r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
    double g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
    double b = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
    const double maxValue = std::max({r, g, b});
    if (maxValue > 1.0) {
        r /= maxValue;
        g /= maxValue;
        b /= maxValue;
    }
    const auto gamma = [](double value) {
        const double clamped = std::clamp(value, 0.0, 1.0);
        if (clamped <= 0.0031308)
            return 12.92 * clamped;
        return 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
    };
    *r01 = gamma(r);
    *g01 = gamma(g);
    *b01 = gamma(b);
}

} // namespace phicore::hue::ipc
