// adapters/plugins/hue/hueadapter.cpp
#include "hueadapter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QDebug>
#include <QLoggingCategory>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>
#include <cmath>
#include <initializer_list>
#include <utility>
#if QT_CONFIG(ssl)
#include <QSslConfiguration>
#include <QSslSocket>
#endif
#include "types.h"
#include "color.h"
#include "deviceupdate.h"

Q_LOGGING_CATEGORY(adapterLog, "phi-core.adapters.hue");

namespace {
constexpr int kMaxConcurrentV2DeviceFetch = 4;
constexpr int kButtonMultiPressWindowMs = 1200;
constexpr int kButtonMultiPressResetGapMs = 500;
constexpr int kInitialSnapshotDelayMs = 300;
constexpr int kV2DeviceFetchSpacingMs = 20;
constexpr int kRenameVerifyDelayMs = 700;
constexpr int kRenameVerifyMaxAttempts = 3;
constexpr int kMaxV2ResourceSnapshotRetries = 3;
constexpr int kV2ResourceRetryBaseDelayMs = 1000;
constexpr auto kZigbeeStatusChannelId = "zigbee_status";
constexpr auto kDeviceSoftwareUpdateChannelId = "device_software_update";

QString channelBindingKey(const QString &deviceExtId, const QString &channelExtId)
{
    return deviceExtId + QLatin1Char('|') + channelExtId;
}

int mapHueSensitivityToLevel(int raw)
{
    switch (raw) {
    case 1:
        return static_cast<int>(phicore::SensitivityLevel::Low);
    case 2:
        return static_cast<int>(phicore::SensitivityLevel::Medium);
    case 3:
        return static_cast<int>(phicore::SensitivityLevel::High);
    case 4:
        return static_cast<int>(phicore::SensitivityLevel::VeryHigh);
    default:
        return static_cast<int>(phicore::SensitivityLevel::Unknown);
    }
}

QString sensitivityLabel(int value)
{
    switch (value) {
    case static_cast<int>(phicore::SensitivityLevel::Low):
        return QStringLiteral("Low");
    case static_cast<int>(phicore::SensitivityLevel::Medium):
        return QStringLiteral("Medium");
    case static_cast<int>(phicore::SensitivityLevel::High):
        return QStringLiteral("High");
    case static_cast<int>(phicore::SensitivityLevel::VeryHigh):
        return QStringLiteral("VeryHigh");
    case static_cast<int>(phicore::SensitivityLevel::Max):
        return QStringLiteral("Max");
    default:
        return QString();
    }
}

QString resourceBindingKey(const QString &resourceType, const QString &resourceId)
{
    return resourceType + QLatin1Char('|') + resourceId;
}

QJsonObject buildServiceRefMap(const QJsonArray &services)
{
    QHash<QString, QStringList> refs;
    for (const QJsonValue &val : services) {
        if (!val.isObject())
            continue;
        const QJsonObject obj = val.toObject();
        const QString type = obj.value(QStringLiteral("rtype")).toString();
        const QString rid  = obj.value(QStringLiteral("rid")).toString();
        if (type.isEmpty() || rid.isEmpty())
            continue;
        refs[type].append(rid);
    }

    QJsonObject map;
    for (auto it = refs.constBegin(); it != refs.constEnd(); ++it) {
        if (it.value().size() == 1) {
            map.insert(it.key(), it.value().constFirst());
            continue;
        }
        QJsonArray arr;
        for (const QString &rid : it.value())
            arr.append(rid);
        map.insert(it.key(), arr);
    }
    return map;
}

void attachServiceRefs(QJsonObject &meta)
{
    const QJsonArray services = meta.value(QStringLiteral("services")).toArray();
    if (services.isEmpty())
        return;
    const QJsonObject refs = buildServiceRefMap(services);
    if (!refs.isEmpty())
        meta.insert(QStringLiteral("serviceRefs"), refs);
}

QString firstNonEmptyString(const QJsonObject &obj, std::initializer_list<QString> keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = obj.value(key);
        if (value.isString()) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty())
                return text;
        } else if (value.isObject()) {
            const QJsonObject nested = value.toObject();
            const QString nestedValue = nested.value(QStringLiteral("value")).toString().trimmed();
            if (!nestedValue.isEmpty())
                return nestedValue;
            const QString nestedName = nested.value(QStringLiteral("name")).toString().trimmed();
            if (!nestedName.isEmpty())
                return nestedName;
        }
    }
    return {};
}

QString serviceRefFromMeta(const QJsonObject &meta, const QString &refKey)
{
    const QJsonObject refs = meta.value(QStringLiteral("serviceRefs")).toObject();
    if (refs.isEmpty())
        return {};
    const QJsonValue entry = refs.value(refKey);
    if (entry.isString()) {
        const QString text = entry.toString().trimmed();
        if (!text.isEmpty())
            return text;
        return {};
    }
    if (entry.isArray()) {
        const QJsonArray arr = entry.toArray();
        for (const QJsonValue &val : arr) {
            if (!val.isString())
                continue;
            const QString text = val.toString().trimmed();
            if (!text.isEmpty())
                return text;
        }
    }
    return {};
}

phicore::DeviceSoftwareUpdateStatus deviceSoftwareUpdateStatusFromString(const QString &value)
{
    const QString normalized = value.toLower();
    if (normalized.contains(QStringLiteral("up")) && normalized.contains(QStringLiteral("date")))
        return phicore::DeviceSoftwareUpdateStatus::UpToDate;
    if (normalized.contains(QStringLiteral("ready")) || normalized.contains(QStringLiteral("available")))
        return phicore::DeviceSoftwareUpdateStatus::UpdateAvailable;
    if (normalized.contains(QStringLiteral("download")))
        return phicore::DeviceSoftwareUpdateStatus::Downloading;
    if (normalized.contains(QStringLiteral("install")) && !normalized.contains(QStringLiteral("ready")))
        return phicore::DeviceSoftwareUpdateStatus::Installing;
    if (normalized.contains(QStringLiteral("reboot")) || normalized.contains(QStringLiteral("restart")))
        return phicore::DeviceSoftwareUpdateStatus::RebootRequired;
    if (normalized.contains(QStringLiteral("fail")))
        return phicore::DeviceSoftwareUpdateStatus::Failed;
    return phicore::DeviceSoftwareUpdateStatus::Unknown;
}

QString deviceSoftwareUpdateStatusToString(phicore::DeviceSoftwareUpdateStatus status)
{
    switch (status) {
    case phicore::DeviceSoftwareUpdateStatus::UpToDate:
        return QStringLiteral("UpToDate");
    case phicore::DeviceSoftwareUpdateStatus::UpdateAvailable:
        return QStringLiteral("UpdateAvailable");
    case phicore::DeviceSoftwareUpdateStatus::Downloading:
        return QStringLiteral("Downloading");
    case phicore::DeviceSoftwareUpdateStatus::Installing:
        return QStringLiteral("Installing");
    case phicore::DeviceSoftwareUpdateStatus::RebootRequired:
        return QStringLiteral("RebootRequired");
    case phicore::DeviceSoftwareUpdateStatus::Failed:
        return QStringLiteral("Failed");
    default:
        return QStringLiteral("Unknown");
    }
}

phicore::ConnectivityStatus connectivityStatusFromString(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("connected"))
        return phicore::ConnectivityStatus::Connected;
    if (normalized == QLatin1String("disconnected"))
        return phicore::ConnectivityStatus::Disconnected;
    if (normalized.contains(QStringLiteral("issue"))
        || normalized.contains(QStringLiteral("limited"))
        || normalized.contains(QStringLiteral("degraded"))) {
        return phicore::ConnectivityStatus::Limited;
    }
    return phicore::ConnectivityStatus::Unknown;
}

phicore::DeviceSoftwareUpdate buildDeviceSoftwareUpdate(const QJsonObject &resObj, qint64 tsMs)
{
    phicore::DeviceSoftwareUpdate info;
    info.statusRaw = firstNonEmptyString(resObj, {QStringLiteral("state"), QStringLiteral("status")});
    info.status = deviceSoftwareUpdateStatusFromString(info.statusRaw);
    info.currentVersion = firstNonEmptyString(resObj, {
        QStringLiteral("current_version"),
        QStringLiteral("currentVersion"),
        QStringLiteral("version"),
        QStringLiteral("firmware"),
        QStringLiteral("installed_version")
    });
    info.targetVersion = firstNonEmptyString(resObj, {
        QStringLiteral("target_version"),
        QStringLiteral("targetVersion"),
        QStringLiteral("available_version"),
        QStringLiteral("availableVersion"),
        QStringLiteral("version_available")
    });
    info.releaseNotesUrl = firstNonEmptyString(resObj, {
        QStringLiteral("release_notes_url"),
        QStringLiteral("releaseNotesUrl"),
        QStringLiteral("release_notes"),
        QStringLiteral("releaseNotes")
    });
    info.message = firstNonEmptyString(resObj, {
        QStringLiteral("message"),
        QStringLiteral("description"),
        QStringLiteral("details")
    });
    info.payloadId = firstNonEmptyString(resObj, {
        QStringLiteral("id"),
        QStringLiteral("rid"),
        QStringLiteral("package_id")
    });
    info.timestampMs = tsMs;
    return info;
}

QJsonObject deviceSoftwareUpdateToJson(const phicore::DeviceSoftwareUpdate &info)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("status"), deviceSoftwareUpdateStatusToString(info.status));
    if (!info.statusRaw.isEmpty())
        obj.insert(QStringLiteral("statusRaw"), info.statusRaw);
    if (!info.currentVersion.isEmpty())
        obj.insert(QStringLiteral("currentVersion"), info.currentVersion);
    if (!info.targetVersion.isEmpty())
        obj.insert(QStringLiteral("targetVersion"), info.targetVersion);
    if (!info.releaseNotesUrl.isEmpty())
        obj.insert(QStringLiteral("releaseNotesUrl"), info.releaseNotesUrl);
    if (!info.message.isEmpty())
        obj.insert(QStringLiteral("message"), info.message);
    if (!info.payloadId.isEmpty())
        obj.insert(QStringLiteral("payloadId"), info.payloadId);
    if (info.timestampMs > 0)
        obj.insert(QStringLiteral("timestampMs"), info.timestampMs);
    return obj;
}

phicore::DeviceClass classifyDeviceString(const QString &text)
{
    if (text.isEmpty())
        return phicore::DeviceClass::Unknown;
    const QString lower = text.toLower();
    if (lower.contains(QStringLiteral("plug")))
        return phicore::DeviceClass::Plug;
    if (lower.contains(QStringLiteral("sensor")))
        return phicore::DeviceClass::Sensor;
    if (lower.contains(QStringLiteral("switch")))
        return phicore::DeviceClass::Switch;
    if (lower.contains(QStringLiteral("bridge")) || lower.contains(QStringLiteral("gateway")))
        return phicore::DeviceClass::Gateway;
    return phicore::DeviceClass::Unknown;
}

void applyDeviceClassFromMetadata(phicore::Device &device,
                                  const QJsonObject &metaObj,
                                  const QJsonObject &productObj)
{
    const QStringList candidates = {
        productObj.value(QStringLiteral("product_archetype")).toString(),
        productObj.value(QStringLiteral("product_name")).toString(),
        metaObj.value(QStringLiteral("archetype")).toString(),
        metaObj.value(QStringLiteral("name")).toString()
    };

    for (const QString &text : candidates) {
        const phicore::DeviceClass cls = classifyDeviceString(text);
        if (cls == phicore::DeviceClass::Unknown)
            continue;
        if (device.deviceClass == phicore::DeviceClass::Unknown
            || device.deviceClass == phicore::DeviceClass::Light) {
            device.deviceClass = cls;
        }
        break;
    }
}

QString beautifyHueEffectLabel(const QString &effect)
{
    if (effect.isEmpty())
        return effect;

    QString label = effect;
    label.replace(QLatin1Char('_'), QLatin1Char(' '));
    label.replace(QLatin1Char('-'), QLatin1Char(' '));
    const QString lower = label.toLower();
    QString result;
    for (int i = 0; i < lower.size(); ++i) {
        if (i == 0 || lower[i - 1].isSpace())
            result.append(lower[i].toUpper());
        else
            result.append(lower[i]);
    }
    return result;
}

phicore::DeviceEffect mapHueEffectName(const QString &effect)
{
    const QString lower = effect.toLower();
    if (lower == QStringLiteral("candle"))
        return phicore::DeviceEffect::Candle;
    if (lower == QStringLiteral("fire") || lower == QStringLiteral("sunbeam"))
        return phicore::DeviceEffect::Fireplace;
    if (lower == QStringLiteral("sparkle") || lower == QStringLiteral("glisten")
        || lower == QStringLiteral("opal") || lower == QStringLiteral("prism")
        || lower == QStringLiteral("underwater") || lower == QStringLiteral("enchant")
        || lower == QStringLiteral("cosmos"))
        return phicore::DeviceEffect::Sparkle;
    if (lower == QStringLiteral("colorloop") || lower.contains(QStringLiteral("palette")))
        return phicore::DeviceEffect::ColorLoop;
    if (lower == QStringLiteral("sunrise") || lower == QStringLiteral("sunset"))
        return phicore::DeviceEffect::Relax;
    return phicore::DeviceEffect::CustomVendor;
}

QString hueEffectNameForDeviceEffect(phicore::DeviceEffect effect)
{
    switch (effect) {
    case phicore::DeviceEffect::Candle:
        return QStringLiteral("candle");
    case phicore::DeviceEffect::Fireplace:
        return QStringLiteral("fire");
    case phicore::DeviceEffect::Sparkle:
        return QStringLiteral("sparkle");
    case phicore::DeviceEffect::ColorLoop:
        return QStringLiteral("colorloop");
    case phicore::DeviceEffect::Relax:
        return QStringLiteral("sunset");
    case phicore::DeviceEffect::Concentrate:
        return QStringLiteral("enchant");
    case phicore::DeviceEffect::Alarm:
        return QStringLiteral("prism");
    default:
        break;
    }
    return QString();
}

void applyHueEffects(phicore::Device &device, const QJsonObject &source)
{
    if (source.isEmpty())
        return;

    QSet<QString> seen;
    for (const phicore::DeviceEffectDescriptor &existing : std::as_const(device.effects)) {
        seen.insert(existing.id.toLower());
        seen.insert(existing.label.toLower());
    }

    auto addEffectsFromArray = [&](const QJsonArray &array, const QString &category) {
        for (const QJsonValue &val : array) {
            if (!val.isString())
                continue;
            const QString value = val.toString().trimmed();
            if (value.isEmpty())
                continue;
            const QString key = value.toLower();
            if (key == QStringLiteral("no_effect"))
                continue;
            if (seen.contains(key))
                continue;
            seen.insert(key);

            phicore::DeviceEffectDescriptor desc;
            desc.id = value;
            desc.label = beautifyHueEffectLabel(value);
            desc.effect = mapHueEffectName(value);
            desc.description = QObject::tr("Hue effect %1").arg(desc.label);
            desc.meta.insert(QStringLiteral("hueEffect"), value);
            desc.meta.insert(QStringLiteral("hueEffectCategory"), category);
            device.effects.push_back(desc);
            qCInfo(adapterLog).noquote()
                << "HueAdapter::applyHueEffects - device" << device.id
                << "registered effect" << desc.label
                << "(" << desc.id << ")"
                << "category" << category
                << "mapped" << static_cast<int>(desc.effect);
        }
    };

    const QJsonArray effectsArr = source.value(QStringLiteral("effects")).toObject()
                                      .value(QStringLiteral("effect_values")).toArray();
    addEffectsFromArray(effectsArr, QStringLiteral("effects"));

    const QJsonArray v2ActionArr = source.value(QStringLiteral("effects_v2")).toObject()
                                       .value(QStringLiteral("action")).toObject()
                                       .value(QStringLiteral("effect_values")).toArray();
    addEffectsFromArray(v2ActionArr, QStringLiteral("effects"));

    const QJsonArray timedArr = source.value(QStringLiteral("timed_effects")).toObject()
                                    .value(QStringLiteral("effect_values")).toArray();
    addEffectsFromArray(timedArr, QStringLiteral("timed_effects"));
}

bool effectsEqual(const phicore::DeviceEffectDescriptorList &lhs,
                  const phicore::DeviceEffectDescriptorList &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (int i = 0; i < lhs.size(); ++i) {
        const auto &a = lhs.at(i);
        const auto &b = rhs.at(i);
        if (a.effect != b.effect
            || a.id != b.id
            || a.label != b.label
            || a.description != b.description
            || a.requiresParams != b.requiresParams
            || a.meta != b.meta) {
            return false;
        }
    }
    return true;
}


}

