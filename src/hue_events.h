#pragma once

// The bridge's event stream, taken apart.
//
// `/eventstream/clip/v2` is a server-sent event stream: lines of `data: ...`,
// an empty line ending each event, each event a JSON array of changes. The
// parser is fed whatever bytes arrive and hands back whole events; what an
// event means to a device is decided in hue_model. Pure.

#include <string>
#include <string_view>
#include <vector>

#include "hue_json.h"

namespace phicore::hue::ipc {

class EventStreamParser
{
public:
    /// Appends bytes; returns every event completed by them, as parsed JSON
    /// (an array of change objects, or whatever the bridge sent).
    std::vector<Json> feed(std::string_view bytes);

    void reset();

private:
    std::string m_line;
    std::string m_data;
};

/// What one change in an event says about a resource.
struct EventChange {
    /// "update", "add", "delete", "error".
    std::string type;
    /// The resource's own type: "button", "light", "zigbee_connectivity", ...
    std::string resourceType;
    /// The resource object as sent.
    Json resource;
};

/// The changes an event (or a whole batch of them) carries, in order.
std::vector<EventChange> changesIn(const Json &event);

} // namespace phicore::hue::ipc
