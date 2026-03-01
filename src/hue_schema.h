#pragma once

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::hue::ipc {

inline constexpr const char kPluginType[] = "hue";

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();

phicore::adapter::v1::AdapterCapabilities capabilities();
phicore::adapter::v1::JsonText configSchemaJson();

} // namespace phicore::hue::ipc