static qint64 parseHueTimestampMs(const QString &isoText)
{
    if (isoText.isEmpty())
        return 0;

    QDateTime dt = QDateTime::fromString(isoText, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(isoText, Qt::ISODate);
    if (!dt.isValid())
        return 0;

    if (dt.timeSpec() != Qt::UTC)
        dt = dt.toUTC();
    return dt.toMSecsSinceEpoch();
}

namespace phicore {

HueAdapter::HueAdapter(QObject *parent)
    : AdapterInterface(parent)
{
    // Pairing timeout timer (used when startPairing is implemented properly)
    m_pairingTimer.setSingleShot(true);
    connect(&m_pairingTimer, &QTimer::timeout,
            this, &HueAdapter::onPairingTimeout);

    // Poll timer is used as a safety net and for periodic full snapshots.
    // Default is aggressive (1s) for Hue v1 where we rely purely on polling.
    // When Hue v2 eventstream is active, we relax to 60s to avoid load.
    m_pollTimer.setInterval(1000);
    m_pollTimer.setSingleShot(false);
    m_pollTimer.setParent(this);
    connect(&m_pollTimer, &QTimer::timeout,
            this, &HueAdapter::onPollTimeout);

    // Coalesce frequent Hue v2 eventstream notifications into a single
    // snapshot refresh to avoid flooding the bridge with /lights calls.
    m_eventSyncTimer.setSingleShot(true);
    m_eventSyncTimer.setInterval(500);
    m_eventSyncTimer.setParent(this);
    connect(&m_eventSyncTimer, &QTimer::timeout,
            this, &HueAdapter::onEventSyncTimeout);

    m_eventStreamRetryTimer.setSingleShot(true);
    m_eventStreamRetryTimer.setInterval(m_eventStreamRetryIntervalMs);
    m_eventStreamRetryTimer.setParent(this);
    connect(&m_eventStreamRetryTimer, &QTimer::timeout,
            this, &HueAdapter::startEventStream);

    m_v2ResyncTimer.setSingleShot(true);
    m_v2ResyncTimer.setInterval(1000);
    m_v2ResyncTimer.setParent(this);
    connect(&m_v2ResyncTimer, &QTimer::timeout,
            this, &HueAdapter::performScheduledV2SnapshotRefresh);

    m_v2DeviceFetchTimer.setSingleShot(true);
    m_v2DeviceFetchTimer.setInterval(kV2DeviceFetchSpacingMs);
    m_v2DeviceFetchTimer.setParent(this);
    connect(&m_v2DeviceFetchTimer, &QTimer::timeout,
            this, &HueAdapter::startNextQueuedV2DeviceFetch);
}

HueAdapter::~HueAdapter()
{
    qCDebug(adapterLog) << "HueAdapter destroyed for" << adapter().id;
}

bool HueAdapter::start(QString &errorString)
{
    // This is called in the dedicated adapter thread.
    m_stopping = false;
    refreshConfig();

    if (adapter().host.isEmpty()) {
        errorString = QStringLiteral("HueAdapter: host is empty");
        setConnected(false);
        return false;
    }

    if (!m_nam) {
        // QNetworkAccessManager must be created in the thread where it is used.
        m_nam = new QNetworkAccessManager(this);
        connect(m_nam, &QNetworkAccessManager::finished,
                this, &HueAdapter::onNetworkReplyFinished);
    }

    // Delay connection state until a successful request or eventstream activity.

    // Start Hue API v2 eventstream immediately so we don't miss live updates
    // while waiting for the delayed snapshot to run.
    QMetaObject::invokeMethod(this, &HueAdapter::startEventStream,
                              Qt::QueuedConnection);

    errorString.clear();
    return true;
}

void HueAdapter::adapterConfigUpdated()
{
    refreshConfig();
}

void HueAdapter::updateStaticConfig(const QJsonObject &config)
{
    m_staticConfig = config;
    m_modelIdToProductNumber.clear();
    const QJsonObject mapping = config.value(QStringLiteral("modelIdToProductNumber")).toObject();
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        const QString key = it.key().trimmed();
        const QString value = it.value().toString().trimmed();
        if (!key.isEmpty() && !value.isEmpty())
            m_modelIdToProductNumber.insert(key, value);
    }
}

void HueAdapter::stop()
{
    m_stopping = true;
    m_pairingTimer.stop();
    m_pollTimer.stop();
    m_eventSyncTimer.stop();
    m_v2ResyncTimer.stop();
    m_pendingV2ResyncReason.clear();
    for (QTimer *timer : std::as_const(m_renameVerifyTimers)) {
        if (!timer)
            continue;
        timer->stop();
        timer->deleteLater();
    }
    m_renameVerifyTimers.clear();
    m_pendingRenameVerifications.clear();
    m_activeRenameFetches.clear();
    stopEventStream();

    if (m_nam) {
        // Cancel all pending requests
        const auto replies = m_nam->findChildren<QNetworkReply *>();
        for (QNetworkReply *r : replies) {
            r->abort();
        }
    }

    setConnected(false);
}

void HueAdapter::applyProductNumberMapping(Device &device, const QJsonObject &productObj)
{
    if (m_modelIdToProductNumber.isEmpty())
        return;
    const QString modelId = productObj.value(QStringLiteral("model_id")).toString().trimmed();
    if (modelId.isEmpty())
        return;
    const QString productNumber = m_modelIdToProductNumber.value(modelId);
    if (productNumber.isEmpty())
        return;
    const QStringList productCandidates = productNumber.split(QRegularExpression(QStringLiteral("[/\\s,]+")),
        Qt::SkipEmptyParts);
    const QString primaryProductNumber = productCandidates.isEmpty() ? productNumber : productCandidates.back();
    QJsonObject meta = device.meta;
    if (meta.value(QStringLiteral("productNumber")).toString().trimmed().isEmpty())
        meta.insert(QStringLiteral("productNumber"), productNumber);
    if (meta.value(QStringLiteral("iconUrl")).toString().trimmed().isEmpty()) {
        meta.insert(QStringLiteral("iconUrl"),
                    QStringLiteral("https://www.zigbee2mqtt.io/images/devices/%1.png").arg(primaryProductNumber));
    }
    device.meta = meta;
}

void HueAdapter::requestFullSync()
{
    if (m_stopping)
        return;
    if (!ensureHostAvailable()) {
        setConnected(false);
        if (!m_eventStreamRetryTimer.isActive())
            m_eventStreamRetryTimer.start(m_eventStreamRetryIntervalMs);
        return;
    }

    // For a pure Hue v2 adapter we bootstrap the current bridge state via
    // /clip/v2/resource/... snapshots and then keep it up to date via the
    // eventstream. No legacy v1 snapshots are used here.
    qCDebug(adapterLog) << "HueAdapter::requestFullSync() - fetching v2 resource snapshots";
    fetchV2ResourcesSnapshot();

    if (!m_pollTimer.isActive()) {
        m_pollTimer.start();
    }
}

void HueAdapter::updateChannelState(const QString &deviceExternalId, const QString &channelExternalId,
    const QVariant &value,  CmdId cmdId)
{
    if (m_stopping) {
        AdapterInterface::updateChannelState(deviceExternalId, channelExternalId, value, cmdId);
        return;
    }

    const QString bindingKey = channelBindingKey(deviceExternalId, channelExternalId);
    const HueChannelBinding binding = m_channelBindings.value(bindingKey);
    if (binding.resourceType != QStringLiteral("light") || binding.resourceId.isEmpty()) {
        AdapterInterface::updateChannelState(deviceExternalId, channelExternalId, value, cmdId);
        return;
    }

    CmdResponse resp;
    resp.id   = cmdId;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();

    QJsonObject body;
    bool handled = false;

    if (channelExternalId == QStringLiteral("on")) {
        const bool on = value.toBool();
        QJsonObject onObj;
        onObj.insert(QStringLiteral("on"), on);
        body.insert(QStringLiteral("on"), onObj);
        resp.finalValue = on;
        handled = true;
    } else if (channelExternalId == QStringLiteral("bri")) {
        double percent = value.toDouble();
        percent = qBound(0.0, percent, 100.0);
        QJsonObject dimObj;
        dimObj.insert(QStringLiteral("brightness"), percent);
        body.insert(QStringLiteral("dimming"), dimObj);
        resp.finalValue = percent;
        handled = true;
    } else if (channelExternalId == QStringLiteral("ct")) {
        const int ctMired = value.toInt();
        QJsonObject ctObj;
        ctObj.insert(QStringLiteral("mirek"), ctMired);
        body.insert(QStringLiteral("color_temperature"), ctObj);
        resp.finalValue = ctMired;
        handled = true;
    } else if (channelExternalId == QStringLiteral("ctPreset")) {
        int presetIdx = value.toInt();
        presetIdx = qBound(0, presetIdx, 4);
        double ctMin = 153.0;
        double ctMax = 500.0;
        const ChannelList channels = m_v2DeviceChannels.value(deviceExternalId);
        for (const Channel &ch : channels) {
            if (ch.id == QStringLiteral("ct")) {
                if (ch.minValue > 0.0)
                    ctMin = ch.minValue;
                if (ch.maxValue > 0.0)
                    ctMax = ch.maxValue;
                break;
            }
        }
        const double span = ctMax - ctMin;
        const double t = span > 0.0 ? (static_cast<double>(presetIdx) / 4.0) : 0.5;
        const int ctMired = static_cast<int>(qRound(ctMin + t * span));
        QJsonObject ctObj;
        ctObj.insert(QStringLiteral("mirek"), ctMired);
        body.insert(QStringLiteral("color_temperature"), ctObj);
        resp.finalValue = presetIdx;
        handled = true;
    } else if (channelExternalId == QStringLiteral("color")) {
        Color c;
        if (value.canConvert<Color>()) {
            c = value.value<Color>();
        } else {
            const QVariantMap map = value.toMap();
            const double r = map.value(QStringLiteral("r")).toDouble();
            const double g = map.value(QStringLiteral("g")).toDouble();
            const double b = map.value(QStringLiteral("b")).toDouble();
            c = makeColor(r, g, b);
        }

        double x = 0.0;
        double y = 0.0;
        colorToXy(c, x, y);
        clampColorToGamut(binding.resourceId, x, y);

        QJsonObject xyObj;
        xyObj.insert(QStringLiteral("x"), x);
        xyObj.insert(QStringLiteral("y"), y);
        QJsonObject colorObj;
        colorObj.insert(QStringLiteral("xy"), xyObj);
        body.insert(QStringLiteral("color"), colorObj);

        resp.finalValue = QVariant::fromValue(c);
        handled = true;
    }

    if (!handled || body.isEmpty()) {
        AdapterInterface::updateChannelState(deviceExternalId, channelExternalId, value, cmdId);
        return;
    }

    const bool sent = sendV2ResourceUpdate(binding.resourceType, binding.resourceId, body);
    resp.status = sent ? CmdStatus::Success : CmdStatus::Failure;
    if (!sent) {
        resp.error = QStringLiteral("Hue request could not be sent");
    }
    emit cmdResult(resp);
}

void HueAdapter::onPairingTimeout()
{
    if (m_stopping)
        return;

    qCWarning(adapterLog) << "HueAdapter::startPairing: pairing timeout reached "
                             "(not yet implemented)";
    emit errorOccurred(QStringLiteral("Hue pairing not yet implemented"));
}

void HueAdapter::onPollTimeout()
{
    if (m_stopping)
        return;
    // For a pure v2-driven adapter we do not perform any periodic v1
    // polling. Initial snapshots (if any) are triggered explicitly from
    // requestFullSync(); further updates are delivered via the v2
    // eventstream.
    Q_UNUSED(m_supportsV2Events);
}

void HueAdapter::onNetworkReplyFinished(QNetworkReply *reply)
{
    if (!reply)
        return;

    const QUrl url = reply->request().url();
    const QByteArray data = reply->readAll();
    const QString path = url.path();

    if (reply->error() != QNetworkReply::NoError) {
        if (!m_stopping)
            setConnected(false);
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        if (statusCode > 0) {
            qCWarning(adapterLog) << "Hue request failed:" << url.toString()
                                  << "status:" << statusCode << reason
                                  << "error:" << reply->errorString();
        } else {
            qCWarning(adapterLog) << "Hue request failed:" << url.toString()
                                  << "error:" << reply->errorString();
        }
        if (!data.isEmpty()) {
            QByteArray snippet = data.left(256);
            QString payloadSnippet = QString::fromUtf8(snippet);
            if (data.size() > snippet.size()) {
                payloadSnippet.append(QStringLiteral(" ..."));
            }
            qCWarning(adapterLog).noquote()
                << "Hue response payload:" << payloadSnippet;
        }

        // Ensure v2 bootstrap can complete even if some resource snapshots fail.
        if (path.startsWith(QStringLiteral("/clip/v2/resource/"))) {
            const QString resourcePath = path.mid(QStringLiteral("/clip/v2/resource/").size());
            // Bootstrap snapshots have the form /clip/v2/resource/{type}
            // without a trailing id component. Lazy per-device fetches use
            // /clip/v2/resource/device/<id> and must not touch the snapshot
            // bookkeeping.
            if (!resourcePath.contains(QLatin1Char('/'))) {
                const QString resourceType = resourcePath;
                if (!resourceType.isEmpty()) {
                    m_pendingV2ResourceTypes.remove(resourceType);
                    const int attempt = m_v2ResourceRetryCount.value(resourceType, 0);
                    if (attempt < kMaxV2ResourceSnapshotRetries) {
                        const int nextAttempt = attempt + 1;
                        const int delayMs = kV2ResourceRetryBaseDelayMs * nextAttempt;
                        m_v2ResourceRetryCount.insert(resourceType, nextAttempt);
                        qCWarning(adapterLog)
                            << "HueAdapter::fetchV2ResourcesSnapshot - retrying resource"
                            << resourceType << "attempt" << nextAttempt << "in" << delayMs << "ms";
                        requestV2ResourceSnapshot(resourceType, delayMs);
                        reply->deleteLater();
                        return;
                    }

                    qCWarning(adapterLog)
                        << "HueAdapter::fetchV2ResourcesSnapshot - giving up on resource"
                        << resourceType << "after" << attempt << "retries";
                    m_v2ResourceRetryCount.remove(resourceType);
                    m_v2SnapshotFailedThisCycle = true;
                    m_v2SnapshotByType.insert(resourceType, QJsonArray());
                    if (m_v2SnapshotPending > 0) {
                        --m_v2SnapshotPending;
                    }
                    finalizeV2SnapshotIfReady();
                }
            } else if (resourcePath.startsWith(QStringLiteral("device/"))) {
                // Lazy single-device fetch failed; stop tracking it so that
                // future events can trigger another attempt if needed.
                const QString deviceId = resourcePath.section(QLatin1Char('/'), 1);
                if (!deviceId.isEmpty()) {
                    m_pendingV2DeviceFetch.remove(deviceId);
                    m_failedV2DeviceFetch.insert(deviceId);
                }
            }
        }

        if (path.startsWith(QStringLiteral("/clip/v2/resource/device/"))) {
            startNextQueuedV2DeviceFetch();
        }

        reply->deleteLater();
        return;
    }

    setConnected(true);

    // Basic JSON parsing
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        reply->deleteLater();
        return;
    }

    const QJsonObject obj = doc.object();

    // Hue API v2 resource snapshots and lazy device fetches.
    if (path.startsWith(QStringLiteral("/clip/v2/resource/"))) {
        const QString resourcePath = path.mid(QStringLiteral("/clip/v2/resource/").size());
        if (resourcePath.startsWith(QStringLiteral("device/"))) {
            // Lazy single device fetch: merge into the existing v2 device map.
            const QString deviceId = resourcePath.section(QLatin1Char('/'), 1);
            if (!deviceId.isEmpty())
                m_pendingV2DeviceFetch.remove(deviceId);

            const QJsonArray dataArr = obj.value(QStringLiteral("data")).toArray();
            for (const QJsonValue &val : dataArr) {
                if (!val.isObject())
                    continue;
                const QJsonObject devObj = val.toObject();
                const QString devIdFromPayload = devObj.value(QStringLiteral("id")).toString();
                const QString effectiveId = !devIdFromPayload.isEmpty() ? devIdFromPayload : deviceId;
                if (effectiveId.isEmpty())
                    continue;

                QString externalId = m_deviceIdToExternalId.value(effectiveId);
                if (externalId.isEmpty()) {
                    externalId = effectiveId;
                    m_deviceIdToExternalId.insert(effectiveId, externalId);
                }

                Device device = m_v2DeviceInfoCache.value(externalId);
                device.id = externalId;

                const QJsonObject metaObj = devObj.value(QStringLiteral("metadata")).toObject();
                const QJsonObject productObj = devObj.value(QStringLiteral("product_data")).toObject();
                const QString fetchedName = metaObj.value(QStringLiteral("name")).toString();
                qCInfo(adapterLog).noquote()
                    << "HueAdapter::fetchV2DeviceResource - received metadata for" << effectiveId
                    << "name=" << fetchedName;

                bool deviceChanged = false;
                const auto assignIfChanged = [&deviceChanged](QString &target, const QString &value) {
                    if (value.isEmpty() || target == value)
                        return;
                    target = value;
                    deviceChanged = true;
                };

                const QString refreshedName = metaObj.value(QStringLiteral("name")).toString().trimmed();
                const bool renameAttempt = m_activeRenameFetches.contains(effectiveId);
                if (!refreshedName.isEmpty()) {
                    assignIfChanged(device.name, refreshedName);
                    if (renameAttempt)
                        completeRenameVerification(device.id);
                } else if (renameAttempt) {
                    const int attempt = m_pendingRenameVerifications.value(device.id, -1);
                    if (attempt >= 0)
                        scheduleRenameVerification(device.id, attempt + 1);
                }
                if (renameAttempt)
                    m_activeRenameFetches.remove(effectiveId);
                assignIfChanged(device.manufacturer, productObj.value(QStringLiteral("manufacturer_name")).toString());
                assignIfChanged(device.model, productObj.value(QStringLiteral("model_id")).toString());
                assignIfChanged(device.firmware, productObj.value(QStringLiteral("software_version")).toString());
                if (!devObj.isEmpty()) {
                    if (device.meta != devObj) {
                        device.meta = devObj;
                        deviceChanged = true;
                    }
                    attachServiceRefs(device.meta);
                    applyProductNumberMapping(device, productObj);
                    applyDeviceClassFromMetadata(device, metaObj, productObj);
                    applyHueEffects(device, devObj);
                }

                m_v2DeviceInfoCache.insert(externalId, device);
                m_v2Devices.insert(externalId, device);

                if (deviceChanged && m_v2BootstrapDone) {
                    const ChannelList channels = m_v2DeviceChannels.value(device.id);
                    if (!channels.isEmpty()) {
                        qCInfo(adapterLog).noquote()
                            << "HueAdapter::fetchV2DeviceResource - refreshing device metadata for"
                            << device.id << device.name;
                        emit deviceUpdated(device, channels);
                    }
                }
            }
        } else {
            const QString resourceType = resourcePath;
            handleV2ResourceSnapshot(resourceType, obj);
        }

        if (resourcePath.startsWith(QStringLiteral("device/"))) {
            startNextQueuedV2DeviceFetch();
        }

        finalizeV2SnapshotIfReady();

        reply->deleteLater();
        return;
    }

    reply->deleteLater();
}

QUrl HueAdapter::baseUrl() const
{
    const Adapter &info = adapter();
    const bool useTls = info.flags.testFlag(AdapterFlag::AdapterFlagUseTls);

    const QString scheme = useTls ? QStringLiteral("https") : QStringLiteral("http");
    QUrl url;
    url.setScheme(scheme);
    const QString host = info.ip.trimmed();
    url.setHost(host);

    const quint16 port = info.port;
    if (port != 0) {
        url.setPort(port);
    } else {
        // Default Hue ports (80 / 443)
        if (useTls)
            url.setPort(443);
        else
            url.setPort(80);
    }

    return url;
}

QUrl HueAdapter::v2ResourceUrl(const QString &resourcePath) const
{
    // Build a Hue API v2 resource URL like:
    //   /clip/v2/resource/device
    //   /clip/v2/resource/light
    QUrl url = baseUrl();
    QString path = QStringLiteral("/clip/v2/");
    if (!resourcePath.startsWith(QStringLiteral("resource/")))
        path += QStringLiteral("resource/");
    path += resourcePath;
    url.setPath(path);
    return url;
}

void HueAdapter::setConnected(bool connected)
{
    if (m_connected == connected)
        return;

    m_connected = connected;
    emit connectionStateChanged(m_connected);
}

