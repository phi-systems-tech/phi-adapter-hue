#include "hue_model.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include <QJsonDocument>
#include <QJsonObject>

namespace phicore::hue::ipc {

namespace {

namespace v1 = phicore::adapter::v1;

QString deviceNameFromObjects(const QJsonObject &deviceObj)
{
    const QJsonObject metadata = deviceObj.value(QStringLiteral("metadata")).toObject();
    const QString metadataName = metadata.value(QStringLiteral("name")).toString().trimmed();
    if (!metadataName.isEmpty())
        return metadataName;

    const QJsonObject product = deviceObj.value(QStringLiteral("product_data")).toObject();
    const QString productName = product.value(QStringLiteral("product_name")).toString().trimmed();
    if (!productName.isEmpty())
        return productName;

    return QStringLiteral("Hue Device");
}

v1::Channel makeOnChannel(bool value)
{
    v1::Channel channel;
    channel.externalId = "on";
    channel.name = "Power";
    channel.kind = v1::ChannelKind::PowerOnOff;
    channel.dataType = v1::ChannelDataType::Bool;
    channel.flags = v1::kChannelFlagDefaultWrite;
    channel.hasValue = true;
    channel.lastValue = value;
    return channel;
}

v1::Channel makeBrightnessChannel(double value)
{
    v1::Channel channel;
    channel.externalId = "bri";
    channel.name = "Brightness";
    channel.kind = v1::ChannelKind::Brightness;
    channel.dataType = v1::ChannelDataType::Float;
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
    v1::Channel channel;
    channel.externalId = "ct";
    channel.name = "Color temperature";
    channel.kind = v1::ChannelKind::ColorTemperature;
    channel.dataType = v1::ChannelDataType::Int;
    channel.flags = v1::kChannelFlagDefaultWrite;
    channel.unit = "mired";
    channel.minValue = minValue;
    channel.maxValue = maxValue;
    channel.stepValue = 1.0;
    channel.hasValue = true;
    channel.lastValue = static_cast<std::int64_t>(value);
    return channel;
}

v1::Channel makeColorChannel(const QJsonObject &colorObj)
{
    v1::Channel channel;
    channel.externalId = "color";
    channel.name = "Color";
    channel.kind = v1::ChannelKind::ColorRGB;
    channel.dataType = v1::ChannelDataType::Color;
    channel.flags = v1::kChannelFlagDefaultWrite;

    const QJsonObject gamutObj = colorObj.value(QStringLiteral("gamut")).toObject();
    if (!gamutObj.isEmpty()) {
        QJsonArray gamut;
        auto appendPoint = [&gamut](const QJsonObject &point) {
            if (point.isEmpty())
                return;
            QJsonArray arr;
            arr.append(point.value(QStringLiteral("x")).toDouble());
            arr.append(point.value(QStringLiteral("y")).toDouble());
            gamut.append(arr);
        };
        appendPoint(gamutObj.value(QStringLiteral("red")).toObject());
        appendPoint(gamutObj.value(QStringLiteral("green")).toObject());
        appendPoint(gamutObj.value(QStringLiteral("blue")).toObject());

        if (gamut.size() >= 3) {
            QJsonObject caps;
            caps.insert(QStringLiteral("space"), QStringLiteral("cie1931_xy"));
            caps.insert(QStringLiteral("gamut"), gamut);
            channel.metaJson = QJsonDocument(caps).toJson(QJsonDocument::Compact).toStdString();
        }
    }

    return channel;
}

QString ownerDeviceId(const QJsonObject &resourceObj)
{
    const QJsonObject ownerObj = resourceObj.value(QStringLiteral("owner")).toObject();
    if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
        return {};
    return ownerObj.value(QStringLiteral("rid")).toString().trimmed();
}

DeviceEntry &ensureDevice(Snapshot *snapshot, const QString &deviceId, v1::DeviceClass fallbackClass)
{
    auto it = snapshot->devices.find(deviceId);
    if (it == snapshot->devices.end()) {
        DeviceEntry placeholder;
        placeholder.device.externalId = deviceId.toStdString();
        placeholder.device.name = "Hue Device";
        placeholder.device.deviceClass = fallbackClass;
        it = snapshot->devices.insert(deviceId, std::move(placeholder));
    } else if (fallbackClass != v1::DeviceClass::Unknown && it->device.deviceClass == v1::DeviceClass::Unknown) {
        it->device.deviceClass = fallbackClass;
    }

    return it.value();
}

void upsertChannel(v1::ChannelList *channels, v1::Channel channel)
{
    const QString channelId = QString::fromStdString(channel.externalId);
    for (v1::Channel &existing : *channels) {
        if (QString::fromStdString(existing.externalId) != channelId)
            continue;
        existing = std::move(channel);
        return;
    }
    channels->push_back(std::move(channel));
}

v1::Channel makeBoolReadChannel(const char *externalId,
                                const char *name,
                                v1::ChannelKind kind,
                                std::optional<bool> value)
{
    v1::Channel channel;
    channel.externalId = externalId;
    channel.name = name;
    channel.kind = kind;
    channel.dataType = v1::ChannelDataType::Bool;
    channel.flags = v1::kChannelFlagDefaultRead;
    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

v1::Channel makeTemperatureChannel(std::optional<double> value)
{
    v1::Channel channel;
    channel.externalId = "temperature";
    channel.name = "Temperature";
    channel.kind = v1::ChannelKind::Temperature;
    channel.dataType = v1::ChannelDataType::Float;
    channel.flags = v1::kChannelFlagDefaultRead;
    channel.unit = "C";
    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

v1::Channel makeIlluminanceChannel(std::optional<std::int64_t> value)
{
    v1::Channel channel;
    channel.externalId = "illuminance";
    channel.name = "Illuminance";
    channel.kind = v1::ChannelKind::Illuminance;
    channel.dataType = v1::ChannelDataType::Int;
    channel.flags = v1::kChannelFlagDefaultRead;
    channel.unit = "lx";
    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

v1::Channel makeBatteryChannel(std::optional<std::int64_t> value)
{
    v1::Channel channel;
    channel.externalId = "battery";
    channel.name = "Battery";
    channel.kind = v1::ChannelKind::Battery;
    channel.dataType = v1::ChannelDataType::Int;
    channel.flags = v1::kChannelFlagDefaultRead;
    channel.minValue = 0.0;
    channel.maxValue = 100.0;
    channel.stepValue = 1.0;
    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

v1::Channel makeMotionSensitivityChannel(std::optional<std::int64_t> value)
{
    v1::Channel channel;
    channel.externalId = "motion_sensitivity";
    channel.name = "Motion sensitivity";
    channel.kind = v1::ChannelKind::MotionSensitivity;
    channel.dataType = v1::ChannelDataType::Enum;
    channel.flags = v1::kChannelFlagDefaultRead;
    channel.minValue = 1.0;
    channel.maxValue = 5.0;
    channel.stepValue = 1.0;

    QJsonObject meta;
    meta.insert(QStringLiteral("enumName"), QStringLiteral("SensitivityLevel"));
    channel.metaJson = QJsonDocument(meta).toJson(QJsonDocument::Compact).toStdString();

    auto addChoice = [&channel](v1::SensitivityLevel level, const char *label) {
        v1::AdapterConfigOption option;
        option.value = std::to_string(static_cast<int>(level));
        option.label = label;
        channel.choices.push_back(std::move(option));
    };
    addChoice(v1::SensitivityLevel::Low, "Low");
    addChoice(v1::SensitivityLevel::Medium, "Medium");
    addChoice(v1::SensitivityLevel::High, "High");
    addChoice(v1::SensitivityLevel::VeryHigh, "VeryHigh");
    addChoice(v1::SensitivityLevel::Max, "Max");

    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

v1::Channel makeButtonChannel(const QString &channelId,
                              const QString &name,
                              std::optional<std::int64_t> value)
{
    v1::Channel channel;
    channel.externalId = channelId.toStdString();
    channel.name = name.toStdString();
    channel.kind = v1::ChannelKind::ButtonEvent;
    channel.dataType = v1::ChannelDataType::Int;
    channel.flags = v1::kChannelFlagDefaultRead;
    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

v1::Channel makeConnectivityChannel(std::optional<std::int64_t> value)
{
    v1::Channel channel;
    channel.externalId = "zigbee_status";
    channel.name = "Connectivity";
    channel.kind = v1::ChannelKind::ConnectivityStatus;
    channel.dataType = v1::ChannelDataType::Enum;
    channel.flags = v1::kChannelFlagDefaultRead;

    auto addChoice = [&channel](v1::ConnectivityStatus status, const char *label) {
        v1::AdapterConfigOption option;
        option.value = std::to_string(static_cast<int>(status));
        option.label = label;
        channel.choices.push_back(std::move(option));
    };
    addChoice(v1::ConnectivityStatus::Unknown, "Unknown");
    addChoice(v1::ConnectivityStatus::Connected, "Connected");
    addChoice(v1::ConnectivityStatus::Limited, "Limited");
    addChoice(v1::ConnectivityStatus::Disconnected, "Disconnected");

    if (value.has_value()) {
        channel.hasValue = true;
        channel.lastValue = *value;
    }
    return channel;
}

std::optional<bool> parseBoolSensor(const QJsonObject &resourceObj,
                                    const QString &objectKey,
                                    const QString &valueKey,
                                    const QString &reportKey)
{
    const QJsonObject obj = resourceObj.value(objectKey).toObject();
    if (obj.contains(valueKey))
        return obj.value(valueKey).toBool(false);

    const QJsonObject reportObj = obj.value(reportKey).toObject();
    if (reportObj.contains(valueKey))
        return reportObj.value(valueKey).toBool(false);

    return std::nullopt;
}

std::optional<double> parseTemperatureCelsius(const QJsonObject &resourceObj)
{
    const QJsonObject tempObj = resourceObj.value(QStringLiteral("temperature")).toObject();
    double raw = std::numeric_limits<double>::quiet_NaN();
    if (tempObj.contains(QStringLiteral("temperature"))) {
        raw = tempObj.value(QStringLiteral("temperature")).toDouble(std::numeric_limits<double>::quiet_NaN());
    } else {
        const QJsonObject reportObj = tempObj.value(QStringLiteral("temperature_report")).toObject();
        if (reportObj.contains(QStringLiteral("temperature")))
            raw = reportObj.value(QStringLiteral("temperature")).toDouble(std::numeric_limits<double>::quiet_NaN());
    }
    if (!std::isfinite(raw))
        return std::nullopt;

    if (std::abs(raw) > 200.0)
        raw /= 100.0;
    return raw;
}

std::optional<std::int64_t> parseIlluminanceLux(const QJsonObject &resourceObj)
{
    const QJsonObject lightObj = resourceObj.value(QStringLiteral("light")).toObject();
    const QJsonObject reportObj = lightObj.value(QStringLiteral("light_level_report")).toObject();

    if (reportObj.contains(QStringLiteral("lux"))) {
        const double lux = reportObj.value(QStringLiteral("lux")).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(lux))
            return static_cast<std::int64_t>(std::llround(lux));
    }
    if (lightObj.contains(QStringLiteral("lux"))) {
        const double lux = lightObj.value(QStringLiteral("lux")).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (std::isfinite(lux))
            return static_cast<std::int64_t>(std::llround(lux));
    }

    auto lightLevelToLux = [](int lightLevel) {
        return std::pow(10.0, (static_cast<double>(lightLevel) - 1.0) / 10000.0);
    };
    if (reportObj.contains(QStringLiteral("light_level"))) {
        return static_cast<std::int64_t>(
            std::llround(lightLevelToLux(reportObj.value(QStringLiteral("light_level")).toInt(0))));
    }
    if (lightObj.contains(QStringLiteral("light_level"))) {
        return static_cast<std::int64_t>(
            std::llround(lightLevelToLux(lightObj.value(QStringLiteral("light_level")).toInt(0))));
    }

    return std::nullopt;
}

std::optional<std::int64_t> parseBatteryLevel(const QJsonObject &resourceObj)
{
    const QJsonObject powerObj = resourceObj.value(QStringLiteral("power_state")).toObject();
    if (!powerObj.contains(QStringLiteral("battery_level")))
        return std::nullopt;

    const int level = powerObj.value(QStringLiteral("battery_level")).toInt(-1);
    if (level < 0)
        return std::nullopt;
    return static_cast<std::int64_t>(std::clamp(level, 0, 100));
}

std::optional<std::int64_t> parseMotionSensitivity(const QJsonObject &resourceObj)
{
    const QJsonObject sensitivityObj = resourceObj.value(QStringLiteral("sensitivity")).toObject();
    if (!sensitivityObj.contains(QStringLiteral("sensitivity")))
        return std::nullopt;

    const int raw = sensitivityObj.value(QStringLiteral("sensitivity")).toInt(0);
    switch (raw) {
    case 1:
        return static_cast<std::int64_t>(v1::SensitivityLevel::Low);
    case 2:
        return static_cast<std::int64_t>(v1::SensitivityLevel::Medium);
    case 3:
        return static_cast<std::int64_t>(v1::SensitivityLevel::High);
    case 4:
        return static_cast<std::int64_t>(v1::SensitivityLevel::VeryHigh);
    default:
        break;
    }
    return std::nullopt;
}

std::optional<std::int64_t> parseConnectivityStatus(const QJsonObject &resourceObj)
{
    const QString status = resourceObj.value(QStringLiteral("status")).toString().trimmed().toLower();
    if (status == QLatin1String("connected"))
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Connected);
    if (status == QLatin1String("disconnected"))
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Disconnected);
    if (status.contains(QStringLiteral("issue"))
        || status.contains(QStringLiteral("limited"))
        || status.contains(QStringLiteral("degraded"))) {
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Limited);
    }
    if (!status.isEmpty())
        return static_cast<std::int64_t>(v1::ConnectivityStatus::Unknown);
    return std::nullopt;
}

std::optional<std::int64_t> mapButtonEventCode(const QString &eventRaw)
{
    const QString event = eventRaw.trimmed().toLower();
    if (event == QLatin1String("initial_press"))
        return static_cast<std::int64_t>(v1::ButtonEventCode::InitialPress);
    if (event == QLatin1String("long_press"))
        return static_cast<std::int64_t>(v1::ButtonEventCode::LongPress);
    if (event == QLatin1String("repeat"))
        return static_cast<std::int64_t>(v1::ButtonEventCode::Repeat);
    if (event == QLatin1String("short_release"))
        return static_cast<std::int64_t>(v1::ButtonEventCode::ShortPressRelease);
    if (event == QLatin1String("long_release"))
        return static_cast<std::int64_t>(v1::ButtonEventCode::LongPressRelease);
    return std::nullopt;
}

bool parseHexColor(const QString &hex, double *r01, double *g01, double *b01)
{
    QString text = hex.trimmed();
    if (text.startsWith(QLatin1Char('#')))
        text.remove(0, 1);
    if (text.size() != 6)
        return false;

    bool ok = false;
    const int value = text.toInt(&ok, 16);
    if (!ok)
        return false;

    const int r = (value >> 16) & 0xff;
    const int g = (value >> 8) & 0xff;
    const int b = value & 0xff;

    *r01 = static_cast<double>(r) / 255.0;
    *g01 = static_cast<double>(g) / 255.0;
    *b01 = static_cast<double>(b) / 255.0;
    return true;
}

std::optional<double> scalarAsDouble(const v1::ScalarValue &value)
{
    if (const auto *d = std::get_if<double>(&value))
        return *d;
    if (const auto *i = std::get_if<std::int64_t>(&value))
        return static_cast<double>(*i);
    if (const auto *b = std::get_if<bool>(&value))
        return *b ? 1.0 : 0.0;
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
        const QString text = QString::fromStdString(*s).trimmed().toLower();
        if (text == QLatin1String("1") || text == QLatin1String("true") || text == QLatin1String("on"))
            return true;
        if (text == QLatin1String("0") || text == QLatin1String("false") || text == QLatin1String("off"))
            return false;
    }
    return std::nullopt;
}

bool extractRgb(const phicore::adapter::sdk::ChannelInvokeRequest &request,
                double *r01,
                double *g01,
                double *b01)
{
    if (request.hasScalarValue) {
        if (const auto *text = std::get_if<std::string>(&request.value))
            return parseHexColor(QString::fromStdString(*text), r01, g01, b01);
    }

    if (!request.valueJson.empty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(request.valueJson));
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            if (obj.contains(QStringLiteral("hex")))
                return parseHexColor(obj.value(QStringLiteral("hex")).toString(), r01, g01, b01);

            const double r = obj.value(QStringLiteral("r")).toDouble(std::numeric_limits<double>::quiet_NaN());
            const double g = obj.value(QStringLiteral("g")).toDouble(std::numeric_limits<double>::quiet_NaN());
            const double b = obj.value(QStringLiteral("b")).toDouble(std::numeric_limits<double>::quiet_NaN());
            if (std::isfinite(r) && std::isfinite(g) && std::isfinite(b)) {
                const bool looks255 = (r > 1.0 || g > 1.0 || b > 1.0);
                *r01 = looks255 ? std::clamp(r / 255.0, 0.0, 1.0) : std::clamp(r, 0.0, 1.0);
                *g01 = looks255 ? std::clamp(g / 255.0, 0.0, 1.0) : std::clamp(g, 0.0, 1.0);
                *b01 = looks255 ? std::clamp(b / 255.0, 0.0, 1.0) : std::clamp(b, 0.0, 1.0);
                return true;
            }
        }
    }

