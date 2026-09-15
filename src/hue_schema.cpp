#include "hue_schema.h"

#include <string>
#include <utility>

namespace phicore::hue::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

v1::AdapterConfigField field(const char *key, v1::AdapterConfigFieldType type, const char *label,
                             const char *description, v1::ScalarValue defaultValue = {})
{
    v1::AdapterConfigField out;
    out.key = key;
    out.type = type;
    out.label = label;
    out.description = description;
    out.defaultValue = std::move(defaultValue);
    return out;
}

/// Two columns: the address beside its port, TLS beside the key, the two
/// intervals short.
v1::AdapterConfigSection section()
{
    using Type = v1::AdapterConfigFieldType;
    v1::AdapterConfigSection out;
    out.title = "Philips Hue Bridge";
    out.description = "Configure connection to a Philips Hue bridge.";
    out.layout.columns = 2;

    v1::AdapterConfigField host = field("host", Type::Hostname, "Bridge host", "IP address or hostname of the Hue bridge.",
                                        v1::Utf8String("philips-hue.local"));
    host.flags = v1::AdapterConfigFieldFlag::Required;
    v1::AdapterConfigField port = field("port", Type::Port, "Port", "TCP port for the Hue API.", std::int64_t{443});
    port.layout.controlWidth = v1::AdapterConfigSize::Narrow;
    v1::AdapterConfigField useTls = field("useTls", Type::Boolean, "Use HTTPS",
                                          "Use HTTPS when talking to the Hue API. The bridge's certificate is"
                                          " verified against Signify's root and has to name the bridge id.",
                                          true);
    v1::AdapterConfigField appKey = field("appKey", Type::Password, "Application key", "Hue API application key.");
    appKey.flags = v1::AdapterConfigFieldFlag::Secret;
    v1::AdapterConfigField poll =
        field("pollIntervalMs", Type::Integer, "Poll interval", "Refresh interval while connected.", std::int64_t{5000});
    poll.layout.controlWidth = v1::AdapterConfigSize::Narrow;
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval",
                                         "Reconnect interval while bridge is unavailable.", std::int64_t{10000});
    retry.layout.controlWidth = v1::AdapterConfigSize::Narrow;

    out.fields = {host, port, useTls, appKey, poll, retry};
    return out;
}

} // namespace

v1::Utf8String displayName()
{
    return "Philips Hue";
}

v1::Utf8String description()
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
        | v1::AdapterFlag::SupportsDiscovery
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

v1::AdapterConfigSchema configSchema()
{
    v1::AdapterConfigSchema schema;
    schema.factory = section();
    schema.instance = section();
    return schema;
}

} // namespace phicore::hue::ipc
