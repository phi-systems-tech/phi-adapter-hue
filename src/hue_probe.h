#pragma once

// "Test connection", at factory scope: with an application key, whether the
// bridge accepts it; without one, pairing - which needs the link button on
// the bridge pressed first, and says so.

#include <functional>
#include <string>

#include "phi/adapter/net/http_client.h"
#include "phi/adapter/v1/types.h"

#include "hue_json.h"
#include "hue_settings.h"

namespace phicore::hue::ipc {

struct ProbeOutcome {
    bool ok = false;
    std::string error;
    std::string message;
    std::string appKey;
    /// Values the form takes over: the clientKey pairing produced.
    phicore::adapter::v1::AdapterFormValues formValues;
};

/// One request on `http`; `done` runs on the loop when it answers.
void runProbe(phicore::adapter::net::HttpClient &http, const ConnectionSettings &settings,
              std::function<void(ProbeOutcome)> done);

} // namespace phicore::hue::ipc
