#pragma once

// One Hue bridge: the poll, the event stream, the devices behind it.
// Everything that talks to phi-core is in here; everything that can be
// decided without a bridge is in hue_model, hue_events and hue_settings.

#include <memory>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::hue::ipc {

std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance();

} // namespace phicore::hue::ipc
