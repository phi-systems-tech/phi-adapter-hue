#pragma once

// A Hue bridge's answers, and what they become.
//
// Everything here is pure conversion: a bridge payload in, a v1 type out, or
// a channel write in and the body the bridge expects out. None of it needs a
// bridge, which is why it is tested without one.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/color.h"
#include "phi/adapter/v1/device.h"
#include "phi/adapter/v1/group.h"
#include "phi/adapter/v1/room.h"
#include "phi/adapter/v1/scene.h"
#include "phi/adapter/v1/types.h"
#include "phi/adapter/v1/value.h"

#include "hue_json.h"

namespace phicore::hue::ipc {

/// The light service behind a device, as far as commands need it.
struct LightState {
    std::string resourceId;
    std::optional<bool> on;
    std::optional<double> brightness;
    std::optional<int> mired;
    std::optional<std::pair<double, double>> xy;
};

struct DeviceEntry {
    phicore::adapter::v1::Device device;
    phicore::adapter::v1::ChannelList channels;
    LightState light;
    /// Which bridge resource type each channel came from, so a poll that
    /// could not fetch that type keeps the channel rather than losing it.
    std::map<std::string, std::string> channelSource;

    [[nodiscard]] const phicore::adapter::v1::Channel *channel(std::string_view id) const;
    /// A digest of everything but values: the device's identity and its
    /// channel definitions. Two entries with the same key need no
    /// re-announcement.
    [[nodiscard]] std::string definitionKey() const;
};

struct Snapshot {
    std::map<std::string, DeviceEntry> devices;
    phicore::adapter::v1::RoomList rooms;
    phicore::adapter::v1::GroupList groups;
    phicore::adapter::v1::SceneList scenes;
    std::string discoveryResourceId;
};

/// The resource types a poll asks for, in the order they are asked.
const std::vector<std::string> &pollResourceTypes();
/// Without these a poll is no poll.
bool resourceTypeRequired(std::string_view type);

/// What a poll fetched: an array per resource type, absent when the fetch
/// failed.
struct Resources {
    std::map<std::string, Json> byType;

    [[nodiscard]] bool has(std::string_view type) const;
    [[nodiscard]] const Json &array(std::string_view type) const;
    [[nodiscard]] std::vector<std::string> missing() const;
};

Snapshot buildSnapshot(const Resources &resources);

/**
 * @brief Carries channels over from the previous snapshot for the resource
 * types this poll could not fetch.
 *
 * The one rule behind the button flicker: a snapshot that did not ask about
 * buttons says nothing about buttons. It used to say there were none, and
 * core dutifully removed them - and recreated them a minute later with new
 * ids, which is what the app showed as a button vanishing and coming back.
 */
void carryOver(const Snapshot &previous, const std::vector<std::string> &missingTypes,
               Snapshot &next);

/// A device's colour as the contract carries it, from its xy and brightness.
std::optional<phicore::adapter::v1::Color> colorOf(const LightState &light);

/// One channel value a resource carries.
struct ChannelReport {
    std::string channelId;
    phicore::adapter::v1::ScalarValue value;
};

/// The channel values in a resource of `resourceType`, as an event or a poll
/// delivers it: a light's on/brightness/mired, a sensor's reading, a battery
/// level, a connectivity status. Colour is asked for separately.
std::vector<ChannelReport> reportsFor(std::string_view resourceType, const Json &resource);

/// The xy a light resource carries, when it does.
std::optional<std::pair<double, double>> xyOf(const Json &lightResource);
/// The brightness a light resource carries, when it does.
std::optional<double> brightnessOf(const Json &lightResource);

/// What a command arrives as from core.
struct CommandValue {
    phicore::adapter::v1::ScalarValue scalar;
    Json json;
};

/// The PUT body for a light channel write, or empty with `error`.
std::string lightCommandBody(std::string_view channelId, const CommandValue &value,
                             std::string *error);

/// The contract's word for a Hue button event ("initial_press", ...).
phicore::adapter::v1::ButtonEventCode buttonEventFor(std::string_view hueEvent);

/// Connected / Disconnected / Limited for a zigbee_connectivity status.
std::optional<phicore::adapter::v1::ConnectivityStatus> connectivityOf(const Json &resource);

/// The device a resource belongs to, or empty.
std::string ownerDeviceId(const Json &resource);

/// Milliseconds since the epoch from a bridge timestamp, or 0.
std::int64_t hueTimestampMs(std::string_view text);

/// Signed steps of a relative_rotary event, or 0.
int rotarySteps(const Json &resource, std::int64_t *reportTsMs);

/// The button channel a button resource reports on, given how many buttons
/// the device has and the resource's control_id.
std::string buttonChannelId(int controlId, bool singleButton);

void rgbToXy(double r01, double g01, double b01, double *x, double *y);
void xyToRgb(double x, double y, double brightness01, double *r01, double *g01, double *b01);

} // namespace phicore::hue::ipc