void HueAdapter::fetchV2ResourcesSnapshot()
{
    if (!m_nam)
        return;

    const Adapter &info = adapter();
    if (info.token.isEmpty()) {
        qCWarning(adapterLog) << "HueAdapter::fetchV2ResourcesSnapshot: appKey is empty, cannot fetch v2 resources";
        return;
    }

    // Fetch a core set of Hue v2 resources and build a device/channel model
    // from their snapshots. We keep these types intentionally small for now
    // and can extend the list as needed.
    m_v2SnapshotByType.clear();
    m_pendingV2DeviceFetch.clear();
    m_v2DeviceFetchQueue.clear();
    m_failedV2DeviceFetch.clear();
    m_pendingV2ResourceTypes.clear();
    m_v2ResourceRetryCount.clear();
    m_v2SnapshotFailedThisCycle = false;

    const QStringList resources = {
        QStringLiteral("device"),
        QStringLiteral("room"),
        QStringLiteral("zone"),
        QStringLiteral("light"),
        QStringLiteral("motion"),
        QStringLiteral("temperature"),
        QStringLiteral("light_level"),
        QStringLiteral("device_power"),
        QStringLiteral("button"),
        QStringLiteral("device_software_update"),
        QStringLiteral("zigbee_connectivity"),
        QStringLiteral("zigbee_device_discovery"),
        QStringLiteral("scene")
    };

    m_v2SnapshotPending = resources.size();
    m_v2BootstrapDone   = false;

    int staggerDelay = 0;
    constexpr int kSnapshotStaggerMs = 400;
    for (const QString &res : resources) {
        int delay = staggerDelay;
        if (res == QStringLiteral("button"))
            delay += 1000;
        requestV2ResourceSnapshot(res, delay);
        staggerDelay += kSnapshotStaggerMs;
    }
}

void HueAdapter::fetchV2DeviceResource(const QString &deviceId)
{
    if (!m_nam || deviceId.isEmpty())
        return;
    if (m_failedV2DeviceFetch.contains(deviceId))
        return;
    if (m_pendingV2DeviceFetch.contains(deviceId))
        return;
    if (m_v2DeviceFetchQueue.contains(deviceId))
        return;

    if (m_pendingV2DeviceFetch.size() >= kMaxConcurrentV2DeviceFetch) {
        m_v2DeviceFetchQueue.enqueue(deviceId);
        qCDebug(adapterLog) << "HueAdapter::fetchV2DeviceResource() - queueing metadata fetch for Hue device"
                            << deviceId;
        return;
    }

    startV2DeviceFetch(deviceId);
}

void HueAdapter::requestV2ResourceSnapshot(const QString &resourceType, int delayMs)
{
    if (resourceType.isEmpty())
        return;

    auto sendRequest = [this, resourceType]() {
        if (!m_nam)
            return;
        if (m_pendingV2ResourceTypes.contains(resourceType))
            return;

        const Adapter &info = adapter();
        if (info.token.isEmpty()) {
            qCWarning(adapterLog)
                << "HueAdapter::requestV2ResourceSnapshot - missing appKey for resource"
                << resourceType;
            return;
        }

        const QUrl url = v2ResourceUrl(QStringLiteral("resource/%1").arg(resourceType));
        QNetworkRequest req(url);
#if QT_CONFIG(ssl)
        if (info.flags.testFlag(AdapterFlag::AdapterFlagUseTls)) {
            QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
            ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
            req.setSslConfiguration(ssl);
        }
#endif
        req.setRawHeader("hue-application-key", info.token.toUtf8());
        req.setRawHeader("Accept", "application/json");

        m_pendingV2ResourceTypes.insert(resourceType);
        m_nam->get(req);
    };

    if (delayMs > 0) {
        QTimer::singleShot(delayMs, this, sendRequest);
    } else {
        sendRequest();
    }
}

bool HueAdapter::startV2DeviceFetch(const QString &deviceId)
{
    if (!m_nam || deviceId.isEmpty())
        return false;
    if (m_failedV2DeviceFetch.contains(deviceId))
        return false;

    const Adapter &info = adapter();
    if (info.token.isEmpty()) {
        qCWarning(adapterLog) << "HueAdapter::fetchV2DeviceResource: appKey is empty, cannot fetch device"
                              << deviceId;
        return false;
    }

    const QUrl url = v2ResourceUrl(QStringLiteral("device/%1").arg(deviceId));
    QNetworkRequest req(url);
#if QT_CONFIG(ssl)
    if (info.flags.testFlag(AdapterFlag::AdapterFlagUseTls)) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }
#endif
    req.setRawHeader("hue-application-key", info.token.toUtf8());
    req.setRawHeader("Accept", "application/json");

    qCDebug(adapterLog) << "HueAdapter::fetchV2DeviceResource() - fetching metadata for Hue device" << deviceId;
    m_pendingV2DeviceFetch.insert(deviceId);
    m_nam->get(req);
    return true;
}

void HueAdapter::startNextQueuedV2DeviceFetch()
{
    if (!m_nam)
        return;

    bool started = false;
    while (!m_v2DeviceFetchQueue.isEmpty()
           && m_pendingV2DeviceFetch.size() < kMaxConcurrentV2DeviceFetch) {
        const QString deviceId = m_v2DeviceFetchQueue.dequeue();
        if (deviceId.isEmpty())
            continue;
        if (m_failedV2DeviceFetch.contains(deviceId))
            continue;
        if (m_pendingV2DeviceFetch.contains(deviceId))
            continue;
        if (!startV2DeviceFetch(deviceId)) {
            // Abort draining when we cannot start the request to avoid a busy loop.
            break;
        }
        started = true;
        break; // stage one fetch per invocation
    }

    if (!started)
        return;

    if (!m_v2DeviceFetchQueue.isEmpty()
        && m_pendingV2DeviceFetch.size() < kMaxConcurrentV2DeviceFetch
        && !m_v2DeviceFetchTimer.isActive()) {
        m_v2DeviceFetchTimer.start();
    }
}

bool HueAdapter::sendV2ResourceUpdate(const QString &resourceType, const QString &resourceId, const QJsonObject &payload)
{
    if (!m_nam)
        return false;
    if (resourceType.isEmpty() || resourceId.isEmpty())
        return false;
    if (payload.isEmpty())
        return false;

    const Adapter &info = adapter();
    if (info.token.isEmpty())
        return false;

    const QUrl url = v2ResourceUrl(QStringLiteral("%1/%2").arg(resourceType, resourceId));
    QNetworkRequest req(url);
#if QT_CONFIG(ssl)
    if (info.flags.testFlag(AdapterFlag::AdapterFlagUseTls)) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }
#endif
    req.setRawHeader("hue-application-key", info.token.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    const QByteArray data = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_nam->sendCustomRequest(req, QByteArrayLiteral("PUT"), data);
    return reply != nullptr;
}
QUrl HueAdapter::eventStreamUrl() const
{
    // Hue API v2 eventstream endpoint:
    //   GET /eventstream/clip/v2
    // with header: hue-application-key: <appKey>
    // This does not use the classic /api/<appKey> path.
    QUrl url = baseUrl();
    url.setPath(QStringLiteral("/eventstream/clip/v2"));
    return url;
}

void HueAdapter::startEventStream()
{
    if (m_stopping)
        return;
    if (m_eventStreamReply)
        return;
    if (!m_nam)
        return;
    if (!ensureHostAvailable()) {
        setConnected(false);
        if (!m_eventStreamRetryTimer.isActive())
            m_eventStreamRetryTimer.start(m_eventStreamRetryIntervalMs);
        return;
    }

    const Adapter &info = adapter();
    if (info.token.isEmpty()) {
        qCWarning(adapterLog) << "HueAdapter::startEventStream: appKey is empty, cannot start v2 eventstream";
        return;
    }

    const QUrl url = eventStreamUrl();
    QNetworkRequest req(url);
#if QT_CONFIG(ssl)
    if (info.flags.testFlag(AdapterFlag::AdapterFlagUseTls)) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }
#endif
    // Hue API v2 uses an application key header instead of /api/<key> in the path.
    req.setRawHeader("hue-application-key", info.token.toUtf8());
    // For server-sent events, advertise that we accept an event-stream.
    req.setRawHeader("Accept", "text/event-stream");

    qCDebug(adapterLog) << "HueAdapter::startEventStream - connecting to" << url.toString();
    m_eventStreamReply = m_nam->get(req);
    if (!m_eventStreamReply) {
        qCWarning(adapterLog) << "HueAdapter::startEventStream: failed to create eventstream request";
        return;
    }

    connect(m_eventStreamReply, &QNetworkReply::readyRead,
            this, &HueAdapter::onEventStreamReadyRead);
    connect(m_eventStreamReply, &QNetworkReply::finished,
            this, &HueAdapter::onEventStreamFinished);
}

void HueAdapter::stopEventStream()
{
    if (!m_eventStreamReply)
        return;

    qCDebug(adapterLog) << "HueAdapter::stopEventStream";
    disconnect(m_eventStreamReply, nullptr, this, nullptr);
    m_eventStreamReply->abort();
    m_eventStreamReply->deleteLater();
    m_eventStreamReply = nullptr;
}

void HueAdapter::onEventStreamReadyRead()
{
    if (!m_eventStreamReply)
        return;

    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_eventStreamReply)
        return;

    // Drain incoming data; each SSE "data:" line carries a JSON payload.
    const QByteArray chunk = reply->readAll();
    if (!chunk.isEmpty()) {
        setConnected(true);
        if (m_eventStreamRetryTimer.isActive())
            m_eventStreamRetryTimer.stop();
        const QList<QByteArray> lines = chunk.split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.startsWith("data:")) {
                QByteArray json = line.mid(5).trimmed();
                if (!json.isEmpty()) {
                    handleEventStreamData(json);
                }
            }
        }

        if (!m_supportsV2Events) {
            m_supportsV2Events = true;
            m_eventStreamRetryCount = 0;
            // Relax poll interval when v2 eventstream is confirmed. We keep
            // polling v1 lights for full snapshots, but stop polling v1
            // sensors (see onPollTimeout) and rely on v2 events for sensor
            // and button/rotary updates.
            if (m_pollTimer.interval() != 60000) {
                m_pollTimer.setInterval(60000);
            }
            qCInfo(adapterLog) << "HueAdapter: v2 eventstream is active; using event-driven updates "
                                  "(polling remains enabled for light snapshots)";
        }
    }
}

void HueAdapter::onEventStreamFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_eventStreamReply)
        return;

    const bool hasError = reply->error() != QNetworkReply::NoError;
    if (hasError && !m_stopping)
        setConnected(false);
    if (hasError) {
        if (m_eventStreamErrorSuppressCount > 0) {
            qCInfo(adapterLog) << "HueAdapter eventstream error (suppressed):"
                               << reply->request().url().toString()
                               << "error:" << reply->errorString();
            --m_eventStreamErrorSuppressCount;
        } else {
            qCWarning(adapterLog) << "HueAdapter eventstream error:" << reply->request().url().toString()
                                  << "error:" << reply->errorString();
        }
    } else {
        qCInfo(adapterLog) << "HueAdapter eventstream finished for"
                           << reply->request().url().toString();
    }

    // In all cases, treat a finished eventstream as unavailable and ensure
    // classic polling is active as a fallback. For transient errors, try to
    // re-establish the eventstream a few times before giving up.
    m_supportsV2Events = false;
    // When v2 is not available, use a short polling interval for responsive
    // updates (Hue v1).
    if (m_pollTimer.interval() != 1000) {
        m_pollTimer.setInterval(1000);
    }
    if (!m_pollTimer.isActive() && !m_stopping) {
        m_pollTimer.start();
    }

    if (!m_stopping && hasError) {
        if (m_eventStreamErrorSuppressCount > 0) {
            qCInfo(adapterLog) << "Retrying Hue v2 eventstream connection (suppressed path)";
            QTimer::singleShot(2000, this, &HueAdapter::startEventStream);
        } else if (m_eventStreamRetryCount < 5) {
            ++m_eventStreamRetryCount;
            qCInfo(adapterLog) << "Retrying Hue v2 eventstream connection (attempt"
                               << m_eventStreamRetryCount << "of 5)";
            QTimer::singleShot(2000, this, &HueAdapter::startEventStream);
        } else {
            if (!m_eventStreamRetryTimer.isActive()) {
                qCWarning(adapterLog) << "Hue eventstream failed after"
                                      << m_eventStreamRetryCount
                                      << "attempts; retrying in"
                                      << m_eventStreamRetryIntervalMs
                                      << "ms";
                m_eventStreamRetryTimer.start(m_eventStreamRetryIntervalMs);
            }
        }
    }

    reply->deleteLater();
    m_eventStreamReply = nullptr;
}

void HueAdapter::refreshConfig()
{
    const int retry = adapter().meta.value(QStringLiteral("retryIntervalMs")).toInt(10000);
    if (retry >= 1000) {
        m_eventStreamRetryIntervalMs = retry;
        if (m_eventStreamRetryTimer.interval() != m_eventStreamRetryIntervalMs)
            m_eventStreamRetryTimer.setInterval(m_eventStreamRetryIntervalMs);
    }
}

bool HueAdapter::ensureHostAvailable() const
{
    return !adapter().ip.trimmed().isEmpty();
}

void HueAdapter::onEventSyncTimeout()
{
    if (m_stopping)
        return;
    // Extra polling after v2 events is currently disabled; we rely on the
    // event payload itself and the regular poll timer as a fallback.
}

void HueAdapter::handleEventStreamData(const QByteArray &jsonData)
{
    QString payload = QString::fromUtf8(jsonData);
    if (payload.size() > 2048) {
        payload.truncate(2048);
        payload.append(QStringLiteral(" ..."));
    }
    qCInfo(adapterLog).noquote()
        << "HueAdapter v2 event stream payload" << payload;

    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonData, &err);
    if (err.error != QJsonParseError::NoError)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            handleEventStreamEventObject(val.toObject(), nowMs);
        }
    } else if (doc.isObject()) {
        handleEventStreamEventObject(doc.object(), nowMs);
    }
}

void HueAdapter::handleEventStreamEventObject(const QJsonObject &eventObj, qint64 nowMs)
{
    const QString eventType = eventObj.value(QStringLiteral("type")).toString();
    if (eventType == QStringLiteral("delete")) {
        const QJsonArray dataArr = eventObj.value(QStringLiteral("data")).toArray();
        handleV2DeleteEvent(dataArr);
        scheduleV2SnapshotRefresh(QStringLiteral("v2 delete event"));
        return;
    }

    const QJsonArray dataArr = eventObj.value(QStringLiteral("data")).toArray();
    for (const QJsonValue &resVal : dataArr) {
        if (!resVal.isObject())
            continue;

        const QJsonObject resObj = resVal.toObject();
        const QString type = resObj.value(QStringLiteral("type")).toString();
        const QString resId = resObj.value(QStringLiteral("id")).toString();
        const QString resPayload = QString::fromUtf8(QJsonDocument(resObj).toJson(QJsonDocument::Compact));
        qCInfo(adapterLog).noquote()
            << "HueAdapter v2 event resource type" << type
            << "id" << resId
            << "payload" << resPayload;

        bool topologyChange = false;
        if (type == QStringLiteral("device")) {
            topologyChange = true;
        } else if (type == QStringLiteral("room") || type == QStringLiteral("zone")) {
            if (resObj.contains(QStringLiteral("children")) || resObj.contains(QStringLiteral("services")))
                topologyChange = true;
        }

        if (type == QStringLiteral("light")) {
            handleV2LightResource(resObj, nowMs);
        } else if (type == QStringLiteral("motion")) {
            handleV2MotionResource(resObj, nowMs);
        } else if (type == QStringLiteral("tamper")) {
            handleV2TamperResource(resObj, nowMs);
        } else if (type == QStringLiteral("temperature")) {
            handleV2TemperatureResource(resObj, nowMs);
        } else if (type == QStringLiteral("light_level")) {
            handleV2LightLevelResource(resObj, nowMs);
        } else if (type == QStringLiteral("device_power")) {
            handleV2DevicePowerResource(resObj, nowMs);
        } else if (type == QStringLiteral("device_software_update")) {
            handleV2DeviceSoftwareUpdateResource(resObj, nowMs);
        } else if (type == QStringLiteral("relative_rotary")) {
            handleV2RelativeRotaryResource(resObj, nowMs);
        } else if (type == QStringLiteral("button")) {
            handleV2ButtonResource(resObj, nowMs);
        } else if (type == QStringLiteral("zigbee_connectivity")) {
            handleV2ZigbeeConnectivityResource(resObj, nowMs);
        } else if (type == QStringLiteral("zigbee_device_discovery")) {
            handleV2ZigbeeDeviceDiscoveryResource(resObj, nowMs);
        } else if (type == QStringLiteral("room")) {
            handleV2RoomResource(resObj);
        } else if (type == QStringLiteral("zone")) {
            handleV2ZoneResource(resObj);
        } else if (type == QStringLiteral("scene")) {
            handleV2SceneResource(resObj);
            emit scenesUpdated(m_v2Scenes.values());
        }

        if (topologyChange)
            scheduleV2SnapshotRefresh(QStringLiteral("v2 event type %1").arg(type));
    }
}

void HueAdapter::handleV2LightResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QString lightServiceId = resObj.value(QStringLiteral("id")).toString();
    if (!lightServiceId.isEmpty())
        m_deviceToLightResource.insert(deviceExtId, lightServiceId);

    // Power state
    if (resObj.contains(QStringLiteral("on"))) {
        const QJsonObject onObj = resObj.value(QStringLiteral("on")).toObject();
        if (onObj.contains(QStringLiteral("on"))) {
            const bool on = onObj.value(QStringLiteral("on")).toBool();
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("on"),
                                     on,
                                     nowMs);
        }
    }

    // Brightness (canonical 0–100%)
    double brightnessPercent = -1.0;
    if (resObj.contains(QStringLiteral("dimming"))) {
        const QJsonObject dimObj = resObj.value(QStringLiteral("dimming")).toObject();
        if (dimObj.contains(QStringLiteral("brightness"))) {
            brightnessPercent = dimObj.value(QStringLiteral("brightness")).toDouble(-1.0);
            if (brightnessPercent >= 0.0) {
                const double percentValue = qBound(0.0, brightnessPercent, 100.0);
                emit channelStateUpdated(deviceExtId,
                                         QStringLiteral("bri"),
                                         percentValue,
                                         nowMs);
            }
        }
    }

    // Color temperature (mired)
    if (resObj.contains(QStringLiteral("color_temperature"))) {
        const QJsonObject ctObj = resObj.value(QStringLiteral("color_temperature")).toObject();
        if (ctObj.contains(QStringLiteral("mirek"))) {
            const int ctMired = ctObj.value(QStringLiteral("mirek")).toInt(0);
            if (ctMired > 0) {
                emit channelStateUpdated(deviceExtId,
                                         QStringLiteral("ct"),
                                         ctMired,
                                         nowMs);
            }
        }
    }

    // Color (xy)
    if (resObj.contains(QStringLiteral("color"))) {
        const QJsonObject colorObj = resObj.value(QStringLiteral("color")).toObject();
        const QJsonObject xyObj = colorObj.value(QStringLiteral("xy")).toObject();
        if (xyObj.contains(QStringLiteral("x")) && xyObj.contains(QStringLiteral("y"))) {
            const double x = xyObj.value(QStringLiteral("x")).toDouble(0.0);
            const double y = xyObj.value(QStringLiteral("y")).toDouble(0.0);

            // Color channel in core is independent from brightness; always
            // derive chromaticity with full brightness and use the dedicated
            // brightness channel for intensity.
            const Color color = colorFromXy(x, y, 1.0);
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("color"),
                                     QVariant::fromValue(color),
                                     nowMs);
        }
    }

    updateDeviceEffectsFromLight(deviceExtId, resObj);
}

