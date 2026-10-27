#include "hue_schema.h"

#include <string>
#include <utility>

namespace phicore::hue::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

v1::AdapterConfigField field(const char *key, v1::AdapterConfigFieldType type, const char *label,
                             const char *description, v1::ScalarValue defaultValue = {})
{
    v1::AdapterConfigField out(key, type, label);
    out.setDescription(description).setDefaultValue(std::move(defaultValue));
    return out;
}

/// Two columns: the address beside its port, TLS beside the key, the two
/// intervals short.
v1::AdapterConfigSection section()
{
    using Type = v1::AdapterConfigFieldType;
    v1::AdapterConfigSection out;
    out.setTitle("Philips Hue Bridge").setDescription("Configure connection to a Philips Hue bridge.");
    out.layout().setColumns(2);

    v1::AdapterConfigField host = field("host", Type::Hostname, "Bridge host", "IP address or hostname of the Hue bridge.",
                                        v1::Utf8String("philips-hue.local"));
    host.setFlags(v1::AdapterConfigFieldFlag::Required);
    v1::AdapterConfigField port = field("port", Type::Port, "Port", "TCP port for the Hue API.", std::int64_t{443});
    port.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField useTls = field("useTls", Type::Boolean, "Use HTTPS",
                                          "Use HTTPS when talking to the Hue API. The bridge's certificate is"
                                          " verified against Signify's root and has to name the bridge id.",
                                          true);
    v1::AdapterConfigField appKey = field("appKey", Type::Password, "Application key", "Hue API application key.");
    appKey.setFlags(v1::AdapterConfigFieldFlag::Secret);
    v1::AdapterConfigField poll =
        field("pollIntervalMs", Type::Integer, "Poll interval", "Refresh interval while connected.", std::int64_t{5000});
    poll.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval",
                                         "Reconnect interval while bridge is unavailable.", std::int64_t{10000});
    retry.layout().setControlWidth(v1::AdapterConfigSize::Narrow);

    out.setFields({host, port, useTls, appKey, poll, retry});
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
    caps.setRequired(v1::AdapterRequirement::Host
        | v1::AdapterRequirement::ManualConfirm
        | v1::AdapterRequirement::UsesRetryInterval);
    caps.setOptional(v1::AdapterRequirement::SupportsTls
        | v1::AdapterRequirement::AppKey
        | v1::AdapterRequirement::Port);
    // Hue names a device, a room, a zone and a scene through one and the same
    // `metadata.name` on the resource, so all four can be carried.
    caps.setFlags(v1::AdapterCapability::SupportsProbe
        | v1::AdapterCapability::SupportsDiscovery
        | v1::AdapterCapability::SupportsDeviceRename
        | v1::AdapterCapability::SupportsRoomRename
        | v1::AdapterCapability::SupportsGroupRename
        | v1::AdapterCapability::SupportsSceneRename
        | v1::AdapterCapability::RequiresPolling);

    v1::AdapterActionDescriptor probe("probe", "Test connection", "Reachability and credentials check");
    // Pairing answers with the application key, which goes into its field.
    probe.setResultField("appKey");
    caps.addFactoryAction(probe);

    caps.setTopologies(v1::Topology::Local);
    // "Add" makes the bridge search; new devices join by themselves.
    caps.setPairingStyles(v1::PairingStyle::JoinWindow);

    return caps;
}

v1::AdapterConfigSchema configSchema()
{
    v1::AdapterConfigSchema schema;
    schema.setFactory(section()).setInstance(section());
    return schema;
}

} // namespace phicore::hue::ipc
