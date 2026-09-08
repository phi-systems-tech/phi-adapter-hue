#include "hue_probe.h"

#include <unistd.h>

#include "phi/runtime/str.h"

namespace phicore::hue::ipc {

namespace str = phi::str;
namespace net = phicore::adapter::net;

namespace {

std::string localHostName()
{
    char buffer[64] = {};
    if (::gethostname(buffer, sizeof(buffer) - 1) != 0 || buffer[0] == '\0')
        return "adapter";
    return std::string(buffer).substr(0, 20);
}

bool parseCreateUser(const std::string &payload, std::string *appKey, std::string *clientKey,
                     std::string *error)
{
    const Json doc = parseJson(payload);
    if (!doc.is_array()) {
        *error = "Unexpected response from Hue bridge";
        return false;
    }
    for (const Json &entry : doc) {
        const Json failure = jsonValue(entry, "error");
        if (failure.is_object()) {
            *error = bridgeErrorText(payload);
            if (error->empty())
                *error = "Hue bridge rejected the request";
            return false;
        }
        const Json success = jsonValue(entry, "success");
        if (!success.is_object())
            continue;
        *appKey = jsonString(success, "username");
        *clientKey = jsonString(success, "clientkey");
        return !appKey->empty();
    }
    *error = "Hue bridge returned no success entry";
    return false;
}

} // namespace

void runProbe(net::HttpClient &http, const ConnectionSettings &settings,
              std::function<void(ProbeOutcome)> done)
{
    if (settings.address().empty()) {
        done({false, "Host must not be empty", {}, {}, Json::object()});
        return;
    }
    // A probe does not know the bridge id yet, so it cannot expect a name.
    ConnectionSettings probeSettings = settings;
    probeSettings.serverName.clear();

    if (!settings.appKey.empty()) {
        const bool issued = http.send(
            callFor(probeSettings, "GET", "/clip/v2/resource/bridge", {}, true),
            [done, appKey = settings.appKey](net::HttpClient::Result result) {
                ProbeOutcome out;
                if (result.ok) {
                    out.ok = true;
                    out.message = "Bridge reachable and credentials valid";
                    out.appKey = appKey;
                } else {
                    out.error = bridgeErrorText(result.body);
                    if (out.error.empty())
                        out.error = result.error.empty()
                            ? "Hue bridge rejected the application key"
                            : result.error;
                }
                done(std::move(out));
            });
        if (!issued)
            done({false, "Another probe is already running", {}, {}, Json::object()});
        return;
    }

    const std::string body = dump(Json{{"devicetype", "phi-core#" + localHostName()},
                                       {"generateclientkey", true}});
    const bool issued = http.send(callFor(probeSettings, "POST", "/api", body, false),
                                  [done](net::HttpClient::Result result) {
                                      ProbeOutcome out;
                                      if (!result.ok) {
                                          out.error = bridgeErrorText(result.body);
                                          if (out.error.empty())
                                              out.error = result.error.empty()
                                                  ? "Failed to create Hue application key"
                                                  : result.error;
                                          done(std::move(out));
                                          return;
                                      }
                                      std::string appKey;
                                      std::string clientKey;
                                      std::string error;
                                      if (!parseCreateUser(result.body, &appKey, &clientKey, &error)) {
                                          out.error = error;
                                          done(std::move(out));
                                          return;
                                      }
                                      out.ok = true;
                                      out.appKey = appKey;
                                      out.message = "Pairing successful";
                                      if (!clientKey.empty())
                                          out.formValues["clientKey"] = clientKey;
                                      done(std::move(out));
                                  });
    if (!issued)
        done({false, "Another probe is already running", {}, {}, Json::object()});
}

} // namespace phicore::hue::ipc
