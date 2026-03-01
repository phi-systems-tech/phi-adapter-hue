#include "hue_schema.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace phicore::hue::ipc {

namespace {

QJsonObject responsive(int xs, int sm, int md, int lg, int xl, int xxl)
{
    QJsonObject out;
    out.insert(QStringLiteral("xs"), xs);
    out.insert(QStringLiteral("sm"), sm);
    out.insert(QStringLiteral("md"), md);
    out.insert(QStringLiteral("lg"), lg);
    out.insert(QStringLiteral("xl"), xl);
    out.insert(QStringLiteral("xxl"), xxl);
    return out;
}

QJsonObject field(const QString &key,
                  const QString &type,
                  const QString &label,
                  const QString &description,
                  const QJsonValue &defaultValue = QJsonValue(),
                  const QJsonArray &flags = {})
{
    QJsonObject out;
    out.insert(QStringLiteral("key"), key);
    out.insert(QStringLiteral("type"), type);
    out.insert(QStringLiteral("label"), label);
    out.insert(QStringLiteral("description"), description);
    if (!defaultValue.isUndefined() && !defaultValue.isNull())
        out.insert(QStringLiteral("default"), defaultValue);
    if (!flags.isEmpty())
        out.insert(QStringLiteral("flags"), flags);
    return out;
}

QJsonArray schemaFields()
{
    QJsonArray fields;

    QJsonArray hostFlags;
    hostFlags.append(QStringLiteral("Required"));
    fields.append(field(QStringLiteral("host"),
                        QStringLiteral("Hostname"),
                        QStringLiteral("Bridge host"),
                        QStringLiteral("IP address or hostname of the Hue bridge."),
                        QJsonValue(QStringLiteral("philips-hue.local")),
                        hostFlags));

    fields.append(field(QStringLiteral("port"),
                        QStringLiteral("Port"),
                        QStringLiteral("Port"),
                        QStringLiteral("TCP port for the Hue API."),
                        QJsonValue(443)));

    fields.append(field(QStringLiteral("useTls"),
                        QStringLiteral("Boolean"),
                        QStringLiteral("Use HTTPS"),
                        QStringLiteral("Use HTTPS when talking to the Hue API."),
                        QJsonValue(true)));

    QJsonArray appKeyFlags;
    appKeyFlags.append(QStringLiteral("Secret"));
    fields.append(field(QStringLiteral("appKey"),
                        QStringLiteral("Password"),
                        QStringLiteral("Application key"),
                        QStringLiteral("Hue API application key."),
                        QJsonValue(),
                        appKeyFlags));

    fields.append(field(QStringLiteral("pollIntervalMs"),
                        QStringLiteral("Integer"),
                        QStringLiteral("Poll interval"),
                        QStringLiteral("Refresh interval while connected."),
                        QJsonValue(5000)));

    fields.append(field(QStringLiteral("retryIntervalMs"),
                        QStringLiteral("Integer"),
                        QStringLiteral("Retry interval"),
                        QStringLiteral("Reconnect interval while bridge is unavailable."),
                        QJsonValue(10000)));

    return fields;
}

QJsonObject section(const QString &title, const QString &description, const QJsonArray &fields)
{
    QJsonObject layout;
    layout.insert(QStringLiteral("gridUnits"), 24);
    QJsonArray gutter;
    gutter.append(12);
    gutter.append(8);
    layout.insert(QStringLiteral("gutter"), gutter);

    QJsonObject defaults;
    defaults.insert(QStringLiteral("span"), responsive(24, 24, 12, 12, 12, 12));
    defaults.insert(QStringLiteral("labelPosition"), QStringLiteral("Left"));
    defaults.insert(QStringLiteral("labelSpan"), 8);
    defaults.insert(QStringLiteral("controlSpan"), 16);
    defaults.insert(QStringLiteral("actionPosition"), QStringLiteral("Inline"));
    defaults.insert(QStringLiteral("actionSpan"), 6);
    layout.insert(QStringLiteral("defaults"), defaults);

    QJsonObject out;
    out.insert(QStringLiteral("title"), title);
    out.insert(QStringLiteral("description"), description);
    out.insert(QStringLiteral("layout"), layout);
    out.insert(QStringLiteral("fields"), fields);
    return out;
}

} // namespace

phicore::adapter::v1::Utf8String displayName()
{
    return "Philips Hue";
}

phicore::adapter::v1::Utf8String description()
{
    return "Provides devices for Philips Hue bridge";
}

phicore::adapter::v1::Utf8String iconSvg()
{
    return
        "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"Hue text logotype\">"
        "<defs>"
        "<linearGradient id=\"hueTextGradient\" x1=\"0\" y1=\"12\" x2=\"24\" y2=\"12\" gradientUnits=\"userSpaceOnUse\">"
        "<stop offset=\"0%\" stop-color=\"#FF5F6D\"/>"
        "<stop offset=\"25%\" stop-color=\"#FFC371\"/>"
        "<stop offset=\"50%\" stop-color=\"#47E9A0\"/>"
        "<stop offset=\"75%\" stop-color=\"#40C2FF\"/>"
        "<stop offset=\"100%\" stop-color=\"#A659FF\"/>"
        "</linearGradient>"
        "</defs>"
        "<text x=\"12\" y=\"16\" text-anchor=\"middle\" font-family=\"'Geist','Inter','Arial',sans-serif\" font-weight=\"600\" font-size=\"11\" fill=\"url(#hueTextGradient)\">hue</text>"
        "</svg>";
}

phicore::adapter::v1::AdapterCapabilities capabilities()
{
    namespace v1 = phicore::adapter::v1;

    v1::AdapterCapabilities caps;
    caps.required = v1::AdapterRequirement::Host
        | v1::AdapterRequirement::ManualConfirm
        | v1::AdapterRequirement::UsesRetryInterval;
    caps.optional = v1::AdapterRequirement::SupportsTls
        | v1::AdapterRequirement::AppKey
        | v1::AdapterRequirement::Port;
    caps.flags = v1::AdapterFlag::SupportsProbe
        | v1::AdapterFlag::SupportsRename
        | v1::AdapterFlag::RequiresPolling;

    v1::AdapterActionDescriptor probe;
    probe.id = "probe";
    probe.label = "Test connection";
    probe.description = "Reachability and credentials check";
    probe.metaJson = R"({"placement":"card","kind":"command","requiresAck":true,"resultField":"appKey"})";
    caps.factoryActions.push_back(probe);

    v1::AdapterActionDescriptor discovery;
    discovery.id = "startDeviceDiscovery";
    discovery.label = "Search for Hue devices";
    discovery.description = "Trigger the bridge to enter Zigbee discovery mode.";
    discovery.metaJson = R"({"placement":"card","kind":"command","requiresAck":true})";
    caps.instanceActions.push_back(discovery);

    caps.defaultsJson = R"({"host":"philips-hue.local","port":443,"useTls":true,"pollIntervalMs":5000,"retryIntervalMs":10000})";
    return caps;
}

phicore::adapter::v1::JsonText configSchemaJson()
{
    const QJsonArray fields = schemaFields();

    QJsonObject schema;
    schema.insert(QStringLiteral("factory"),
                  section(QStringLiteral("Philips Hue Bridge"),
                          QStringLiteral("Configure connection to a Philips Hue bridge."),
                          fields));
    schema.insert(QStringLiteral("instance"),
                  section(QStringLiteral("Philips Hue Bridge"),
                          QStringLiteral("Configure connection to a Philips Hue bridge."),
                          fields));

    return QJsonDocument(schema).toJson(QJsonDocument::Compact).toStdString();
}

} // namespace phicore::hue::ipc
