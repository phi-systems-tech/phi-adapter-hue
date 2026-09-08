#include "hue_settings.h"

#include <chrono>

#include "phi/adapter/v1/tlsconfig.h"
#include "phi/runtime/str.h"

#ifndef HUE_BRIDGE_CA_FILE
#define HUE_BRIDGE_CA_FILE "/usr/share/phi/adapters/hue/huebridge_cacert.pem"
#endif

namespace phicore::hue::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace net = phicore::adapter::net;

using namespace std::chrono_literals;

std::string ConnectionSettings::address() const
{
    const std::string trimmedIp = str::trimmed(ip);
    if (!trimmedIp.empty())
        return trimmedIp;
    return str::trimmed(host);
}

int ConnectionSettings::effectivePort() const
{
    if (port > 0)
        return port;
    return useTls ? 443 : 80;
}

std::string ConnectionSettings::baseUrl() const
{
    const std::string where = address();
    if (where.empty())
        return {};
    return std::string(useTls ? "https://" : "http://") + where + ":" + str::number(effectivePort());
}

bool ConnectionSettings::sameBridge(const ConnectionSettings &other) const
{
    return baseUrl() == other.baseUrl() && appKey == other.appKey && caFile == other.caFile
        && serverName == other.serverName;
}

const char *bundledCaFile()
{
    return HUE_BRIDGE_CA_FILE;
}

ConnectionSettings settingsFromAdapter(const v1::Adapter &adapter, const Json &meta)
{
    ConnectionSettings settings;
    settings.host = str::trimmed(adapter.host);
    settings.ip = str::trimmed(adapter.ip);
    settings.port = static_cast<int>(adapter.port);
    settings.appKey = str::trimmed(adapter.token);
    settings.serverName = str::trimmed(adapter.externalId);

    if (meta.contains("host"))
        settings.host = jsonString(meta, "host");
    if (meta.contains("ip"))
        settings.ip = jsonString(meta, "ip");
    if (meta.contains("port"))
        settings.port = jsonInt(meta, "port", settings.port);
    if (meta.contains("appKey"))
        settings.appKey = jsonString(meta, "appKey");

    if (meta.contains("useTls")) {
        settings.useTls = v1::tlsTruthOf(
            meta.at("useTls").is_boolean() ? v1::ScalarValue(meta.at("useTls").get<bool>())
                                           : v1::ScalarValue(jsonScalarText(meta.at("useTls"))),
            true);
    } else if (v1::hasFlag(adapter.flags, v1::AdapterFlag::UseTls)) {
        settings.useTls = true;
    } else if (settings.port > 0) {
        settings.useTls = settings.port == 443;
    } else {
        settings.useTls = true;
    }
    if (settings.port <= 0)
        settings.port = settings.useTls ? 443 : 80;

    const std::string caFile = jsonString(meta, v1::kTlsCaFileFieldKey);
    settings.caFile = caFile.empty() ? bundledCaFile() : caFile;
    return settings;
}

void applyProbeParams(const Json &params, ConnectionSettings &settings)
{
    if (!params.is_object())
        return;
    if (params.contains("host"))
        settings.host = jsonString(params, "host");
    if (params.contains("ip"))
        settings.ip = jsonString(params, "ip");
    if (params.contains("port"))
        settings.port = jsonInt(params, "port", settings.port);
    if (params.contains("useTls")) {
        settings.useTls = v1::tlsTruthOf(
            params.at("useTls").is_boolean() ? v1::ScalarValue(params.at("useTls").get<bool>())
                                             : v1::ScalarValue(jsonScalarText(params.at("useTls"))),
            settings.useTls);
    }
    if (params.contains("appKey"))
        settings.appKey = jsonString(params, "appKey");
    if (settings.port <= 0)
        settings.port = settings.useTls ? 443 : 80;
}

net::HttpClient::Call callFor(const ConnectionSettings &settings, std::string_view method,
                              std::string_view path, std::string body, bool withAppKey)
{
    net::HttpClient::Call call;
    std::string target(path);
    if (target.empty() || target.front() != '/')
        target.insert(target.begin(), '/');
    call.url = settings.baseUrl() + target;
    call.method = std::string(method);
    call.body = std::move(body);
    call.headers.emplace_back("Accept", "application/json");
    call.headers.emplace_back("User-Agent", "phi-adapter-hue-ipc/2.0");
    if (!call.body.empty())
        call.headers.emplace_back("Content-Type", "application/json");
    if (withAppKey && !settings.appKey.empty())
        call.headers.emplace_back("hue-application-key", settings.appKey);
    call.timeout = 10000ms;
    call.tls.enabled = settings.useTls;
    call.tls.caFile = settings.caFile;
    // The certificate names the bridge id, never the address it was dialled
    // by. With the id known the check is against it; a probe that does not
    // know it yet verifies the chain alone - Signify's root, nobody else's.
    call.tls.verifyHostname = !settings.serverName.empty();
    call.tlsServerName = settings.serverName;
    return call;
}

std::string bridgeErrorText(const std::string &payload)
{
    const Json doc = parseJson(payload);
    // CLIP v2: {"errors":[{"description":...}]}. v1 (/api): [{"error":{...}}].
    const Json errors = jsonValue(doc, "errors");
    if (errors.is_array()) {
        for (const Json &error : errors) {
            const std::string description = jsonString(error, "description", false);
            if (!description.empty())
                return description;
        }
    }
    if (doc.is_array()) {
        for (const Json &entry : doc) {
            const Json error = jsonValue(entry, "error");
            if (!error.is_object())
                continue;
            if (jsonInt(error, "type", 0) == 101)
                return "Press the link button on the Hue bridge, then retry.";
            const std::string description = jsonString(error, "description", false);
            if (!description.empty())
                return description;
        }
    }
    return {};
}

} // namespace phicore::hue::ipc