QString HueAdapter::deviceExternalIdFromV2Resource(const QJsonObject &resObj) const
{
    const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
    if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
        return {};

    const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
    if (deviceId.isEmpty())
        return {};

    QString mapped = m_deviceIdToExternalId.value(deviceId);
    if (mapped.isEmpty()) {
        mapped = deviceId;
        auto *self = const_cast<HueAdapter *>(this);
        self->m_deviceIdToExternalId.insert(deviceId, mapped);
        self->fetchV2DeviceResource(deviceId);
    } else {
        const Device deviceInfo = m_v2DeviceInfoCache.value(mapped);
        if (deviceInfo.meta.isEmpty()) {
            auto *self = const_cast<HueAdapter *>(this);
            self->fetchV2DeviceResource(deviceId);
        }
    }
    return mapped;
}

QString HueAdapter::deviceExtIdForResource(const QString &resourceType, const QString &resourceId) const
{
    if (resourceType.isEmpty() || resourceId.isEmpty())
        return {};

    if (resourceType == QStringLiteral("device"))
        return m_deviceIdToExternalId.value(resourceId, resourceId);

    if (resourceType == QStringLiteral("room"))
        return {};

    const QString key = resourceBindingKey(resourceType, resourceId);
    const QString mapped = m_v2ResourceToDevice.value(key);
    if (!mapped.isEmpty())
        return mapped;

    const QJsonArray arr = m_v2SnapshotByType.value(resourceType);
    for (const QJsonValue &val : arr) {
        if (!val.isObject())
            continue;
        const QJsonObject obj = val.toObject();
        if (obj.value(QStringLiteral("id")).toString() != resourceId)
            continue;
        const QString ownerExtId = deviceExternalIdFromV2Resource(obj);
        if (!ownerExtId.isEmpty())
            return ownerExtId;
        break;
    }

    return {};
}

void HueAdapter::handleV2ResourceSnapshot(const QString &resourceType, const QJsonObject &root)
{
    const QJsonArray dataArr = root.value(QStringLiteral("data")).toArray();
    qCInfo(adapterLog).noquote()
        << "HueAdapter::handleV2ResourceSnapshot type" << resourceType << "count" << dataArr.size();
    m_v2SnapshotByType.insert(resourceType, dataArr);
    m_pendingV2ResourceTypes.remove(resourceType);
    m_v2ResourceRetryCount.remove(resourceType);

    if (resourceType == QStringLiteral("scene")) {
        m_v2Scenes.clear();
        for (const QJsonValue &val : dataArr) {
            if (!val.isObject())
                continue;
            const QJsonObject obj = val.toObject();
            handleV2SceneResource(obj);
        }
        m_sceneSnapshotDirty = true;
    }

    if (m_v2SnapshotPending > 0) {
        --m_v2SnapshotPending;
    }

    finalizeV2SnapshotIfReady();
}

void HueAdapter::updateDeviceEffectsFromLight(const QString &deviceExtId, const QJsonObject &lightObj)
{
    if (deviceExtId.isEmpty() || lightObj.isEmpty())
        return;

    Device cached = m_v2DeviceInfoCache.value(deviceExtId);
    if (cached.id.isEmpty())
        return;

    Device updated = cached;
    applyHueEffects(updated, lightObj);
    if (effectsEqual(cached.effects, updated.effects))
        return;

    m_v2DeviceInfoCache.insert(deviceExtId, updated);
    m_v2Devices.insert(deviceExtId, updated);

    if (!m_v2BootstrapDone)
        return;

    const ChannelList channels = m_v2DeviceChannels.value(deviceExtId);
    if (channels.isEmpty())
        return;

    qCInfo(adapterLog).noquote()
        << "HueAdapter::updateDeviceEffectsFromLight - refreshing effects for" << deviceExtId;
    emit deviceUpdated(updated, channels);
}

void HueAdapter::finalizeV2SnapshotIfReady()
{
    if (!m_v2BootstrapDone
        && m_v2SnapshotPending == 0
        && m_pendingV2DeviceFetch.isEmpty()
        && m_v2DeviceFetchQueue.isEmpty()) {
        if (m_v2SnapshotFailedThisCycle) {
            qCWarning(adapterLog)
                << "HueAdapter::buildDevicesFromV2Snapshots - skipping rebuild due to failed resource snapshots";
            m_v2BootstrapDone = true;
            m_v2SnapshotFailedThisCycle = false;
            return;
        }
        buildDevicesFromV2Snapshots();
    }
}

