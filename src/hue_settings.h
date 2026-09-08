#pragma once

// Where the bridge is and how it is spoken to: the adapter's configuration
// turned into calls.

#include <cstdint>
#include <string>
#include <string_view>

#include "phi/adapter/net/http_client.h"
#include "phi/adapter/v1/schema.h"

#include "hue_json.h"

namespace phicore::hue::ipc {

struct ConnectionSettings {
    std::string host;
    std::string ip;
    int port = 0;
    bool useTls = true;
    std::string appKey;
    /// Signify's root, bundled with the package unless the meta names another.
    std::string caFile;
    /// The bridge id, which is what the bridge's certificate names. Empty for
    /// a probe that does not know it yet.
    std::string serverName;

    /// The address dialled: the IP when there is one, else the host.
    [[nodiscard]] std::string address() const;
    [[nodiscard]] int effectivePort() const;
    [[nodiscard]] std::string baseUrl() const;
    [[nodiscard]] bool sameBridge(const ConnectionSettings &other) const;
};

/// The bundled CA's path, as the package installs it.
const char *bundledCaFile();

/// From the adapter record and its meta. Meta wins over the record for host,
/// ip, port, appKey and useTls; the bridge id is the adapter's external id.
ConnectionSettings settingsFromAdapter(const phicore::adapter::v1::Adapter &adapter,
                                       const Json &meta);

/// The probe's form overrides: host, ip, port, useTls, appKey.
void applyProbeParams(const Json &params, ConnectionSettings &settings);

/// A call to `path`, with the app key when `withAppKey`, TLS as configured.
phicore::adapter::net::HttpClient::Call callFor(const ConnectionSettings &settings,
                                                std::string_view method, std::string_view path,
                                                std::string body, bool withAppKey);

/// The `description` of the first error in a bridge error payload, or empty.
std::string bridgeErrorText(const std::string &payload);

} // namespace phicore::hue::ipc