    return false;
}

} // namespace

Snapshot buildSnapshot(const QJsonArray &deviceData,
                       const QJsonArray &lightData,
                       const QJsonArray &motionData,
                       const QJsonArray &tamperData,
                       const QJsonArray &temperatureData,
                       const QJsonArray &lightLevelData,
                       const QJsonArray &devicePowerData,
                       const QJsonArray &buttonData,
                       const QJsonArray &zigbeeConnectivityData,
                       const QJsonArray &roomData,
                       const QJsonArray &zoneData,
                       const QJsonArray &sceneData)
{
    Snapshot snapshot;

    for (const QJsonValue &entry : deviceData) {
        if (!entry.isObject())
            continue;
        const QJsonObject deviceObj = entry.toObject();
        const QString deviceId = deviceObj.value(QStringLiteral("id")).toString().trimmed();
        if (deviceId.isEmpty())
            continue;

        DeviceEntry deviceEntry;
        deviceEntry.device.externalId = deviceId.toStdString();
        deviceEntry.device.name = deviceNameFromObjects(deviceObj).toStdString();

        const QJsonObject product = deviceObj.value(QStringLiteral("product_data")).toObject();
        deviceEntry.device.manufacturer = product.value(QStringLiteral("manufacturer_name")).toString().toStdString();
        deviceEntry.device.model = product.value(QStringLiteral("model_id")).toString().toStdString();
        deviceEntry.device.firmware = product.value(QStringLiteral("software_version")).toString().toStdString();
        deviceEntry.device.deviceClass = v1::DeviceClass::Unknown;
        deviceEntry.device.metaJson = QJsonDocument(deviceObj).toJson(QJsonDocument::Compact).toStdString();

        snapshot.devices.insert(deviceId, std::move(deviceEntry));
    }

    for (const QJsonValue &entry : lightData) {
        if (!entry.isObject())
            continue;
        const QJsonObject lightObj = entry.toObject();
        const QString lightId = lightObj.value(QStringLiteral("id")).toString().trimmed();
        if (lightId.isEmpty())
            continue;

        const QString deviceId = ownerDeviceId(lightObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Light);
        device.state.lightResourceId = lightId;

        const QJsonObject onObj = lightObj.value(QStringLiteral("on")).toObject();
        if (onObj.contains(QStringLiteral("on"))) {
            const bool on = onObj.value(QStringLiteral("on")).toBool(false);
            device.state.hasOn = true;
            device.state.on = on;
            upsertChannel(&device.channels, makeOnChannel(on));
        }

        const QJsonObject dimObj = lightObj.value(QStringLiteral("dimming")).toObject();
        if (dimObj.contains(QStringLiteral("brightness"))) {
            const double bri = dimObj.value(QStringLiteral("brightness")).toDouble(0.0);
            device.state.hasBrightness = true;
            device.state.brightness = std::clamp(bri, 0.0, 100.0);
            upsertChannel(&device.channels, makeBrightnessChannel(device.state.brightness));
        }

        const QJsonObject ctObj = lightObj.value(QStringLiteral("color_temperature")).toObject();
        if (ctObj.contains(QStringLiteral("mirek"))) {
            const int ct = ctObj.value(QStringLiteral("mirek")).toInt(0);
            if (ct > 0) {
                const QJsonObject schema = ctObj.value(QStringLiteral("mirek_schema")).toObject();
                const int ctMin = schema.value(QStringLiteral("mirek_minimum")).toInt(153);
                const int ctMax = schema.value(QStringLiteral("mirek_maximum")).toInt(500);
                device.state.hasColorTemperature = true;
                device.state.colorTemperatureMired = ct;
                upsertChannel(&device.channels, makeCtChannel(ct, ctMin, ctMax));
            }
        }

        const QJsonObject colorObj = lightObj.value(QStringLiteral("color")).toObject();
        const QJsonObject xyObj = colorObj.value(QStringLiteral("xy")).toObject();
        if (!xyObj.isEmpty()) {
            device.state.hasColorXy = true;
            device.state.colorX = xyObj.value(QStringLiteral("x")).toDouble(0.0);
            device.state.colorY = xyObj.value(QStringLiteral("y")).toDouble(0.0);
            upsertChannel(&device.channels, makeColorChannel(colorObj));
        }
    }

    for (const QJsonValue &entry : motionData) {
        if (!entry.isObject())
            continue;
        const QJsonObject motionObj = entry.toObject();
        const QString deviceId = ownerDeviceId(motionObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Sensor);
        upsertChannel(&device.channels,
                      makeBoolReadChannel("motion",
                                          "Motion",
                                          v1::ChannelKind::Motion,
                                          parseBoolSensor(motionObj,
                                                          QStringLiteral("motion"),
                                                          QStringLiteral("motion"),
                                                          QStringLiteral("motion_report"))));

        const std::optional<std::int64_t> sensitivity = parseMotionSensitivity(motionObj);
        if (sensitivity.has_value())
            upsertChannel(&device.channels, makeMotionSensitivityChannel(sensitivity));
    }

    for (const QJsonValue &entry : tamperData) {
        if (!entry.isObject())
            continue;
        const QJsonObject tamperObj = entry.toObject();
        const QString deviceId = ownerDeviceId(tamperObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Sensor);
        upsertChannel(&device.channels,
                      makeBoolReadChannel("tamper",
                                          "Tamper",
                                          v1::ChannelKind::Tamper,
                                          parseBoolSensor(tamperObj,
                                                          QStringLiteral("tamper"),
                                                          QStringLiteral("tamper"),
                                                          QStringLiteral("tamper_report"))));
    }

    for (const QJsonValue &entry : temperatureData) {
        if (!entry.isObject())
            continue;
        const QJsonObject temperatureObj = entry.toObject();
        const QString deviceId = ownerDeviceId(temperatureObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Sensor);
        upsertChannel(&device.channels, makeTemperatureChannel(parseTemperatureCelsius(temperatureObj)));
    }

    for (const QJsonValue &entry : lightLevelData) {
        if (!entry.isObject())
            continue;
        const QJsonObject lightLevelObj = entry.toObject();
        const QString deviceId = ownerDeviceId(lightLevelObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Sensor);
        upsertChannel(&device.channels, makeIlluminanceChannel(parseIlluminanceLux(lightLevelObj)));
    }

    for (const QJsonValue &entry : devicePowerData) {
        if (!entry.isObject())
            continue;
        const QJsonObject powerObj = entry.toObject();
        const QString deviceId = ownerDeviceId(powerObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Sensor);
        const std::optional<std::int64_t> battery = parseBatteryLevel(powerObj);
        upsertChannel(&device.channels, makeBatteryChannel(battery));
        device.device.flags |= v1::DeviceFlag::Battery;
    }

    struct ButtonEntry {
        int controlId = 0;
        std::optional<std::int64_t> eventCode;
    };
    QHash<QString, std::vector<ButtonEntry>> buttonsByDevice;

    for (const QJsonValue &entry : buttonData) {
        if (!entry.isObject())
            continue;
        const QJsonObject buttonObj = entry.toObject();
        const QString deviceId = ownerDeviceId(buttonObj);
        if (deviceId.isEmpty())
            continue;

        const QJsonObject metadataObj = buttonObj.value(QStringLiteral("metadata")).toObject();
        const int controlId = metadataObj.value(QStringLiteral("control_id")).toInt(0);

        const QJsonObject buttonStateObj = buttonObj.value(QStringLiteral("button")).toObject();
        QString event = buttonStateObj.value(QStringLiteral("last_event")).toString();
        if (event.isEmpty()) {
            const QJsonObject reportObj = buttonStateObj.value(QStringLiteral("button_report")).toObject();
            event = reportObj.value(QStringLiteral("event")).toString();
        }

        ButtonEntry mapped;
        mapped.controlId = controlId;
        mapped.eventCode = mapButtonEventCode(event);
        buttonsByDevice[deviceId].push_back(std::move(mapped));
    }

    for (auto it = buttonsByDevice.cbegin(); it != buttonsByDevice.cend(); ++it) {
        const QString deviceId = it.key();
        const std::vector<ButtonEntry> &entries = it.value();
        if (entries.empty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Button);
        const bool singleButton = (entries.size() == 1);
        for (const ButtonEntry &entry : entries) {
            QString channelId = QStringLiteral("button");
            QString channelName = QStringLiteral("Button");
            if (!singleButton && entry.controlId > 0) {
                channelId = QStringLiteral("button%1").arg(entry.controlId);
                channelName = QStringLiteral("Button %1").arg(entry.controlId);
            }

            upsertChannel(&device.channels,
                          makeButtonChannel(channelId,
                                            channelName,
                                            entry.eventCode));
        }
    }

    for (const QJsonValue &entry : zigbeeConnectivityData) {
        if (!entry.isObject())
            continue;
        const QJsonObject connectivityObj = entry.toObject();
        const QString deviceId = ownerDeviceId(connectivityObj);
        if (deviceId.isEmpty())
            continue;

        DeviceEntry &device = ensureDevice(&snapshot, deviceId, v1::DeviceClass::Sensor);
        upsertChannel(&device.channels, makeConnectivityChannel(parseConnectivityStatus(connectivityObj)));
    }

    QHash<QString, QStringList> memberships;
    auto collectMemberships = [&memberships](const QJsonArray &arr) {
        for (const QJsonValue &entry : arr) {
            if (!entry.isObject())
                continue;
            const QJsonObject obj = entry.toObject();
            const QString id = obj.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty())
                continue;

            QStringList members;
            const QJsonArray children = obj.value(QStringLiteral("children")).toArray();
            for (const QJsonValue &childValue : children) {
                if (!childValue.isObject())
                    continue;
                const QJsonObject child = childValue.toObject();
                if (child.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                    continue;
                const QString rid = child.value(QStringLiteral("rid")).toString().trimmed();
                if (!rid.isEmpty())
                    members.push_back(rid);
            }
            memberships.insert(id, members);
        }
    };

    collectMemberships(roomData);
    collectMemberships(zoneData);

    for (const QJsonValue &entry : roomData) {
        if (!entry.isObject())
            continue;
        const QJsonObject roomObj = entry.toObject();

        v1::Room room;
        room.externalId = roomObj.value(QStringLiteral("id")).toString().toStdString();
        room.name = roomObj.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("name")).toString().toStdString();
        room.zone = "room";
        room.metaJson = QJsonDocument(roomObj).toJson(QJsonDocument::Compact).toStdString();

        const QStringList memberIds = memberships.value(QString::fromStdString(room.externalId));
        for (const QString &memberId : memberIds)
            room.deviceExternalIds.push_back(memberId.toStdString());

        if (!room.externalId.empty())
            snapshot.rooms.push_back(std::move(room));
    }

    for (const QJsonValue &entry : zoneData) {
        if (!entry.isObject())
            continue;
        const QJsonObject zoneObj = entry.toObject();

        v1::Group group;
        group.externalId = zoneObj.value(QStringLiteral("id")).toString().toStdString();
        group.name = zoneObj.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("name")).toString().toStdString();
        group.zone = "zone";
        group.metaJson = QJsonDocument(zoneObj).toJson(QJsonDocument::Compact).toStdString();

        const QStringList memberIds = memberships.value(QString::fromStdString(group.externalId));
        for (const QString &memberId : memberIds)
            group.deviceExternalIds.push_back(memberId.toStdString());

        if (!group.externalId.empty())
            snapshot.groups.push_back(std::move(group));
    }

    for (const QJsonValue &entry : sceneData) {
        if (!entry.isObject())
            continue;
        const QJsonObject sceneObj = entry.toObject();

        v1::Scene scene;
        scene.externalId = sceneObj.value(QStringLiteral("id")).toString().toStdString();
        scene.name = sceneObj.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("name")).toString().toStdString();
        const QJsonObject group = sceneObj.value(QStringLiteral("group")).toObject();
        scene.scopeExternalId = group.value(QStringLiteral("rid")).toString().toStdString();
        scene.scopeType = group.value(QStringLiteral("rtype")).toString().toStdString();
        scene.metaJson = QJsonDocument(sceneObj).toJson(QJsonDocument::Compact).toStdString();

        if (!scene.externalId.empty())
            snapshot.scenes.push_back(std::move(scene));
    }

    return snapshot;
}