void HueAdapter::buildDevicesFromV2Snapshots()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (m_v2BootstrapDone)
        return;

    m_deviceIdToExternalId.clear();
    m_v2DeviceChannels.clear();
    m_buttonResourceToChannel.clear();
    m_channelBindings.clear();
    m_v2ResourceToDevice.clear();
    m_deviceToLightResource.clear();
    m_v2RoomMemberships.clear();

    QSet<QString> snapshotDeviceIds;

    // Start from cached metadata (filled via snapshots and lazy fetches).
    m_v2Devices = m_v2DeviceInfoCache;

    // ---------------------------------------------------------------------
    // 1) Update cached devices from /resource/device
    // ---------------------------------------------------------------------
    const QJsonArray deviceArray = m_v2SnapshotByType.value(QStringLiteral("device"));
    for (const QJsonValue &val : deviceArray) {
        if (!val.isObject())
            continue;
        const QJsonObject devObj = val.toObject();
        const QString deviceId = devObj.value(QStringLiteral("id")).toString();
        if (deviceId.isEmpty())
            continue;

        QString externalId = m_deviceIdToExternalId.value(deviceId);
        if (externalId.isEmpty()) {
            externalId = deviceId;
            m_deviceIdToExternalId.insert(deviceId, externalId);
        }
        snapshotDeviceIds.insert(externalId);

        Device device = m_v2DeviceInfoCache.value(externalId);
        device.id = externalId;

        const QJsonObject metaObj = devObj.value(QStringLiteral("metadata")).toObject();
        const QJsonObject productObj = devObj.value(QStringLiteral("product_data")).toObject();

        const QString name = metaObj.value(QStringLiteral("name")).toString();
        const QString productName = productObj.value(QStringLiteral("product_name")).toString();
        if (!name.isEmpty()) {
            device.name = name;
            completeRenameVerification(device.id);
        } else if (device.name.isEmpty()) {
            if (!productName.isEmpty()) {
                device.name = productName;
                qCInfo(adapterLog).noquote()
                    << "HueAdapter::buildDevicesFromV2Snapshots - product name fallback for" << device.id
                    << "->" << productName;
            } else {
                device.name = QStringLiteral("Hue Device");
            }
        }

        const QString manufacturer = productObj.value(QStringLiteral("manufacturer_name")).toString();
        if (!manufacturer.isEmpty())
            device.manufacturer = manufacturer;
        const QString model = productObj.value(QStringLiteral("model_id")).toString();
        if (!model.isEmpty())
            device.model = model;
        const QString firmware = productObj.value(QStringLiteral("software_version")).toString();
        if (!firmware.isEmpty())
            device.firmware = firmware;

        device.meta = devObj;
        attachServiceRefs(device.meta);

        applyProductNumberMapping(device, productObj);
        applyDeviceClassFromMetadata(device, metaObj, productObj);
        applyHueEffects(device, devObj);

        m_v2DeviceInfoCache.insert(externalId, device);
        m_v2Devices.insert(externalId, device);
    }

    // Retain mappings for devices that only exist via lazy fetches.
    const auto cacheKeys = m_v2DeviceInfoCache.keys();
    for (const QString &extId : cacheKeys) {
        if (!m_deviceIdToExternalId.contains(extId))
            m_deviceIdToExternalId.insert(extId, extId);
    }

    const QJsonArray lightEffectsArray = m_v2SnapshotByType.value(QStringLiteral("light"));
    for (const QJsonValue &val : lightEffectsArray) {
        if (!val.isObject())
            continue;
        const QJsonObject lightObj = val.toObject();
        const QString ownerId = lightObj.value(QStringLiteral("owner")).toObject().value(QStringLiteral("rid")).toString();
        if (ownerId.isEmpty())
            continue;
        const QString deviceExtId = deviceExtIdForResource(QStringLiteral("device"), ownerId);
        if (deviceExtId.isEmpty())
            continue;
        const QString lightId = lightObj.value(QStringLiteral("id")).toString();
        Device device = m_v2DeviceInfoCache.value(deviceExtId);
        if (!lightId.isEmpty())
            m_deviceToLightResource.insert(deviceExtId, lightId);
        if (device.id.isEmpty())
            continue;
        Device updated = device;
        applyHueEffects(updated, lightObj);
        if (effectsEqual(device.effects, updated.effects))
            continue;
        m_v2DeviceInfoCache.insert(deviceExtId, updated);
        m_v2Devices.insert(deviceExtId, updated);
    }

    auto channelListForDevice = [this](const QString &deviceExtId) -> ChannelList & {
        return m_v2DeviceChannels[deviceExtId];
    };

    buildRoomsFromV2Snapshot();
    buildGroupsFromV2Snapshot();

    auto propagateDeviceClass = [this](const QString &deviceExtId, DeviceClass cls) {
        if (deviceExtId.isEmpty())
            return;
        auto it = m_v2DeviceInfoCache.find(deviceExtId);
        if (it != m_v2DeviceInfoCache.end()) {
            if (it->id.isEmpty())
                it->id = deviceExtId;
            it->deviceClass = cls;
            return;
        }
        Device placeholder;
        placeholder.id = deviceExtId;
        placeholder.deviceClass = cls;
        m_v2DeviceInfoCache.insert(deviceExtId, placeholder);
    };

    // ---------------------------------------------------------------------
    // 2) Ensure all owner RIDs have metadata (trigger lazy fetches if needed)
    // ---------------------------------------------------------------------
    QSet<QString> missingOwners;
    const auto markMissingOwner = [this, &missingOwners](const QString &ownerId) {
        if (ownerId.isEmpty())
            return;
        if (m_failedV2DeviceFetch.contains(ownerId))
            return;
        const QString externalId = m_deviceIdToExternalId.value(ownerId, ownerId);
        auto it = m_v2DeviceInfoCache.constFind(externalId);
        if (it != m_v2DeviceInfoCache.constEnd() && !it->meta.isEmpty())
            return;
        missingOwners.insert(ownerId);
    };

    const auto collectOwners = [&](const QString &resourceType) {
        const QJsonArray arr = m_v2SnapshotByType.value(resourceType);
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            markMissingOwner(deviceId);
        }
    };

    collectOwners(QStringLiteral("light"));
    collectOwners(QStringLiteral("motion"));
    collectOwners(QStringLiteral("tamper"));
    collectOwners(QStringLiteral("temperature"));
    collectOwners(QStringLiteral("light_level"));
    collectOwners(QStringLiteral("device_power"));
    collectOwners(QStringLiteral("button"));
    collectOwners(QStringLiteral("device_software_update"));

    if (!missingOwners.isEmpty()) {
        for (const QString &deviceId : std::as_const(missingOwners)) {
            if (deviceId.isEmpty())
                continue;
            if (m_pendingV2DeviceFetch.contains(deviceId))
                continue;
            if (m_v2DeviceFetchQueue.contains(deviceId))
                continue;

            qCInfo(adapterLog) << "HueAdapter::buildDevicesFromV2Snapshots - fetching metadata for missing device"
                               << deviceId;
            fetchV2DeviceResource(deviceId);
        }
        return;
    }

    auto deviceForHueOwner = [this](const QString &ownerId) -> QString {
        if (ownerId.isEmpty())
            return {};

        QString mapped = m_deviceIdToExternalId.value(ownerId);
        if (!mapped.isEmpty() && m_v2Devices.contains(mapped))
            return mapped;

        // No device metadata yet; create placeholder and request it lazily.
        mapped = ownerId;
        Device &device = m_v2Devices[mapped];
        if (device.id.isEmpty())
            device.id = mapped;
        if (!m_v2DeviceInfoCache.contains(mapped))
            m_v2DeviceInfoCache.insert(mapped, device);
        m_deviceIdToExternalId.insert(ownerId, mapped);
        fetchV2DeviceResource(ownerId);
        return mapped;
    };

    // ---------------------------------------------------------------------
    // 3) Attach light services
    // ---------------------------------------------------------------------
    const QJsonArray lightArray = m_v2SnapshotByType.value(QStringLiteral("light"));
    for (const QJsonValue &val : lightArray) {
        if (!val.isObject())
            continue;
        const QJsonObject resObj = val.toObject();
        const QJsonObject lightMetaObj = resObj.value(QStringLiteral("metadata")).toObject();
        const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
        const QString ownerType = ownerObj.value(QStringLiteral("rtype")).toString();
        if (ownerType != QStringLiteral("device"))
            continue;
        const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
        const QString deviceExtId = deviceForHueOwner(deviceId);
        if (deviceExtId.isEmpty())
            continue;

        Device &device = m_v2Devices[deviceExtId];
        if (device.id.isEmpty())
            device.id = deviceExtId;

        // If the bridge did not provide a /resource/device snapshot (or it
        // failed), we may not have a human-readable name yet. In that case,
        // derive the name from the first attached light service metadata.
        if (device.name.isEmpty()) {
            const QString lightName = lightMetaObj.value(QStringLiteral("name")).toString();
            if (!lightName.isEmpty()) {
                device.name = lightName;
                qCInfo(adapterLog).noquote()
                    << "HueAdapter::buildDevicesFromV2Snapshots - light service name fallback for"
                    << device.id << "->" << lightName;
            }
        }

        ChannelList &channels = channelListForDevice(deviceExtId);

        // Mark device as light by default when a light service is present,
        // but never override explicit classifications like Plug or Button.
        if (device.deviceClass == DeviceClass::Unknown) {
            device.deviceClass = DeviceClass::Light;
            propagateDeviceClass(deviceExtId, device.deviceClass);
        }

        const QString lightServiceId = resObj.value(QStringLiteral("id")).toString();
        if (!lightServiceId.isEmpty()) {
            m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("light"), lightServiceId), deviceExtId);
        }
        const QJsonObject onObj = resObj.value(QStringLiteral("on")).toObject();
        const QJsonObject dimObj = resObj.value(QStringLiteral("dimming")).toObject();
        const QJsonObject ctObj  = resObj.value(QStringLiteral("color_temperature")).toObject();
        const QJsonObject colorObj = resObj.value(QStringLiteral("color")).toObject();
        const auto bindChannel = [this, &deviceExtId, &lightServiceId](const QString &channelId) {
            if (lightServiceId.isEmpty())
                return;
            HueChannelBinding binding;
            binding.resourceId = lightServiceId;
            binding.resourceType = QStringLiteral("light");
            m_channelBindings.insert(channelBindingKey(deviceExtId, channelId), binding);
            if (!m_deviceToLightResource.contains(deviceExtId))
                m_deviceToLightResource.insert(deviceExtId, lightServiceId);
        };

        // Power channel
        if (!onObj.isEmpty()) {
            Channel power;
            power.id = QStringLiteral("on");
            power.name       = QStringLiteral("Power");
            power.kind       = ChannelKind::PowerOnOff;
            power.dataType   = ChannelDataType::Bool;
            power.flags      = ChannelFlagDefaultWrite;
            channels.push_back(power);
            bindChannel(power.id);
        }

        // Brightness channel
        if (!dimObj.isEmpty()) {
            Channel brightness;
            brightness.id = QStringLiteral("bri");
            brightness.name       = QStringLiteral("Brightness");
            brightness.kind       = ChannelKind::Brightness;
            brightness.dataType   = ChannelDataType::Float;
            brightness.flags      = ChannelFlagDefaultWrite;
            brightness.minValue   = 0.0;
            brightness.maxValue   = 100.0;
            brightness.stepValue  = 0.1;
            channels.push_back(brightness);
            bindChannel(brightness.id);
        }

        // Color temperature + presets
        double ctMin = 153.0;
        double ctMax = 500.0;
        const QJsonObject ctSchema = ctObj.value(QStringLiteral("mirek_schema")).toObject();
        if (!ctSchema.isEmpty()) {
            ctMin = ctSchema.value(QStringLiteral("mirek_minimum")).toDouble(ctMin);
            ctMax = ctSchema.value(QStringLiteral("mirek_maximum")).toDouble(ctMax);
        }

        if (!ctObj.isEmpty()) {
            Channel colorTemp;
            colorTemp.id = QStringLiteral("ct");
            colorTemp.name       = QStringLiteral("Color temperature");
            colorTemp.kind       = ChannelKind::ColorTemperature;
            colorTemp.dataType   = ChannelDataType::Int;
            colorTemp.flags      = ChannelFlagDefaultWrite;
            colorTemp.unit       = QStringLiteral("mired");
            colorTemp.minValue   = ctMin;
            colorTemp.maxValue   = ctMax;
            colorTemp.stepValue  = 1.0;
            channels.push_back(colorTemp);
            bindChannel(colorTemp.id);
            Channel preset;
            preset.id = QStringLiteral("ctPreset");
            preset.name       = QStringLiteral("Color temperature preset");
            preset.kind       = ChannelKind::ColorTemperaturePreset;
            preset.dataType   = ChannelDataType::Enum;
            preset.flags      = ChannelFlagDefaultWrite;
            preset.minValue   = 0.0;
            preset.maxValue   = 4.0;
            preset.stepValue  = 1.0;
            channels.push_back(preset);
            bindChannel(preset.id);
        }

        // Color channel (if the light supports color)
        const QJsonObject xyObj = colorObj.value(QStringLiteral("xy")).toObject();
        if (!xyObj.isEmpty()) {
            Channel colorChannel;
            colorChannel.id = QStringLiteral("color");
            colorChannel.name       = QStringLiteral("Color");
            colorChannel.kind       = ChannelKind::ColorRGB;
            colorChannel.dataType   = ChannelDataType::Color;
            colorChannel.flags      = ChannelFlagDefaultWrite;

            const QJsonObject gamutObj = colorObj.value(QStringLiteral("gamut")).toObject();
                if (!gamutObj.isEmpty()) {
                    // Convert Hue gamut object into the canonical
                    // colorCapabilities structure used by the UI.
                    QJsonArray gamutArray;
                const QJsonObject red   = gamutObj.value(QStringLiteral("red")).toObject();
                const QJsonObject green = gamutObj.value(QStringLiteral("green")).toObject();
                const QJsonObject blue  = gamutObj.value(QStringLiteral("blue")).toObject();
                const auto pointToArray = [](const QJsonObject &p) {
                    QJsonArray arr;
                    arr.append(p.value(QStringLiteral("x")).toDouble(0.0));
                    arr.append(p.value(QStringLiteral("y")).toDouble(0.0));
                    return arr;
                };
                if (!red.isEmpty())
                    gamutArray.append(pointToArray(red));
                if (!green.isEmpty())
                    gamutArray.append(pointToArray(green));
                if (!blue.isEmpty())
                    gamutArray.append(pointToArray(blue));

                if (gamutArray.size() >= 3) {
                    QJsonObject caps;
                    caps.insert(QStringLiteral("space"), QStringLiteral("cie1931_xy"));
                    caps.insert(QStringLiteral("gamut"), gamutArray);
                    colorChannel.meta.insert(QStringLiteral("colorCapabilities"), caps);

                    if (!lightServiceId.isEmpty()) {
                        HueGamut g;
                        const auto pointFromArray = [](const QJsonArray &arr) -> QPointF {
                            if (arr.size() >= 2) {
                                return QPointF(arr.at(0).toDouble(0.0), arr.at(1).toDouble(0.0));
                            }
                            return QPointF();
                        };
                        g.p1 = pointFromArray(gamutArray.at(0).toArray());
                        g.p2 = pointFromArray(gamutArray.at(1).toArray());
                        g.p3 = pointFromArray(gamutArray.at(2).toArray());
                        if (g.isValid()) {
                            m_gamutByLightId.insert(lightServiceId, g);
                        }
                    }
                }
            }

            channels.push_back(colorChannel);
            bindChannel(colorChannel.id);
        }
    }

    // ---------------------------------------------------------------------
    // 4) Attach sensor-like services: motion, temperature, light_level, battery
    // ---------------------------------------------------------------------
    const auto attachBoolSensor = [&](const QString &typeKey,
                                      const QString &channelId,
                                      const QString &channelName,
                                      ChannelKind kind,
                                      const char *valuePath) {
        const QJsonArray arr = m_v2SnapshotByType.value(typeKey);
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            ChannelList &channels = channelListForDevice(deviceExtId);
            Device &device = m_v2Devices[deviceExtId];
            if (device.deviceClass == DeviceClass::Unknown) {
                device.deviceClass = DeviceClass::Sensor;
                propagateDeviceClass(deviceExtId, device.deviceClass);
            }
            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(typeKey, resourceId), deviceExtId);
            }
            Channel ch;
            ch.id = channelId;
            ch.name       = channelName;
            ch.kind       = kind;
            ch.dataType   = ChannelDataType::Bool;
            ch.flags      = ChannelFlagDefaultRead;
            channels.push_back(ch);

            if (typeKey == QStringLiteral("motion")) {
                const QJsonObject sensitivityObj = resObj.value(QStringLiteral("sensitivity")).toObject();
                if (sensitivityObj.contains(QStringLiteral("sensitivity"))) {
                    bool hasChannel = false;
                    for (const Channel &existing : std::as_const(channels)) {
                        if (existing.id == QStringLiteral("motion_sensitivity")) {
                            hasChannel = true;
                            break;
                        }
                    }
                    if (!hasChannel) {
                        Channel sensitivity;
                        sensitivity.id = QStringLiteral("motion_sensitivity");
                        sensitivity.name = QStringLiteral("Motion sensitivity");
                        sensitivity.kind = ChannelKind::Unknown;
                        sensitivity.dataType = ChannelDataType::Enum;
                        sensitivity.flags = ChannelFlagDefaultRead;
                        sensitivity.meta.insert(QStringLiteral("enumName"), QStringLiteral("SensitivityLevel"));
                        const int values[] = {
                            static_cast<int>(phicore::SensitivityLevel::Low),
                            static_cast<int>(phicore::SensitivityLevel::Medium),
                            static_cast<int>(phicore::SensitivityLevel::High),
                            static_cast<int>(phicore::SensitivityLevel::VeryHigh),
                        };
                        for (const int value : values) {
                            AdapterConfigOption opt;
                            opt.value = QString::number(value);
                            const QString label = sensitivityLabel(value);
                            opt.label = label.isEmpty() ? QString::number(value) : label;
                            sensitivity.choices.push_back(opt);
                        }
                        channels.push_back(sensitivity);
                    }
                }
            }
        }
    };

    attachBoolSensor(QStringLiteral("motion"),
                     QStringLiteral("motion"),
                     QStringLiteral("Motion"),
                     ChannelKind::Motion,
                     "motion");
    attachBoolSensor(QStringLiteral("tamper"),
                     QStringLiteral("tamper"),
                     QStringLiteral("Tamper"),
                     ChannelKind::Tamper,
                     "tamper");

    // Temperature
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("temperature"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            ChannelList &channels = channelListForDevice(deviceExtId);
            Device &device = m_v2Devices[deviceExtId];
            if (device.deviceClass == DeviceClass::Unknown) {
                device.deviceClass = DeviceClass::Sensor;
                propagateDeviceClass(deviceExtId, device.deviceClass);
            }
            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("temperature"), resourceId), deviceExtId);
            }
            Channel temp;
            temp.id = QStringLiteral("temperature");
            temp.name       = QStringLiteral("Temperature");
            temp.kind       = ChannelKind::Temperature;
            temp.dataType   = ChannelDataType::Float;
            temp.flags      = ChannelFlagDefaultRead;
            temp.unit       = QStringLiteral("°C");
            channels.push_back(temp);
        }
    }

    // Light level
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("light_level"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            ChannelList &channels = channelListForDevice(deviceExtId);
            Device &device = m_v2Devices[deviceExtId];
            if (device.deviceClass == DeviceClass::Unknown) {
                device.deviceClass = DeviceClass::Sensor;
                propagateDeviceClass(deviceExtId, device.deviceClass);
            }
            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("light_level"), resourceId),
                                            deviceExtId);
            }
            Channel illum;
            illum.id = QStringLiteral("illuminance");
            illum.name       = QStringLiteral("Illuminance");
            illum.kind       = ChannelKind::Illuminance;
            illum.dataType   = ChannelDataType::Int;
            illum.flags      = ChannelFlagDefaultRead;
            illum.unit       = QStringLiteral("lx");
            channels.push_back(illum);
        }
    }

    // Battery (device_power)
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("device_power"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            ChannelList &channels = channelListForDevice(deviceExtId);
            Device &device = m_v2Devices[deviceExtId];
            if (device.deviceClass == DeviceClass::Unknown) {
                device.deviceClass = DeviceClass::Sensor;
                propagateDeviceClass(deviceExtId, device.deviceClass);
            }
            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("device_power"), resourceId),
                                            deviceExtId);
            }
            device.flags |= DeviceFlag::DeviceFlagBattery;
            Channel bat;
            bat.id = QStringLiteral("battery");
            bat.name       = QStringLiteral("Battery");
            bat.kind       = ChannelKind::Battery;
            bat.dataType   = ChannelDataType::Int;
            bat.flags      = ChannelFlagDefaultRead;
            bat.minValue   = 0.0;
            bat.maxValue   = 100.0;
            bat.stepValue  = 1.0;
            channels.push_back(bat);
        }
    }

    // Zigbee connectivity (signal strength / status)
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("zigbee_connectivity"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            ChannelList &channels = channelListForDevice(deviceExtId);
            bool hasStatusChannel = false;
            for (const Channel &existing : std::as_const(channels)) {
                if (existing.id == QLatin1String(kZigbeeStatusChannelId)) {
                    hasStatusChannel = true;
                    break;
                }
            }
            if (!hasStatusChannel) {
                Channel connectivity;
                connectivity.id = QString::fromLatin1(kZigbeeStatusChannelId);
                connectivity.name       = QStringLiteral("Connectivity");
                connectivity.kind       = ChannelKind::ConnectivityStatus;
                connectivity.dataType   = ChannelDataType::Enum;
                connectivity.flags      = ChannelFlagDefaultRead;
                channels.push_back(connectivity);
            }

            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("zigbee_connectivity"), resourceId),
                                            deviceExtId);
            }

            updateDeviceConnectivityMeta(deviceExtId, resObj, nowMs);
        }
    }

    // Device software updates
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("device_software_update"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            ChannelList &channels = channelListForDevice(deviceExtId);
            bool hasUpdateChannel = false;
            for (const Channel &existing : std::as_const(channels)) {
                if (existing.id == QLatin1String(kDeviceSoftwareUpdateChannelId)) {
                    hasUpdateChannel = true;
                    break;
                }
            }
            if (!hasUpdateChannel) {
                Channel updateChannel;
                updateChannel.id = QString::fromLatin1(kDeviceSoftwareUpdateChannelId);
                updateChannel.name       = QStringLiteral("Firmware Update");
                updateChannel.kind       = ChannelKind::DeviceSoftwareUpdate;
                updateChannel.dataType   = ChannelDataType::Enum;
                updateChannel.flags      = ChannelFlagDefaultRead;
                channels.push_back(updateChannel);
            }

            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("device_software_update"), resourceId),
                                            deviceExtId);
            }

            updateDeviceSoftwareUpdateMeta(deviceExtId, resObj, nowMs);
        }
    }

    // Zigbee device discovery status
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("zigbee_device_discovery"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            const QString resourceId = resObj.value(QStringLiteral("id")).toString();
            if (!resourceId.isEmpty()) {
                m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("zigbee_device_discovery"), resourceId),
                                            deviceExtId);
            }
            updateZigbeeDeviceDiscoveryMeta(deviceExtId, resObj, nowMs);
        }
    }

    // ---------------------------------------------------------------------
    // 5) Buttons and rotary (tap dials, remotes, etc.)
    // ---------------------------------------------------------------------
    {
        struct ButtonEntry {
            int     controlId = 1;
            QString resourceId;
        };
        QHash<QString, QList<ButtonEntry>> buttonsByDevice;

        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("button"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
            if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
            const QString deviceExtId = deviceForHueOwner(deviceId);
            if (deviceExtId.isEmpty())
                continue;

            const QString buttonId = resObj.value(QStringLiteral("id")).toString();
            const QJsonObject metaObj = resObj.value(QStringLiteral("metadata")).toObject();
            int controlId = metaObj.value(QStringLiteral("control_id")).toInt(0);
            if (controlId <= 0)
                controlId = 1;

            ButtonEntry entry;
            entry.controlId = controlId;
            entry.resourceId = buttonId;
            buttonsByDevice[deviceExtId].append(entry);
        }

        const auto deviceIds = buttonsByDevice.keys();
        for (const QString &deviceExtId : deviceIds) {
            const QList<ButtonEntry> entries = buttonsByDevice.value(deviceExtId);
            const bool singleButton = (entries.size() <= 1);
            ChannelList &channels = channelListForDevice(deviceExtId);

            for (const ButtonEntry &entry : entries) {
                Channel button;
                if (singleButton) {
                    button.id = QStringLiteral("button");
                    button.name       = QStringLiteral("Button");
                } else {
                    button.id = QStringLiteral("button%1").arg(entry.controlId);
                    button.name       = QStringLiteral("Button %1").arg(entry.controlId);
                }
                button.kind       = ChannelKind::ButtonEvent;
                button.dataType   = ChannelDataType::Int;
                button.flags      = ChannelFlag::ChannelFlagReportable | ChannelFlag::ChannelFlagRetained;
                channels.push_back(button);

                if (!entry.resourceId.isEmpty()) {
                    m_buttonResourceToChannel.insert(entry.resourceId, button.id);
                }
            }

            Device &device = m_v2Devices[deviceExtId];
            if (device.deviceClass == DeviceClass::Unknown) {
                device.deviceClass = DeviceClass::Button;
                propagateDeviceClass(deviceExtId, device.deviceClass);
            }
        }
    }

    {
        QSet<QString> devicesWithDial;
        const auto deviceIds = m_v2Devices.keys();
        for (const QString &deviceExtId : deviceIds) {
            const Device device = m_v2Devices.value(deviceExtId);
            const QJsonArray services = device.meta.value(QStringLiteral("services")).toArray();
            for (const QJsonValue &serviceVal : services) {
                const QJsonObject serviceObj = serviceVal.toObject();
                const QString rtype = serviceObj.value(QStringLiteral("rtype")).toString();
                if (rtype == QStringLiteral("relative_rotary")) {
                    devicesWithDial.insert(deviceExtId);
                    break;
                }
            }
        }

        for (const QString &deviceExtId : devicesWithDial) {
            ChannelList &channels = channelListForDevice(deviceExtId);
            Channel dial;
            dial.id = QStringLiteral("dial");
            dial.name       = QStringLiteral("Dial rotation");
            dial.kind       = ChannelKind::RelativeRotation;
            dial.dataType   = ChannelDataType::Int;
            dial.flags      = ChannelFlag::ChannelFlagReportable | ChannelFlag::ChannelFlagRetained;
            channels.push_back(dial);
        }
    }

    // ---------------------------------------------------------------------
    // 6) Emit devices and channels
    // ---------------------------------------------------------------------
    const auto deviceKeys = m_v2Devices.keys();
    bool devicesPendingMetadata = false;
    for (const QString &deviceExtId : deviceKeys) {
        const ChannelList channels = m_v2DeviceChannels.value(deviceExtId);
        if (channels.isEmpty())
            continue;

        Device device = m_v2Devices.value(deviceExtId);
        if (device.meta.isEmpty()) {
            devicesPendingMetadata = true;
            fetchV2DeviceResource(deviceExtId);
            continue;
        }

        // Ensure a stable externalId and a non-empty name for all devices
        // before handing them to the core/DB layer.
        if (device.id.isEmpty())
            device.id = deviceExtId;
        if (device.name.isEmpty()) {
            const QJsonObject metaObj = device.meta.value(QStringLiteral("metadata")).toObject();
            const QString metaName = metaObj.value(QStringLiteral("name")).toString();
            if (!metaName.isEmpty()) {
                device.name = metaName;
            } else {
                const QJsonObject productObj = device.meta.value(QStringLiteral("product_data")).toObject();
                const QString productName = productObj.value(QStringLiteral("product_name")).toString();
                if (!productName.isEmpty())
                    device.name = productName;
                else
                    device.name = QStringLiteral("Hue Device");
            }
        }

        emit deviceUpdated(device, channels);
    }

    if (devicesPendingMetadata)
        return;

    const QSet<QString> removedDevices = m_knownDeviceExternalIds - snapshotDeviceIds;
    if (!removedDevices.isEmpty()) {
        for (const QString &extId : removedDevices) {
            if (extId.isEmpty())
                continue;
            qCInfo(adapterLog).noquote()
                << "HueAdapter::buildDevicesFromV2Snapshots - removing missing device" << extId;
            emit deviceRemoved(extId);
            m_v2Devices.remove(extId);
            m_v2DeviceChannels.remove(extId);
            m_v2DeviceInfoCache.remove(extId);
        }
    }
    m_knownDeviceExternalIds = snapshotDeviceIds;

    // ---------------------------------------------------------------------
    // 7) Seed initial channel state from snapshots (after devices exist)
    // ---------------------------------------------------------------------

    // Lights: on/bri/ct/ctPreset/color
    for (const QJsonValue &val : lightArray) {
        if (!val.isObject())
            continue;
        const QJsonObject resObj = val.toObject();
        const QJsonObject ownerObj = resObj.value(QStringLiteral("owner")).toObject();
        if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
            continue;
        const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
        const QString deviceExtId = deviceForHueOwner(deviceId);
        if (deviceExtId.isEmpty())
            continue;

        const QJsonObject onObj = resObj.value(QStringLiteral("on")).toObject();
        const QJsonObject dimObj = resObj.value(QStringLiteral("dimming")).toObject();
        const QJsonObject ctObj  = resObj.value(QStringLiteral("color_temperature")).toObject();
        const QJsonObject colorObj = resObj.value(QStringLiteral("color")).toObject();

        if (onObj.contains(QStringLiteral("on"))) {
            const bool on = onObj.value(QStringLiteral("on")).toBool(false);
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("on"),
                                     on,
                                     nowMs);
        }

        if (dimObj.contains(QStringLiteral("brightness"))) {
            const double percent = dimObj.value(QStringLiteral("brightness")).toDouble(-1.0);
            if (percent >= 0.0) {
                const double percentValue = qBound(0.0, percent, 100.0);
                emit channelStateUpdated(deviceExtId,
                                         QStringLiteral("bri"),
                                         percentValue,
                                         nowMs);
            }
        }

        if (ctObj.contains(QStringLiteral("mirek"))) {
            const int ctMired = ctObj.value(QStringLiteral("mirek")).toInt();
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("ct"),
                                     ctMired,
                                     nowMs);
        }

        const QJsonObject xyObj = colorObj.value(QStringLiteral("xy")).toObject();
        if (xyObj.contains(QStringLiteral("x")) && xyObj.contains(QStringLiteral("y"))) {
            const double x = xyObj.value(QStringLiteral("x")).toDouble(0.0);
            const double y = xyObj.value(QStringLiteral("y")).toDouble(0.0);
            const Color color = colorFromXy(x, y, 1.0);
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("color"),
                                     QVariant::fromValue(color),
                                     nowMs);
        }
    }

    // Motion sensors: motion
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("motion"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            handleV2MotionResource(val.toObject(), nowMs);
        }
    }

    // Tamper sensors
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("tamper"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            handleV2TamperResource(val.toObject(), nowMs);
        }
    }

    // Temperature
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("temperature"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            handleV2TemperatureResource(val.toObject(), nowMs);
        }
    }

    // Light level
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("light_level"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            handleV2LightLevelResource(val.toObject(), nowMs);
        }
    }

    // Battery
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("device_power"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            handleV2DevicePowerResource(val.toObject(), nowMs);
        }
    }

    // Buttons: emit the last_event from the snapshot once devices/channels exist.
    {
        const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("button"));
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject resObj = val.toObject();
            handleV2ButtonResource(resObj, nowMs);
        }
    }
    if (!m_pendingV2DeviceFetch.isEmpty() || !m_v2DeviceFetchQueue.isEmpty())
        return;

    if (!m_pendingConnectivityStatus.isEmpty()) {
        for (auto it = m_pendingConnectivityStatus.cbegin(); it != m_pendingConnectivityStatus.cend(); ++it) {
            emit channelStateUpdated(it.key(),
                                     QString::fromLatin1(kZigbeeStatusChannelId),
                                     static_cast<int>(it.value()),
                                     nowMs);
        }
        m_pendingConnectivityStatus.clear();
    }

    if (!m_pendingDeviceSoftwareUpdates.isEmpty()) {
        for (auto it = m_pendingDeviceSoftwareUpdates.cbegin(); it != m_pendingDeviceSoftwareUpdates.cend(); ++it) {
            emit channelStateUpdated(it.key(),
                                     QString::fromLatin1(kDeviceSoftwareUpdateChannelId),
                                     it.value(),
                                     nowMs);
        }
        m_pendingDeviceSoftwareUpdates.clear();
    }

    if (!m_pendingDiscoveryDeviceUpdates.isEmpty()) {
        for (const QString &extId : std::as_const(m_pendingDiscoveryDeviceUpdates)) {
            const Device device = m_v2DeviceInfoCache.value(extId);
            const ChannelList channels = m_v2DeviceChannels.value(extId);
            if (device.id.isEmpty() || channels.isEmpty())
                continue;
            emit deviceUpdated(device, channels);
        }
        m_pendingDiscoveryDeviceUpdates.clear();
    }

    if (m_sceneSnapshotDirty) {
        m_sceneSnapshotDirty = false;
        const QList<Scene> scenes = m_v2Scenes.values();
        if (!scenes.isEmpty())
            emit scenesUpdated(scenes);
    }

    m_v2BootstrapDone = true;
    emit fullSyncCompleted();
}

