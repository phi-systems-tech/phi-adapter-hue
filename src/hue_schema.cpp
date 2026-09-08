#include "hue_schema.h"

#include <string>

#include "hue_json.h"

namespace phicore::hue::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

Json field(const std::string &key, const std::string &type, const std::string &label,
           const std::string &description, const Json &defaultValue = Json(),
           const Json &flags = Json::array())
{
    Json out = Json::object();
    out["key"] = key;
    out["type"] = type;
    out["label"] = label;
    out["description"] = description;
    if (!defaultValue.is_null())
        out["default"] = defaultValue;
    if (flags.is_array() && !flags.empty())
        out["flags"] = flags;
    return out;
}

Json responsive(int xs, int sm, int md, int lg, int xl, int xxl)
{
    return Json{{"xs", xs}, {"sm", sm}, {"md", md}, {"lg", lg}, {"xl", xl}, {"xxl", xxl}};
}

Json schemaFields()
{
    Json fields = Json::array();
    fields.push_back(field("host", "Hostname", "Bridge host",
                           "IP address or hostname of the Hue bridge.", "philips-hue.local",
                           Json::array({"Required"})));
    fields.push_back(field("port", "Port", "Port", "TCP port for the Hue API.", 443));
    fields.push_back(field("useTls", "Boolean", "Use HTTPS",
                           "Use HTTPS when talking to the Hue API. The bridge's certificate is"
                           " verified against Signify's root and has to name the bridge id.",
                           true));
    fields.push_back(field("appKey", "Password", "Application key", "Hue API application key.",
                           Json(), Json::array({"Secret"})));
    fields.push_back(field("pollIntervalMs", "Integer", "Poll interval",
                           "Refresh interval while connected.", 5000));
    fields.push_back(field("retryIntervalMs", "Integer", "Retry interval",
                           "Reconnect interval while bridge is unavailable.", 10000));
    return fields;
}

Json section(const std::string &title, const std::string &description, const Json &fields)
{
    Json defaults = Json::object();
    defaults["span"] = responsive(24, 24, 12, 12, 12, 12);
    defaults["labelPosition"] = "top";
    defaults["labelSpan"] = 8;
    defaults["controlSpan"] = 16;
    defaults["actionPosition"] = "inline";
    defaults["actionSpan"] = 6;
    Json layout = Json::object();
    layout["gridUnits"] = 24;
    layout["gutter"] = Json::array({12, 8});
    layout["defaults"] = defaults;
    Json out = Json::object();
    out["title"] = title;
    out["description"] = description;
    out["layout"] = layout;
    out["fields"] = fields;
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

v1::JsonText configSchemaJson()
{
    const Json fields = schemaFields();
    Json schema = Json::object();
    schema["factory"] = section("Philips Hue Bridge", "Configure connection to a Philips Hue bridge.", fields);
    schema["instance"] = section("Philips Hue Bridge", "Configure connection to a Philips Hue bridge.", fields);
    return dump(schema);
}

} // namespace phicore::hue::ipc