void rgbToXy(double r01, double g01, double b01, double *x, double *y)
{
    auto gamma = [](double value) {
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

QByteArray buildLightCommandPayload(const QString &channelExternalId,
                                    const phicore::adapter::sdk::ChannelInvokeRequest &request,
                                    QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QByteArray();
    };

    QJsonObject body;

    if (channelExternalId == QLatin1String("on")) {
        if (!request.hasScalarValue)
            return fail(QStringLiteral("Expected boolean value"));
        const auto value = scalarAsBool(request.value);
        if (!value.has_value())
            return fail(QStringLiteral("Invalid boolean value"));

        QJsonObject onObj;
        onObj.insert(QStringLiteral("on"), *value);
        body.insert(QStringLiteral("on"), onObj);
    } else if (channelExternalId == QLatin1String("bri")) {
        if (!request.hasScalarValue)
            return fail(QStringLiteral("Expected numeric brightness"));
        const auto value = scalarAsDouble(request.value);
        if (!value.has_value())
            return fail(QStringLiteral("Invalid brightness value"));

        const double brightness = std::clamp(*value, 0.0, 100.0);
        QJsonObject onObj;
        onObj.insert(QStringLiteral("on"), brightness > 0.0);
        body.insert(QStringLiteral("on"), onObj);

        QJsonObject dimObj;
        dimObj.insert(QStringLiteral("brightness"), brightness);
        body.insert(QStringLiteral("dimming"), dimObj);
    } else if (channelExternalId == QLatin1String("ct")) {
        if (!request.hasScalarValue)
            return fail(QStringLiteral("Expected numeric color temperature"));
        const auto value = scalarAsDouble(request.value);
        if (!value.has_value())
            return fail(QStringLiteral("Invalid color temperature value"));

        const int mired = static_cast<int>(std::round(std::clamp(*value, 100.0, 1000.0)));
        QJsonObject ctObj;
        ctObj.insert(QStringLiteral("mirek"), mired);
        body.insert(QStringLiteral("color_temperature"), ctObj);
    } else if (channelExternalId == QLatin1String("color")) {
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
        if (!extractRgb(request, &r, &g, &b))
            return fail(QStringLiteral("Invalid color payload"));

        double x = 0.0;
        double y = 0.0;
        rgbToXy(r, g, b, &x, &y);

        QJsonObject xyObj;
        xyObj.insert(QStringLiteral("x"), x);
        xyObj.insert(QStringLiteral("y"), y);

        QJsonObject colorObj;
        colorObj.insert(QStringLiteral("xy"), xyObj);
        body.insert(QStringLiteral("color"), colorObj);
    } else {
        return fail(QStringLiteral("Unsupported channel"));
    }

    if (body.isEmpty())
        return fail(QStringLiteral("Empty command payload"));

    if (error)
        error->clear();
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

} // namespace phicore::hue::ipc