void HueAdapter::handleV2MotionResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QJsonObject motionObj = resObj.value(QStringLiteral("motion")).toObject();
    bool hasValue = false;
    bool motion = false;
    if (motionObj.contains(QStringLiteral("motion"))) {
        motion = motionObj.value(QStringLiteral("motion")).toBool(false);
        hasValue = true;
    } else {
        const QJsonObject reportObj = motionObj.value(QStringLiteral("motion_report")).toObject();
        if (reportObj.contains(QStringLiteral("motion"))) {
            motion = reportObj.value(QStringLiteral("motion")).toBool(false);
            hasValue = true;
        }
    }
    if (!hasValue)
        return;

    qint64 eventTs = nowMs;
    const QJsonObject reportObj = motionObj.value(QStringLiteral("motion_report")).toObject();
    const qint64 reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("changed")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    emit channelStateUpdated(deviceExtId,
                             QStringLiteral("motion"),
                             motion,
                             eventTs);

    const QJsonObject sensitivityObj = resObj.value(QStringLiteral("sensitivity")).toObject();
    if (sensitivityObj.contains(QStringLiteral("sensitivity"))) {
        const int raw = sensitivityObj.value(QStringLiteral("sensitivity")).toInt(0);
        const int mapped = mapHueSensitivityToLevel(raw);
        if (mapped != static_cast<int>(phicore::SensitivityLevel::Unknown)) {
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("motion_sensitivity"),
                                     mapped,
                                     eventTs);
        }
    }
}

void HueAdapter::handleV2TamperResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QJsonObject tamperObj = resObj.value(QStringLiteral("tamper")).toObject();
    bool hasValue = false;
    bool tamper = false;
    if (tamperObj.contains(QStringLiteral("tamper"))) {
        tamper = tamperObj.value(QStringLiteral("tamper")).toBool(false);
        hasValue = true;
    } else {
        const QJsonObject reportObj = tamperObj.value(QStringLiteral("tamper_report")).toObject();
        if (reportObj.contains(QStringLiteral("tamper"))) {
            tamper = reportObj.value(QStringLiteral("tamper")).toBool(false);
            hasValue = true;
        }
    }
    if (!hasValue)
        return;

    qint64 eventTs = nowMs;
    const QJsonObject reportObj = tamperObj.value(QStringLiteral("tamper_report")).toObject();
    const qint64 reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("changed")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    emit channelStateUpdated(deviceExtId,
                             QStringLiteral("tamper"),
                             tamper,
                             eventTs);
}

void HueAdapter::handleV2TemperatureResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QJsonObject tempObj = resObj.value(QStringLiteral("temperature")).toObject();
    double rawTemp = 0.0;
    bool hasValue = false;
    if (tempObj.contains(QStringLiteral("temperature"))) {
        rawTemp = tempObj.value(QStringLiteral("temperature")).toDouble(0.0);
        hasValue = true;
    } else {
        const QJsonObject reportObj = tempObj.value(QStringLiteral("temperature_report")).toObject();
        if (reportObj.contains(QStringLiteral("temperature"))) {
            rawTemp = reportObj.value(QStringLiteral("temperature")).toDouble(0.0);
            hasValue = true;
        }
    }
    if (!hasValue)
        return;

    qint64 eventTs = nowMs;
    const QJsonObject reportObj = tempObj.value(QStringLiteral("temperature_report")).toObject();
    const qint64 reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("changed")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    double celsius = rawTemp;
    if (qAbs(rawTemp) > 200.0)
        celsius = rawTemp / 100.0;

    emit channelStateUpdated(deviceExtId,
                             QStringLiteral("temperature"),
                             celsius,
                             eventTs);
}

void HueAdapter::handleV2LightLevelResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QJsonObject lightObj = resObj.value(QStringLiteral("light")).toObject();
    bool hasValue = false;
    double lux = 0.0;
    const QJsonObject reportObj = lightObj.value(QStringLiteral("light_level_report")).toObject();
    if (reportObj.contains(QStringLiteral("lux"))) {
        lux = reportObj.value(QStringLiteral("lux")).toDouble(0.0);
        hasValue = true;
    } else if (lightObj.contains(QStringLiteral("lux"))) {
        lux = lightObj.value(QStringLiteral("lux")).toDouble(0.0);
        hasValue = true;
    } else if (reportObj.contains(QStringLiteral("light_level"))) {
        const int lightLevel = reportObj.value(QStringLiteral("light_level")).toInt();
        lux = std::pow(10.0, (static_cast<double>(lightLevel) - 1.0) / 10000.0);
        hasValue = true;
    } else if (lightObj.contains(QStringLiteral("light_level"))) {
        const int lightLevel = lightObj.value(QStringLiteral("light_level")).toInt();
        lux = std::pow(10.0, (static_cast<double>(lightLevel) - 1.0) / 10000.0);
        hasValue = true;
    }
    if (!hasValue)
        return;

    qint64 eventTs = nowMs;
    const qint64 reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("changed")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    emit channelStateUpdated(deviceExtId,
                             QStringLiteral("illuminance"),
                             qRound(lux),
                             eventTs);
}

void HueAdapter::handleV2DevicePowerResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QJsonObject powerObj = resObj.value(QStringLiteral("power_state")).toObject();
    if (!powerObj.contains(QStringLiteral("battery_level")))
        return;

    const int battery = powerObj.value(QStringLiteral("battery_level")).toInt(-1);
    if (battery < 0)
        return;

    Device &device = m_v2Devices[deviceExtId];
    device.flags |= DeviceFlag::DeviceFlagBattery;

    emit channelStateUpdated(deviceExtId,
                             QStringLiteral("battery"),
                             battery,
                             nowMs);
}

void HueAdapter::handleV2DeviceSoftwareUpdateResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const bool metaChanged = updateDeviceSoftwareUpdateMeta(deviceExtId, resObj, nowMs);
    if (metaChanged && m_v2BootstrapDone) {
        const ChannelList channels = m_v2DeviceChannels.value(deviceExtId);
        if (!channels.isEmpty()) {
            const Device device = m_v2DeviceInfoCache.value(deviceExtId);
            emit deviceUpdated(device, channels);
        }
    }
}

void HueAdapter::handleV2RelativeRotaryResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QJsonObject rrObj = resObj.value(QStringLiteral("relative_rotary")).toObject();
    const QJsonObject lastEvent = rrObj.value(QStringLiteral("last_event")).toObject();
    const QJsonObject rotation = lastEvent.value(QStringLiteral("rotation")).toObject();
    if (!rotation.contains(QStringLiteral("steps")))
        return;

    const QString direction = rotation.value(QStringLiteral("direction")).toString();
    int steps = rotation.value(QStringLiteral("steps")).toInt(0);
    if (steps == 0)
        return;

    if (direction == QStringLiteral("counter_clock_wise")) {
        steps = -steps;
    } else if (direction != QStringLiteral("clock_wise")) {
        return;
    }

    qint64 eventTs = nowMs;
    const QJsonObject reportObj = rrObj.value(QStringLiteral("rotary_report")).toObject();
    const qint64 reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("updated")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    emit channelStateUpdated(deviceExtId,
                             QStringLiteral("dial"),
                             steps,
                             eventTs);

    scheduleDialReset(deviceExtId);
}

static ButtonEventCode mapHueV2ButtonEventToCode(const QString &event)
{
    // Map Hue v2 button.last_event strings to canonical ButtonEventCode
    // values so automations can treat button/remote events uniformly across
    // adapters.
    if (event == QLatin1String("initial_press"))
        return ButtonEventCode::InitialPress;
    if (event == QLatin1String("long_press"))
        return ButtonEventCode::LongPress;
    if (event == QLatin1String("repeat"))
        return ButtonEventCode::Repeat;
    if (event == QLatin1String("short_release"))
        return ButtonEventCode::ShortPressRelease;
    if (event == QLatin1String("long_release"))
        return ButtonEventCode::LongPressRelease;

    // Ignore other events.
    return ButtonEventCode::None;
}

static ButtonEventCode mapHueV1ButtonEventToCode(int value)
{
    // Hue v1 "buttonevent" encodes the button id in the thousands/hundreds
    // place (1000, 2000, ...) and the action in the last digit:
    //
    //  - x000: initial press
    //  - x001: repeat while held
    //  - x002: short release
    //  - x003: long release
    //
    // We ignore the button index here and only normalize the action so that
    // v1 and v2 bridges behave consistently.
    const int action = value % 10;
    switch (action) {
    case 0:
        return ButtonEventCode::InitialPress;
    case 1:
        return ButtonEventCode::Repeat;
    case 2:
        return ButtonEventCode::ShortPressRelease;
    case 3:
        return ButtonEventCode::LongPressRelease;
    default:
        return ButtonEventCode::None;
    }
}

void HueAdapter::handleV2ButtonResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QString buttonResourceId = resObj.value(QStringLiteral("id")).toString();
    const QJsonObject buttonObj = resObj.value(QStringLiteral("button")).toObject();
    QString lastEvent = buttonObj.value(QStringLiteral("last_event")).toString();
    if (lastEvent.isEmpty()) {
        // Live v2 events sometimes only include button_report.event; fall back
        // to that so we don't miss presses emitted via the eventstream.
        const QJsonObject reportObj = buttonObj.value(QStringLiteral("button_report")).toObject();
        lastEvent = reportObj.value(QStringLiteral("event")).toString();
    }
    if (lastEvent.isEmpty())
        return;

    qint64 eventTs = nowMs;
    const QJsonObject reportObj = buttonObj.value(QStringLiteral("button_report")).toObject();
    const qint64 reportTs = parseHueTimestampMs(reportObj.value(QStringLiteral("updated")).toString());
    if (reportTs > 0)
        eventTs = reportTs;

    const ButtonEventCode code = mapHueV2ButtonEventToCode(lastEvent);
    if (code == ButtonEventCode::None)
        return;

    // Map per-button events to either a dedicated "buttonN" channel (for
    // multi-button devices) or fall back to the legacy "button" channel for
    // single-button devices and backwards compatibility.
    QString channelExtId;
    if (!buttonResourceId.isEmpty())
        channelExtId = m_buttonResourceToChannel.value(buttonResourceId);

    const ChannelList channels = m_v2DeviceChannels.value(deviceExtId);

    // Try to route based on metadata.control_id when available.
    const QJsonObject metaObj = resObj.value(QStringLiteral("metadata")).toObject();
    const int controlId = metaObj.value(QStringLiteral("control_id")).toInt(0);
    if (channelExtId.isEmpty() && controlId > 0) {
        const QString candidate = QStringLiteral("button%1").arg(controlId);
        bool hasCandidate = false;
        for (const Channel &ch : channels) {
            if (ch.id == candidate) {
                hasCandidate = true;
                break;
            }
        }
        if (hasCandidate)
            channelExtId = candidate;
    }

    if (channelExtId.isEmpty()) {
        // Some bridges omit control_id for certain events; in that case try
        // to pick a reasonable fallback: a generic "button" channel, or the
        // first "buttonN" channel on the device.
        bool hasGeneric = false;
        QString firstButtonN;
        for (const Channel &ch : channels) {
            if (ch.id == QStringLiteral("button")) {
                hasGeneric = true;
                channelExtId = ch.id;
                break;
            }
            if (firstButtonN.isEmpty() && ch.id.startsWith(QStringLiteral("button"))) {
                firstButtonN = ch.id;
            }
        }
        if (channelExtId.isEmpty() && !hasGeneric && !firstButtonN.isEmpty()) {
            channelExtId = firstButtonN;
        }
    }

    if (channelExtId.isEmpty())
        channelExtId = QStringLiteral("button");

    if (!buttonResourceId.isEmpty())
        m_buttonResourceToChannel.insert(buttonResourceId, channelExtId);

    const QString bindingKey = channelBindingKey(deviceExtId, channelExtId);

    if (code == ButtonEventCode::InitialPress) {
        auto it = m_buttonMultiPress.find(bindingKey);
        if (it != m_buttonMultiPress.end()) {
            const ButtonMultiPressTracker &tracker = it.value();
            if (tracker.count > 0 && tracker.lastTs > 0) {
                const qint64 gap = eventTs - tracker.lastTs;
                if (gap >= kButtonMultiPressResetGapMs) {
                    finalizePendingShortPress(bindingKey);
                }
            }
        }
    }

    if (code == ButtonEventCode::ShortPressRelease) {
        handleShortPressRelease(deviceExtId, channelExtId, eventTs);
    }

    emit channelStateUpdated(deviceExtId,
                             channelExtId,
                             static_cast<int>(code),
                             eventTs);
}

void HueAdapter::handleV2ZigbeeConnectivityResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const bool metaChanged = updateDeviceConnectivityMeta(deviceExtId, resObj, nowMs);
    if (metaChanged && m_v2BootstrapDone) {
        const ChannelList channels = m_v2DeviceChannels.value(deviceExtId);
        if (!channels.isEmpty()) {
            const Device device = m_v2DeviceInfoCache.value(deviceExtId);
            emit deviceUpdated(device, channels);
        }
    }

}

void HueAdapter::handleV2ZigbeeDeviceDiscoveryResource(const QJsonObject &resObj, qint64 nowMs)
{
    const QString deviceExtId = deviceExternalIdFromV2Resource(resObj);
    if (deviceExtId.isEmpty())
        return;

    const QString resourceId = resObj.value(QStringLiteral("id")).toString();
    if (!resourceId.isEmpty()) {
        m_v2ResourceToDevice.insert(resourceBindingKey(QStringLiteral("zigbee_device_discovery"), resourceId),
                                    deviceExtId);
    }
    updateZigbeeDeviceDiscoveryMeta(deviceExtId, resObj, nowMs);
}

// --------------------------------------------------------------------------
// Gamut helpers
// --------------------------------------------------------------------------

namespace {

static QPointF closestPointOnSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const double abx = b.x() - a.x();
    const double aby = b.y() - a.y();
    const double apx = p.x() - a.x();
    const double apy = p.y() - a.y();

    const double abLen2 = abx * abx + aby * aby;
    if (abLen2 <= 1e-12)
        return a;

    double t = (apx * abx + apy * aby) / abLen2;
    t = qBound(0.0, t, 1.0);
    return a + (b - a) * t;
}

static double signedArea(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return 0.5 * ((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()));
}

static bool pointInTriangle(const QPointF &p, const QPointF &a,
                            const QPointF &b, const QPointF &c)
{
    const double area1 = signedArea(p, a, b);
    const double area2 = signedArea(p, b, c);
    const double area3 = signedArea(p, c, a);

    const bool hasNeg = (area1 < 0.0) || (area2 < 0.0) || (area3 < 0.0);
    const bool hasPos = (area1 > 0.0) || (area2 > 0.0) || (area3 > 0.0);
    return !(hasNeg && hasPos);
}

} // namespace

void HueAdapter::updateGamutForLight(const QString &lightId, const QJsonObject &controlObj)
{
    const QJsonArray gamut = controlObj.value(QStringLiteral("colorgamut")).toArray();
    if (gamut.size() < 3)
        return;

    HueGamut g;
    const auto pointFrom = [](const QJsonValue &v) -> QPointF {
        const QJsonArray arr = v.toArray();
        if (arr.size() >= 2) {
            return QPointF(arr.at(0).toDouble(0.0), arr.at(1).toDouble(0.0));
        }
        return QPointF();
    };

    g.p1 = pointFrom(gamut.at(0));
    g.p2 = pointFrom(gamut.at(1));
    g.p3 = pointFrom(gamut.at(2));
    if (!g.isValid())
        return;

    m_gamutByLightId.insert(lightId, g);
}

void HueAdapter::clampColorToGamut(const QString &lightId, double &x, double &y) const
{
    auto it = m_gamutByLightId.constFind(lightId);
    if (it == m_gamutByLightId.constEnd())
        return;

    const HueGamut &g = it.value();
    if (!g.isValid())
        return;

    QPointF p(x, y);
    const QPointF a = g.p1;
    const QPointF b = g.p2;
    const QPointF c = g.p3;

    // If point is already inside the triangle, keep as-is.
    if (pointInTriangle(p, a, b, c))
        return;

    // Otherwise clamp to the closest point on the triangle edges.
    const QPointF pAB = closestPointOnSegment(p, a, b);
    const QPointF pBC = closestPointOnSegment(p, b, c);
    const QPointF pCA = closestPointOnSegment(p, c, a);

    const auto dist2 = [](const QPointF &u, const QPointF &v) {
        const double dx = u.x() - v.x();
        const double dy = u.y() - v.y();
        return dx * dx + dy * dy;
    };

    const double dAB = dist2(p, pAB);
    const double dBC = dist2(p, pBC);
    const double dCA = dist2(p, pCA);

    QPointF closest = pAB;
    double dMin = dAB;
    if (dBC < dMin) {
        dMin = dBC;
        closest = pBC;
    }
    if (dCA < dMin) {
        closest = pCA;
    }

    x = closest.x();
    y = closest.y();
}

void HueAdapter::scheduleDialReset(const QString &deviceExtId)
{
    QTimer *timer = m_dialResetTimers.value(deviceExtId, nullptr);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        m_dialResetTimers.insert(deviceExtId, timer);
        connect(timer, &QTimer::timeout, this, [this, deviceExtId]() {
            if (m_stopping)
                return;
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            emit channelStateUpdated(deviceExtId,
                                     QStringLiteral("dial"),
                                     0,
                                     nowMs);
        });
    }

    timer->stop();
    timer->start(200);
}

void HueAdapter::updateDeviceName(const QString &deviceExtId, const QString &name, CmdId cmdId)
{
    const QString trimmed = name.trimmed();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (deviceExtId.isEmpty() || trimmed.isEmpty()) {
        if (cmdId != 0) {
            CmdResponse resp;
            resp.id     = cmdId;
            resp.status = CmdStatus::InvalidArgument;
            resp.error  = QStringLiteral("Invalid device id or name");
            resp.tsMs   = nowMs;
            emit cmdResult(resp);
        }
        return;
    }

    if (cmdId != 0) {
        const CmdId previousCmd = m_pendingRenameCommands.value(deviceExtId, 0);
        if (previousCmd != 0 && previousCmd != cmdId) {
            finishRenameCommand(deviceExtId,
                                false,
                                tr("Rename request superseded by a newer command"));
        }
        m_pendingRenameCommands.insert(deviceExtId, cmdId);
    }

    cancelRenameVerification(deviceExtId);

    QJsonObject metadata;
    metadata.insert(QStringLiteral("name"), trimmed);
    QJsonObject body;
    body.insert(QStringLiteral("metadata"), metadata);

    qCInfo(adapterLog).noquote()
        << "HueAdapter::updateDeviceName - sending rename for" << deviceExtId
        << "->" << trimmed;
    const bool sent = sendV2ResourceUpdate(QStringLiteral("device"), deviceExtId, body);
    if (sent) {
        scheduleRenameVerification(deviceExtId, 0);
    } else {
        qCWarning(adapterLog).noquote()
            << tr("HueAdapter::updateDeviceName - failed to send rename request for device %1").arg(deviceExtId);
        if (cmdId != 0) {
            finishRenameCommand(deviceExtId,
                                false,
                                QStringLiteral("Hue rename request rejected locally"));
        }
    }
}

QString HueAdapter::lightResourceIdForDevice(const QString &deviceExternalId) const
{
    if (deviceExternalId.isEmpty())
        return {};

    const QString direct = m_deviceToLightResource.value(deviceExternalId);
    if (!direct.isEmpty())
        return direct;

    const QString prefix = deviceExternalId + QLatin1Char('|');
    auto it = m_channelBindings.constBegin();
    for (; it != m_channelBindings.constEnd(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        const HueChannelBinding &binding = it.value();
        if (binding.resourceType == QStringLiteral("light") && !binding.resourceId.isEmpty())
            return binding.resourceId;
    }
    return {};
}

bool HueAdapter::updateDeviceConnectivityMeta(const QString &deviceExternalId,
                                              const QJsonObject &resObj,
                                              qint64 tsMs)
{
    if (deviceExternalId.isEmpty())
        return false;

    auto it = m_v2DeviceInfoCache.find(deviceExternalId);
    if (it == m_v2DeviceInfoCache.end())
        return false;

    Device updated = it.value();
    const QJsonObject previous = updated.meta.value(QStringLiteral("zigbeeConnectivity")).toObject();
    bool metaChanged = false;
    if (previous != resObj) {
        QJsonObject meta = updated.meta;
        meta.insert(QStringLiteral("zigbeeConnectivity"), resObj);
        updated.meta = meta;
        m_v2DeviceInfoCache.insert(deviceExternalId, updated);
        m_v2Devices.insert(deviceExternalId, updated);
        metaChanged = true;
    }

    const QString status = resObj.value(QStringLiteral("status")).toString().trimmed();
    const phicore::ConnectivityStatus statusEnum = connectivityStatusFromString(status);
    auto queueForLater = [this, deviceExternalId, statusValue = statusEnum]() {
        m_pendingConnectivityStatus.insert(deviceExternalId, statusValue);
    };

    if (!status.isEmpty()) {
        qCInfo(adapterLog).noquote()
            << "HueAdapter::updateDeviceConnectivityMeta - status update device" << deviceExternalId
            << "status" << status
            << "raw payload" << QString::fromUtf8(QJsonDocument(resObj).toJson(QJsonDocument::Compact));
    }

    if (status.isEmpty()) {
        return metaChanged;
    }

    if (!m_v2BootstrapDone) {
        queueForLater();
        return metaChanged;
    }

    const ChannelList channels = m_v2DeviceChannels.value(deviceExternalId);
    bool hasChannel = false;
    for (const Channel &ch : channels) {
        if (ch.id == QLatin1String(kZigbeeStatusChannelId)) {
            hasChannel = true;
            break;
        }
    }
    if (!hasChannel) {
        queueForLater();
        return metaChanged;
        }

    emit channelStateUpdated(deviceExternalId,
                             QString::fromLatin1(kZigbeeStatusChannelId),
                             static_cast<int>(statusEnum),
                             tsMs);
    return metaChanged;
}

bool HueAdapter::updateDeviceSoftwareUpdateMeta(const QString &deviceExternalId,
                                                 const QJsonObject &resObj,
                                                 qint64 tsMs)
{
    if (deviceExternalId.isEmpty())
        return false;

    auto it = m_v2DeviceInfoCache.find(deviceExternalId);
    if (it == m_v2DeviceInfoCache.end())
        return false;

    Device updated = it.value();
    const QJsonObject previous = updated.meta.value(QStringLiteral("softwareUpdate")).toObject();
    bool metaChanged = false;
    if (previous != resObj) {
        QJsonObject meta = updated.meta;
        meta.insert(QStringLiteral("softwareUpdate"), resObj);
        updated.meta = meta;
        m_v2DeviceInfoCache.insert(deviceExternalId, updated);
        m_v2Devices.insert(deviceExternalId, updated);
        metaChanged = true;
    }

    phicore::DeviceSoftwareUpdate info = buildDeviceSoftwareUpdate(resObj, tsMs);
    const bool hasInfo = info.status != phicore::DeviceSoftwareUpdateStatus::Unknown
        || !info.currentVersion.isEmpty()
        || !info.targetVersion.isEmpty()
        || !info.message.isEmpty()
        || !info.releaseNotesUrl.isEmpty();
    if (!hasInfo)
        return metaChanged;

    const QJsonObject payload = deviceSoftwareUpdateToJson(info);
    auto queueForLater = [this, deviceExternalId, payload]() {
        m_pendingDeviceSoftwareUpdates.insert(deviceExternalId, payload);
    };

    if (!m_v2BootstrapDone) {
        queueForLater();
        return metaChanged;
    }

    const ChannelList channels = m_v2DeviceChannels.value(deviceExternalId);
    bool hasChannel = false;
    for (const Channel &ch : channels) {
        if (ch.id == QLatin1String(kDeviceSoftwareUpdateChannelId)) {
            hasChannel = true;
            break;
        }
    }
    if (!hasChannel) {
        queueForLater();
        return metaChanged;
    }

    emit channelStateUpdated(deviceExternalId,
                             QString::fromLatin1(kDeviceSoftwareUpdateChannelId),
                             payload,
                             tsMs);
    return metaChanged;
}

bool HueAdapter::updateZigbeeDeviceDiscoveryMeta(const QString &deviceExternalId,
                                                 const QJsonObject &resObj,
                                                 qint64 tsMs)
{
    Q_UNUSED(tsMs);
    if (deviceExternalId.isEmpty())
        return false;

    auto it = m_v2DeviceInfoCache.find(deviceExternalId);
    if (it == m_v2DeviceInfoCache.end())
        return false;

    Device updated = it.value();
    const QJsonObject previous = updated.meta.value(QStringLiteral("zigbeeDeviceDiscovery")).toObject();
    if (previous == resObj)
        return false;

    QJsonObject meta = updated.meta;
    meta.insert(QStringLiteral("zigbeeDeviceDiscovery"), resObj);
    updated.meta = meta;
    m_v2DeviceInfoCache.insert(deviceExternalId, updated);
    m_v2Devices.insert(deviceExternalId, updated);
    const bool ready = m_v2BootstrapDone && !m_v2DeviceChannels.value(deviceExternalId).isEmpty();
    if (ready) {
        emit deviceUpdated(updated, m_v2DeviceChannels.value(deviceExternalId));
    } else {
        m_pendingDiscoveryDeviceUpdates.insert(deviceExternalId);
    }
    return true;
}

QString HueAdapter::zigbeeDeviceDiscoveryOwnerDevice() const
{
    QString fallback;
    for (auto it = m_v2DeviceInfoCache.cbegin(); it != m_v2DeviceInfoCache.cend(); ++it) {
        const Device &device = it.value();
        const QString ref = serviceRefFromMeta(device.meta, QStringLiteral("zigbee_device_discovery"));
        if (ref.isEmpty())
            continue;
        if (device.deviceClass == DeviceClass::Gateway)
            return it.key();
        if (fallback.isEmpty())
            fallback = it.key();
    }

    const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("zigbee_device_discovery"));
    for (const QJsonValue &val : arr) {
        if (!val.isObject())
            continue;
        const QJsonObject ownerObj = val.toObject().value(QStringLiteral("owner")).toObject();
        if (ownerObj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
            continue;
        const QString deviceId = ownerObj.value(QStringLiteral("rid")).toString();
        if (deviceId.isEmpty())
            continue;
        const QString mapped = m_deviceIdToExternalId.value(deviceId);
        if (!mapped.isEmpty())
            return mapped;
    }

    return fallback;
}

QString HueAdapter::zigbeeDeviceDiscoveryResourceId() const
{
    const QString ownerExtId = zigbeeDeviceDiscoveryOwnerDevice();
    if (!ownerExtId.isEmpty()) {
        const Device device = m_v2DeviceInfoCache.value(ownerExtId);
        const QString ref = serviceRefFromMeta(device.meta, QStringLiteral("zigbee_device_discovery"));
        if (!ref.isEmpty())
            return ref;
    }

    const QJsonArray arr = m_v2SnapshotByType.value(QStringLiteral("zigbee_device_discovery"));
    for (const QJsonValue &val : arr) {
        if (!val.isObject())
            continue;
        const QString resourceId = val.toObject().value(QStringLiteral("id")).toString();
        if (!resourceId.isEmpty())
            return resourceId;
    }

    return {};
}

void HueAdapter::invokeDeviceEffect(const QString &deviceExternalId,
                                    DeviceEffect effect,
                                    const QString &effectId,
                                    const QJsonObject &params,
                                    CmdId cmdId)
{
    if (cmdId == 0)
        return;

    CmdResponse resp;
    resp.id = cmdId;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();

    if (deviceExternalId.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("deviceExternalId is required");
        emit cmdResult(resp);
        return;
    }

    const QString lightServiceId = lightResourceIdForDevice(deviceExternalId);
    if (lightServiceId.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("Hue light resource for this device is unknown");
        emit cmdResult(resp);
        return;
    }

    Device device = m_v2DeviceInfoCache.value(deviceExternalId);
    const DeviceEffectDescriptor *descriptor = nullptr;
    for (const DeviceEffectDescriptor &desc : std::as_const(device.effects)) {
        if (!effectId.isEmpty() && desc.id == effectId) {
            descriptor = &desc;
            break;
        }
        if (descriptor == nullptr && desc.effect == effect) {
            descriptor = &desc;
        }
    }

    QString hueEffectName = descriptor
        ? descriptor->meta.value(QStringLiteral("hueEffect")).toString()
        : QString();
    if (hueEffectName.isEmpty() && !effectId.isEmpty())
        hueEffectName = effectId;
    if (hueEffectName.isEmpty())
        hueEffectName = hueEffectNameForDeviceEffect(effect);

    if (hueEffectName.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("Unsupported effect for this device");
        emit cmdResult(resp);
        return;
    }

    QString category = descriptor
        ? descriptor->meta.value(QStringLiteral("hueEffectCategory")).toString()
        : QStringLiteral("effects");
    if (category.isEmpty())
        category = QStringLiteral("effects");

    QJsonObject payload;
    if (category == QStringLiteral("timed_effects")) {
        QJsonObject timed;
        timed.insert(QStringLiteral("effect"), hueEffectName);
        if (params.contains(QStringLiteral("duration")))
            timed.insert(QStringLiteral("duration"), params.value(QStringLiteral("duration")));
        payload.insert(QStringLiteral("timed_effects"), timed);
    } else {
        QJsonObject effectsObj;
        effectsObj.insert(QStringLiteral("effect"), hueEffectName);
        payload.insert(QStringLiteral("effects"), effectsObj);
    }

    if (!sendV2ResourceUpdate(QStringLiteral("light"), lightServiceId, payload)) {
        resp.status = CmdStatus::InternalError;
        resp.error = QStringLiteral("Hue bridge rejected the effect request");
        emit cmdResult(resp);
        return;
    }

    qCInfo(adapterLog).noquote()
        << "HueAdapter::invokeDeviceEffect - device" << deviceExternalId
        << "effect" << hueEffectName
        << "category" << category
        << "payload" << QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    resp.status = CmdStatus::Success;
    emit cmdResult(resp);
}

void HueAdapter::invokeScene(const QString &sceneExternalId,
                             const QString &groupExternalId,
                             const QString &action,
                             CmdId cmdId)
{
    if (cmdId == 0)
        return;

    CmdResponse resp;
    resp.id = cmdId;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();

    if (sceneExternalId.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("sceneExternalId is required");
        emit cmdResult(resp);
        return;
    }

    const Scene sceneInfo = m_v2Scenes.value(sceneExternalId);
    QString targetRid = groupExternalId;
    if (targetRid.isEmpty())
        targetRid = sceneInfo.scopeId;
    if (targetRid.isEmpty())
        targetRid = sceneInfo.meta.value(QStringLiteral("scopeExternalId")).toString();

    QString targetType;
    const QJsonObject groupObj = sceneInfo.meta.value(QStringLiteral("group")).toObject();
    targetType = groupObj.value(QStringLiteral("rtype")).toString();
    if (targetType.isEmpty()) {
        const QString normalized = sceneInfo.scopeType.toLower();
        if (normalized == QStringLiteral("group"))
            targetType = QStringLiteral("zone");
        else if (normalized == QStringLiteral("room"))
            targetType = QStringLiteral("room");
        else if (!normalized.isEmpty())
            targetType = normalized;
    }

    const QString requestedAction = action.trimmed();
    const QString normalizedAction = requestedAction.toLower();
    QString finalAction;
    if (normalizedAction.isEmpty() || normalizedAction == QStringLiteral("activate")) {
        finalAction = QStringLiteral("active");
    } else if (normalizedAction == QStringLiteral("deactivate")) {
        finalAction = QStringLiteral("inactive");
    } else if (normalizedAction == QStringLiteral("dynamic")) {
        finalAction = QStringLiteral("dynamic_palette");
    } else {
        finalAction = requestedAction;
    }

    QJsonObject recall;
    recall.insert(QStringLiteral("action"), finalAction);
    if (!targetRid.isEmpty()) {
        QJsonObject target;
        target.insert(QStringLiteral("rid"), targetRid);
        if (!targetType.isEmpty())
            target.insert(QStringLiteral("rtype"), targetType);
        recall.insert(QStringLiteral("target"), target);
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("recall"), recall);

    qCInfo(adapterLog).noquote()
        << "HueAdapter::invokeScene - scene" << sceneExternalId
        << "target" << (targetRid.isEmpty() ? QStringLiteral("<default>") : targetRid);

    if (!sendV2ResourceUpdate(QStringLiteral("scene"), sceneExternalId, payload)) {
        resp.status = CmdStatus::InternalError;
        resp.error = QStringLiteral("Hue bridge rejected the scene request");
        emit cmdResult(resp);
        return;
    }

    resp.status = CmdStatus::Success;
    emit cmdResult(resp);
}

void HueAdapter::invokeAdapterAction(const QString &actionId,
                                     const QJsonObject &params,
                                     CmdId cmdId)
{
    Q_UNUSED(params);
    if (actionId != QStringLiteral("startDeviceDiscovery")) {
        AdapterInterface::invokeAdapterAction(actionId, params, cmdId);
        return;
    }

    if (cmdId == 0) {
        qCWarning(adapterLog) << "HueAdapter::invokeAdapterAction - missing CmdId for discovery action";
    }

    if (!m_nam) {
        if (cmdId != 0) {
            ActionResponse resp;
            resp.id = cmdId;
            resp.status = CmdStatus::Failure;
            resp.error = QStringLiteral("Network manager unavailable");
            resp.tsMs = QDateTime::currentMSecsSinceEpoch();
            emit actionResult(resp);
        }
        return;
    }

    const QString resourceId = zigbeeDeviceDiscoveryResourceId();
    if (resourceId.isEmpty()) {
        ActionResponse resp;
        if (cmdId != 0)
            resp.id = cmdId;
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Discovery resource not ready");
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit actionResult(resp);
        return;
    }

    const Adapter &info = adapter();
    if (info.token.isEmpty()) {
        ActionResponse resp;
        if (cmdId != 0)
            resp.id = cmdId;
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Hue bridge application key missing");
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit actionResult(resp);
        return;
    }

    m_eventStreamErrorSuppressCount = qMax(m_eventStreamErrorSuppressCount, 3);
    const QPointer<HueAdapter> guardReset(this);
    QTimer::singleShot(5000, this, [guardReset]() {
        if (guardReset)
            guardReset->m_eventStreamErrorSuppressCount = 0;
    });
    const QUrl url = v2ResourceUrl(QStringLiteral("zigbee_device_discovery/%1").arg(resourceId));
    QNetworkRequest req(url);
#if QT_CONFIG(ssl)
    if (info.flags.testFlag(AdapterFlag::AdapterFlagUseTls)) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }
#endif
    req.setRawHeader("hue-application-key", info.token.toUtf8());
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");

    QJsonObject body;
    body.insert(QStringLiteral("state"), QStringLiteral("start"));
    QJsonObject actionObj;
    actionObj.insert(QStringLiteral("type"), QStringLiteral("search"));
    actionObj.insert(QStringLiteral("action_type"), QStringLiteral("search"));
    body.insert(QStringLiteral("action"), actionObj);
    const QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = m_nam->sendCustomRequest(req, QByteArrayLiteral("PUT"), data);
    if (!reply) {
        ActionResponse resp;
        if (cmdId != 0)
            resp.id = cmdId;
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Hue discovery command could not be sent");
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit actionResult(resp);
        return;
    }

    qCInfo(adapterLog) << "HueAdapter::invokeAdapterAction - starting device discovery, resource" << resourceId;
    QPointer<HueAdapter> guardReply(this);
    connect(reply, &QNetworkReply::finished, this, [this, guardReply, reply, cmdId]() {
        if (!guardReply) {
            reply->deleteLater();
            return;
        }
        ActionResponse resp;
        resp.id = cmdId;
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError && statusCode >= 200 && statusCode < 300) {
            resp.status = CmdStatus::Success;
        } else {
            resp.status = CmdStatus::Failure;
            if (reply->error() != QNetworkReply::NoError) {
                resp.error = reply->errorString();
            } else {
                resp.error = tr("Hue bridge returned status %1").arg(statusCode);
            }
        }
        emit actionResult(resp);
        reply->deleteLater();
    });
}

void HueAdapter::scheduleRenameVerification(const QString &deviceExtId, int attempt)
{
    if (deviceExtId.isEmpty() || m_stopping)
        return;

    const int current = m_pendingRenameVerifications.value(deviceExtId, -1);
    if (current >= attempt)
        return;

    if (attempt >= kRenameVerifyMaxAttempts) {
        qCWarning(adapterLog).noquote()
            << "HueAdapter::updateDeviceName - giving up rename verification for" << deviceExtId;
        finishRenameCommand(deviceExtId,
                            false,
                            tr("Hue bridge did not confirm rename"));
        cancelRenameVerification(deviceExtId);
        return;
    }

    m_pendingRenameVerifications.insert(deviceExtId, attempt);
    QTimer *timer = m_renameVerifyTimers.value(deviceExtId, nullptr);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, deviceExtId]() {
            if (m_stopping)
                return;
            const int attempt = m_pendingRenameVerifications.value(deviceExtId, -1);
            if (attempt < 0)
                return;

            qCInfo(adapterLog).noquote()
                << "HueAdapter::updateDeviceName - verifying metadata for" << deviceExtId
                << "(attempt" << (attempt + 1) << ")";
            m_activeRenameFetches.insert(deviceExtId);
            fetchV2DeviceResource(deviceExtId);
        });
        m_renameVerifyTimers.insert(deviceExtId, timer);
    }

    timer->start(kRenameVerifyDelayMs);
}

void HueAdapter::cancelRenameVerification(const QString &deviceExtId)
{
    if (deviceExtId.isEmpty())
        return;

    if (QTimer *timer = m_renameVerifyTimers.take(deviceExtId)) {
        timer->stop();
        timer->deleteLater();
    }
    m_pendingRenameVerifications.remove(deviceExtId);
    m_activeRenameFetches.remove(deviceExtId);
}

void HueAdapter::completeRenameVerification(const QString &deviceExtId)
{
    if (deviceExtId.isEmpty())
        return;
    if (!m_pendingRenameVerifications.contains(deviceExtId))
        return;

    finishRenameCommand(deviceExtId, true);
    cancelRenameVerification(deviceExtId);
    qCInfo(adapterLog).noquote()
        << "HueAdapter::updateDeviceName - verification succeeded for" << deviceExtId;
}

void HueAdapter::finishRenameCommand(const QString &deviceExtId, bool success, const QString &error)
{
    if (deviceExtId.isEmpty())
        return;

    const CmdId cmdId = m_pendingRenameCommands.value(deviceExtId, 0);
    if (cmdId == 0)
        return;

    m_pendingRenameCommands.remove(deviceExtId);

    CmdResponse resp;
    resp.id     = cmdId;
    resp.status = success ? CmdStatus::Success : CmdStatus::Failure;
    if (!success && !error.isEmpty())
        resp.error = error;
    resp.tsMs   = QDateTime::currentMSecsSinceEpoch();
    emit cmdResult(resp);
}


void HueAdapter::handleShortPressRelease(const QString &deviceExtId,
                                         const QString &channelExtId,
                                         qint64 eventTs)
{
    if (deviceExtId.isEmpty() || channelExtId.isEmpty())
        return;
    const QString key = channelBindingKey(deviceExtId, channelExtId);
    ButtonMultiPressTracker &tracker = m_buttonMultiPress[key];
    tracker.deviceExtId = deviceExtId;
    tracker.channelExtId = channelExtId;
    tracker.lastTs = eventTs;
    tracker.count += 1;

    if (!tracker.timer) {
        tracker.timer = new QTimer(this);
        tracker.timer->setSingleShot(true);
        connect(tracker.timer, &QTimer::timeout, this, [this, key]() {
            finalizePendingShortPress(key);
        });
    }

    tracker.timer->start(kButtonMultiPressWindowMs);
}

void HueAdapter::finalizePendingShortPress(const QString &key)
{
    auto it = m_buttonMultiPress.find(key);
    if (it == m_buttonMultiPress.end())
        return;

    ButtonMultiPressTracker &tracker = it.value();
    if (tracker.timer)
        tracker.timer->stop();

    if (tracker.count < 2) {
        tracker.count = 0;
        tracker.lastTs = 0;
        return;
    }

    const int count = tracker.count;
    tracker.count = 0;
    const qint64 ts = tracker.lastTs;
    tracker.lastTs = 0;

    ButtonEventCode aggregated = ButtonEventCode::None;
    switch (count) {
    case 2:
        aggregated = ButtonEventCode::DoublePress;
        break;
    case 3:
        aggregated = ButtonEventCode::TriplePress;
        break;
    case 4:
        aggregated = ButtonEventCode::QuadruplePress;
        break;
    default:
        aggregated = ButtonEventCode::QuintuplePress;
        break;
    }

    if (aggregated != ButtonEventCode::None &&
        !tracker.deviceExtId.isEmpty() &&
        !tracker.channelExtId.isEmpty()) {
        emit channelStateUpdated(tracker.deviceExtId,
                                 tracker.channelExtId,
                                 static_cast<int>(aggregated),
                                 ts);
    }
}

void HueAdapter::handleV2RoomResource(const QJsonObject &resObj)
{
    const QString roomId = resObj.value(QStringLiteral("id")).toString();
    if (roomId.isEmpty())
        return;

    Room room;
    room.externalId = roomId;

    const QJsonObject metaObj = resObj.value(QStringLiteral("metadata")).toObject();
    const QString name = metaObj.value(QStringLiteral("name")).toString();
    room.name = !name.isEmpty() ? name : QStringLiteral("Hue Room");
    room.zone = metaObj.value(QStringLiteral("archetype")).toString();
    room.meta = resObj;

    QStringList memberDevices;
    QSet<QString> seenDevices;
    const auto addMember = [&memberDevices, &seenDevices](const QString &extId) {
        if (extId.isEmpty() || seenDevices.contains(extId))
            return;
        seenDevices.insert(extId);
        memberDevices.append(extId);
    };

    const auto bindDevices = [this, &addMember](const QJsonArray &arr) {
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject obj = val.toObject();
            if (obj.value(QStringLiteral("rtype")).toString() != QStringLiteral("device"))
                continue;
            const QString rid = obj.value(QStringLiteral("rid")).toString();
            if (rid.isEmpty())
                continue;
            const QString extId = m_deviceIdToExternalId.value(rid, rid);
            addMember(extId);
        }
    };

    bindDevices(resObj.value(QStringLiteral("services")).toArray());
    bindDevices(resObj.value(QStringLiteral("children")).toArray());
    room.deviceExternalIds = memberDevices;
    m_v2RoomMemberships.insert(room.externalId, memberDevices);

    emit roomUpdated(room);
}

void HueAdapter::handleV2ZoneResource(const QJsonObject &resObj)
{
    const QString groupId = resObj.value(QStringLiteral("id")).toString();
    if (groupId.isEmpty())
        return;

    Group group;
    group.id = groupId;

    const QJsonObject metaObj = resObj.value(QStringLiteral("metadata")).toObject();
    const QString name = metaObj.value(QStringLiteral("name")).toString();
    group.name = !name.isEmpty() ? name : QStringLiteral("Hue Zone");
    group.zone = metaObj.value(QStringLiteral("archetype")).toString();
    group.meta = resObj;

    QStringList memberDevices;
    QSet<QString> seenDevices;
    const auto addMember = [&memberDevices, &seenDevices](const QString &extId) {
        if (extId.isEmpty() || seenDevices.contains(extId))
            return;
        seenDevices.insert(extId);
        memberDevices.append(extId);
    };

    const auto appendRoomMembers = [this, &addMember, &group](const QString &roomId, const char *section) {
        const QStringList roomMembers = m_v2RoomMemberships.value(roomId);
        if (roomMembers.isEmpty()) {
            qCInfo(adapterLog).noquote()
                << "HueAdapter::handleV2ZoneResource - zone" << group.id
                << "room ref" << roomId << "via" << section << "has no cached members";
            return;
        }
        qCInfo(adapterLog).noquote()
            << "HueAdapter::handleV2ZoneResource - zone" << group.id
            << "room ref" << roomId << "via" << section << "members" << roomMembers;
        for (const QString &roomDevice : roomMembers)
            addMember(roomDevice);
    };

    const auto bindResources = [this, &addMember, &appendRoomMembers, &group](const QJsonArray &arr,
                                                                             const char *section) {
        for (const QJsonValue &val : arr) {
            if (!val.isObject())
                continue;
            const QJsonObject obj = val.toObject();
            const QString type = obj.value(QStringLiteral("rtype")).toString();
            const QString rid = obj.value(QStringLiteral("rid")).toString();
            if (rid.isEmpty())
                continue;
            if (type == QStringLiteral("device")) {
                const QString extId = m_deviceIdToExternalId.value(rid, rid);
                qCInfo(adapterLog).noquote()
                    << "HueAdapter::handleV2ZoneResource - zone" << group.id
                    << section << "device ref" << rid << "maps to" << extId;
                addMember(extId);
            } else if (type == QStringLiteral("room")) {
                appendRoomMembers(rid, section);
            } else {
                const QString resolved = deviceExtIdForResource(type, rid);
                if (!resolved.isEmpty()) {
                    qCInfo(adapterLog).noquote()
                        << "HueAdapter::handleV2ZoneResource - zone" << group.id
                        << section << type << "ref" << rid << "maps to" << resolved;
                    addMember(resolved);
                } else {
                    qCInfo(adapterLog).noquote()
                        << "HueAdapter::handleV2ZoneResource - zone" << group.id
                        << section << type << "ref" << rid << "has no device match";
                }
            }
        }
    };

    bindResources(resObj.value(QStringLiteral("services")).toArray(), "services");
    bindResources(resObj.value(QStringLiteral("children")).toArray(), "children");
    group.deviceExternalIds = memberDevices;

    qCInfo(adapterLog).noquote()
        << "HueAdapter::handleV2ZoneResource - emitting zone" << group.id
        << "members:" << memberDevices.size();
    if (!memberDevices.isEmpty()) {
        qCInfo(adapterLog).noquote()
            << "HueAdapter::handleV2ZoneResource - member devices" << memberDevices;
    }

    emit groupUpdated(group);
}

void HueAdapter::handleV2SceneResource(const QJsonObject &resObj)
{
    const QString sceneId = resObj.value(QStringLiteral("id")).toString();
    if (sceneId.isEmpty()) {
        qCWarning(adapterLog).noquote()
            << "HueAdapter::handleV2SceneResource - ignoring scene without id";
        return;
    }

    Scene scene = m_v2Scenes.value(sceneId);
    scene.id = sceneId;

    const QJsonObject metaObj = resObj.value(QStringLiteral("metadata")).toObject();
    const QString sceneName = firstNonEmptyString(metaObj, {QStringLiteral("name")});
    if (!sceneName.trimmed().isEmpty())
        scene.name = sceneName;

    if (scene.name.trimmed().isEmpty()) {
        qCWarning(adapterLog).noquote()
            << "HueAdapter::handleV2SceneResource - ignoring scene" << scene.id
            << "because metadata name is empty";
        return;
    }

    if (metaObj.contains(QStringLiteral("description")))
        scene.description = metaObj.value(QStringLiteral("description")).toString(scene.description);

    const QJsonObject imageObj = metaObj.value(QStringLiteral("image")).toObject();
    const QString avatarImage = imageObj.value(QStringLiteral("rid")).toString();
    if (!avatarImage.isEmpty())
        scene.image = avatarImage;

    const auto parseSceneState = [](const QString &value) -> SceneState {
        const QString normalized = value.trimmed().toLower();
        if (normalized == QStringLiteral("dynamic"))
            return SceneState::ActiveDynamic;
        if (normalized == QStringLiteral("static") || normalized == QStringLiteral("active"))
            return SceneState::ActiveStatic;
        if (normalized == QStringLiteral("inactive"))
            return SceneState::Inactive;
        return SceneState::Unknown;
    };

    const QJsonObject statusObj = resObj.value(QStringLiteral("status")).toObject();
    if (statusObj.contains(QStringLiteral("active"))) {
        const QString activeValue = statusObj.value(QStringLiteral("active")).toString();
        const SceneState parsed = parseSceneState(activeValue);
        if (parsed != SceneState::Unknown)
            scene.state = parsed;
    }

    const QJsonObject groupObj = resObj.value(QStringLiteral("group")).toObject();
    const QString scopeExternalId = groupObj.value(QStringLiteral("rid")).toString();
    if (!scopeExternalId.isEmpty())
        scene.scopeId = scopeExternalId;
    const QString rtype = groupObj.value(QStringLiteral("rtype")).toString();
    if (!rtype.isEmpty()) {
        if (rtype == QStringLiteral("room"))
            scene.scopeType = QStringLiteral("room");
        else if (rtype == QStringLiteral("zone"))
            scene.scopeType = QStringLiteral("group");
        else
            scene.scopeType = rtype;
    }

    if (scene.state == SceneState::Unknown)
        scene.state = SceneState::Inactive;

    if (!resObj.isEmpty()) {
        QJsonObject merged = scene.meta;
        for (auto it = resObj.constBegin(); it != resObj.constEnd(); ++it)
            merged.insert(it.key(), it.value());
        scene.meta = merged;
        attachServiceRefs(scene.meta);
    }

    const auto hasAction = [&resObj](const QString &action) -> bool {
        if (action.isEmpty())
            return false;
        const QJsonObject statusObj = resObj.value(QStringLiteral("status")).toObject();
        const QJsonValue actionsVal = statusObj.value(QStringLiteral("action_values"));
        if (actionsVal.isArray()) {
            const QJsonArray arr = actionsVal.toArray();
            for (const QJsonValue &val : arr) {
                if (val.toString().compare(action, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        }
        if (actionsVal.isString())
            return actionsVal.toString().compare(action, Qt::CaseInsensitive) == 0;
        return false;
    };

    SceneFlags newFlags = scene.flags;
    newFlags &= ~(SceneFlag::SceneFlagSupportsDynamic | SceneFlag::SceneFlagSupportsDeactivate);
    if (hasAction(QStringLiteral("dynamic_palette")))
        newFlags |= SceneFlag::SceneFlagSupportsDynamic;
    scene.flags = newFlags;

    m_v2Scenes.insert(scene.id, scene);
}

void HueAdapter::handleV2DeleteEvent(const QJsonArray &dataArr)
{
    for (const QJsonValue &resVal : dataArr) {
        if (!resVal.isObject())
            continue;
        const QJsonObject resObj = resVal.toObject();
        const QString type = resObj.value(QStringLiteral("type")).toString();
        if (type != QStringLiteral("device"))
            continue;

        const QString deviceId = resObj.value(QStringLiteral("id")).toString();
        if (deviceId.isEmpty())
            continue;

        QString extId = m_deviceIdToExternalId.take(deviceId);
        if (extId.isEmpty())
            extId = deviceId;

        qCInfo(adapterLog).noquote()
            << "HueAdapter::handleV2DeleteEvent - removing device" << deviceId
            << "->" << extId;

        emit deviceRemoved(extId);
        m_v2Devices.remove(extId);
        m_v2DeviceChannels.remove(extId);
        m_v2DeviceInfoCache.remove(extId);
        m_knownDeviceExternalIds.remove(extId);
        m_deviceToLightResource.remove(extId);
    }
}

void HueAdapter::scheduleV2SnapshotRefresh(const QString &reason)
{
    if (m_stopping)
        return;
    if (m_v2SnapshotPending > 0) {
        qCInfo(adapterLog).noquote()
            << "HueAdapter::scheduleV2SnapshotRefresh - skipping due to pending snapshot, reason"
            << reason;
        return;
    }
    if (m_v2ResyncTimer.isActive()) {
        qCInfo(adapterLog).noquote()
            << "HueAdapter::scheduleV2SnapshotRefresh - refresh already scheduled, ignoring reason"
            << reason;
        return;
    }

    m_pendingV2ResyncReason = reason;
    qCInfo(adapterLog).noquote()
        << "HueAdapter::scheduleV2SnapshotRefresh - scheduling refresh due to" << reason;
    m_v2ResyncTimer.start();
}

void HueAdapter::performScheduledV2SnapshotRefresh()
{
    if (m_stopping)
        return;
    const QString reason = m_pendingV2ResyncReason;
    m_pendingV2ResyncReason.clear();
    qCInfo(adapterLog).noquote()
        << "HueAdapter::scheduleV2SnapshotRefresh - running refresh due to" << reason;
    requestFullSync();
}

void HueAdapter::buildRoomsFromV2Snapshot()
{
    const QJsonArray roomArray = m_v2SnapshotByType.value(QStringLiteral("room"));
    for (const QJsonValue &val : roomArray) {
        if (!val.isObject())
            continue;
        const QJsonObject roomObj = val.toObject();
        handleV2RoomResource(roomObj);
    }
}

void HueAdapter::buildGroupsFromV2Snapshot()
{
    const QJsonArray zoneArray = m_v2SnapshotByType.value(QStringLiteral("zone"));
    qCInfo(adapterLog).noquote()
        << "HueAdapter::buildGroupsFromV2Snapshot - zones count" << zoneArray.size();
    for (const QJsonValue &val : zoneArray) {
        if (!val.isObject())
            continue;
        const QJsonObject zoneObj = val.toObject();
        handleV2ZoneResource(zoneObj);
    }
}


} // namespace phicore
