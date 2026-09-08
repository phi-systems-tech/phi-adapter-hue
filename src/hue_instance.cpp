#include "hue_instance.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "phi/adapter/net/http_client.h"
#include "phi/adapter/sdk/button_presses.h"
#include "phi/runtime/loop.h"
#include "phi/runtime/str.h"

#include "hue_events.h"
#include "hue_json.h"
#include "hue_model.h"
#include "hue_schema.h"
#include "hue_settings.h"

namespace phicore::hue::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;
namespace net = phicore::adapter::net;

using namespace std::chrono_literals;

namespace {

/// While the event stream is up, the poll is a safety net, not the source.
constexpr auto kPollWhileStreaming = 60s;
/// The stream is reopened quickly a few times, then at the retry interval.
constexpr auto kStreamFastRetry = 2s;
constexpr int kStreamFastRetries = 5;
/// An event that changes what exists (a device added, a room renamed) is
/// answered with a poll, but not one per event: a bridge announces a join
/// as a burst.
constexpr auto kEventPollDebounce = 3s;
/// A dial's rotation goes back to zero once it stops turning.
constexpr auto kDialResetDelay = 1500ms;
/// Core retracts its link-down blanket three seconds after link-up; the
/// bridge device, which nothing reports on again, is reported once more
/// after that. See phi-adapter-z2m for the measurement.
constexpr auto kLinkSettle = 4s;

std::int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string keyOf(const v1::ScalarValue &value)
{
    if (const bool *b = std::get_if<bool>(&value))
        return *b ? "b1" : "b0";
    if (const std::int64_t *i = std::get_if<std::int64_t>(&value))
        return "i" + str::number(static_cast<long long>(*i));
    if (const double *d = std::get_if<double>(&value))
        return "d" + str::number(*d);
    if (const std::string *s = std::get_if<std::string>(&value))
        return "s" + *s;
    return "-";
}

/// The last value reported per channel, so a poll that finds everything
/// where it left it reports nothing.
class ReportedValues
{
public:
    bool isNews(const std::string &deviceId, const std::string &channelId,
                const v1::ScalarValue &value)
    {
        const std::string key = deviceId + "|" + channelId;
        const std::string text = keyOf(value);
        const auto it = m_values.find(key);
        if (it != m_values.end() && it->second == text)
            return false;
        m_values[key] = text;
        return true;
    }
    bool isNewsColor(const std::string &deviceId, const v1::Color &color)
    {
        const std::string key = deviceId + "|color";
        const std::string text = str::number(color.r) + "," + str::number(color.g) + ","
            + str::number(color.b);
        const auto it = m_values.find(key);
        if (it != m_values.end() && it->second == text)
            return false;
        m_values[key] = text;
        return true;
    }
    void forgetDevice(const std::string &deviceId)
    {
        const std::string prefix = deviceId + "|";
        for (auto it = m_values.begin(); it != m_values.end();) {
            if (it->first.rfind(prefix, 0) == 0)
                it = m_values.erase(it);
            else
                ++it;
        }
    }
    void forget() { m_values.clear(); }

private:
    std::map<std::string, std::string> m_values;
};

std::string roomKey(const v1::Room &room)
{
    return room.name + "|" + room.zone + "|" + room.metaJson + "|"
        + str::join(room.deviceExternalIds, ",");
}

std::string groupKey(const v1::Group &group)
{
    return group.name + "|" + group.zone + "|" + group.metaJson + "|"
        + str::join(group.deviceExternalIds, ",");
}

std::string sceneKey(const v1::Scene &scene)
{
    return scene.name + "|" + scene.scopeExternalId + "|" + scene.scopeType + "|" + scene.metaJson;
}

std::string hueEffectNameFor(v1::DeviceEffect effect)
{
    switch (effect) {
    case v1::DeviceEffect::Candle:
        return "candle";
    case v1::DeviceEffect::Fireplace:
        return "fire";
    case v1::DeviceEffect::Sparkle:
        return "sparkle";
    case v1::DeviceEffect::ColorLoop:
        return "colorloop";
    case v1::DeviceEffect::Relax:
        return "sunset";
    case v1::DeviceEffect::Concentrate:
        return "enchant";
    case v1::DeviceEffect::Alarm:
        return "prism";
    default:
        return {};
    }
}

class HueInstance final : public sdk::AdapterInstance
{
protected:
    bool start() override
    {
        m_loop = phi::runtime::Loop::current();
        if (m_loop == nullptr) {
            std::cerr << "hue instance started off a loop; no timers are possible\n";
            return false;
        }
        m_http.emplace(*m_loop);
        m_streamHttp.emplace(*m_loop);
        m_lifecycle = Lifecycle::Running;
        setLinkUp(false, true);
        return true;
    }

    void stop() override
    {
        m_lifecycle = Lifecycle::Stopped;
        failQueued("Instance stopped");
        setLinkUp(false);
        releaseLoopResources();
    }

    void onDisconnected() override
    {
        // phi-core went away, and comes back with the configuration.
        m_lifecycle = Lifecycle::Paused;
        failQueued("Instance disconnected");
        stopPolling();
        closeStream();
        forgetBridge();
        m_linkUp = false;
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        if (m_lifecycle == Lifecycle::Stopped || !m_http)
            return;
        const ConnectionSettings previous = m_settings;
        m_info = request.adapter;
        m_meta = parseObject(request.adapter.metaJson);
        m_settings = settingsFromAdapter(m_info, m_meta);
        m_pollIntervalMs = std::clamp(jsonInt(m_meta, "pollIntervalMs", 5000), 1000, 600000);
        m_retryIntervalMs = std::clamp(jsonInt(m_meta, "retryIntervalMs", 10000), 1000, 600000);
        m_lifecycle = Lifecycle::Running;

        // Core sends config.changed for every change to the adapter's record,
        // including the meta this adapter writes. The same bridge with the
        // same key keeps its stream and its devices.
        if (m_settings.sameBridge(previous) && m_streamOpen) {
            std::cerr << "hue-ipc config.changed adapterId=" << request.adapterId
                      << " (same bridge, kept)\n";
            armPollTimer();
            return;
        }
        std::cerr << "hue-ipc config.changed adapterId=" << request.adapterId
                  << " externalId=" << m_info.externalId << " bridge=" << m_settings.baseUrl()
                  << " tls=" << (m_settings.useTls ? "verified" : "off")
                  << " keySet=" << (m_settings.appKey.empty() ? "false" : "true") << '\n';
        stopPolling();
        closeStream();
        forgetBridge();
        m_streamRetries = 0;
        openStream();
        armPollTimer();
        beginPoll();
    }

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        if (m_lifecycle != Lifecycle::Running || !m_http) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Instance is not running");
            return;
        }
        const auto deviceIt = m_snapshot.devices.find(request.deviceExternalId);
        if (deviceIt == m_snapshot.devices.end()) {
            answerCommand(request.cmdId, v1::CmdStatus::NotSupported, "Unknown device");
            return;
        }
        const DeviceEntry &entry = deviceIt->second;
        if (entry.light.resourceId.empty()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument,
                          "No Hue light resource for device");
            return;
        }
        CommandValue value;
        value.scalar = request.value;
        if (!request.hasScalarValue)
            value.json = parseJson(request.valueJson);
        std::string error;
        const std::string body = lightCommandBody(request.channelExternalId, value, &error);
        if (body.empty()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument, error);
            return;
        }
        const v1::CmdId cmdId = request.cmdId;
        enqueue(callFor(m_settings, "PUT", "/clip/v2/resource/light/" + entry.light.resourceId, body,
                        true),
                [this, cmdId](net::HttpClient::Result result) {
                    // The bridge's answer, not the request having left. The
                    // new state comes back on the event stream like every
                    // other state; nothing is reported from here.
                    if (result.ok)
                        answerCommand(cmdId, v1::CmdStatus::Success, {});
                    else
                        answerCommand(cmdId, failureStatus(result), failureText(result, "Hue command failed"));
                });
    }

    void onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        const std::string actionId = str::trimmed(request.actionId);
        if (actionId != "startDeviceDiscovery") {
            answerAction(request.cmdId, v1::CmdStatus::NotImplemented, "Unsupported adapter action");
            return;
        }
        if (m_lifecycle != Lifecycle::Running || !m_http) {
            answerAction(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Instance is not running");
            return;
        }
        if (m_snapshot.discoveryResourceId.empty()) {
            answerAction(request.cmdId, v1::CmdStatus::Failure, "Discovery resource not ready yet");
            return;
        }
        const std::string body = dump(Json{
            {"state", "start"}, {"action", Json{{"type", "search"}, {"action_type", "search"}}}});
        const v1::CmdId cmdId = request.cmdId;
        enqueue(callFor(m_settings, "PUT",
                        "/clip/v2/resource/zigbee_device_discovery/" + m_snapshot.discoveryResourceId,
                        body, true),
                [this, cmdId](net::HttpClient::Result result) {
                    if (!result.ok) {
                        answerAction(cmdId, failureStatus(result),
                                     failureText(result, "Failed to start Hue Zigbee discovery"));
                        return;
                    }
                    v1::ActionResponse response;
                    response.id = cmdId;
                    response.tsMs = nowMs();
                    response.status = v1::CmdStatus::Success;
                    response.resultType = v1::ActionResultType::String;
                    response.resultValue = std::string("Hue Zigbee discovery started");
                    sendAction(response);
                });
    }

    void onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        if (m_lifecycle != Lifecycle::Running || !m_http) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Instance is not running");
            return;
        }
        const std::string name = str::trimmed(request.name);
        if (request.deviceExternalId.empty() || name.empty()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument, "Device and name are required");
            return;
        }
        const std::string body = dump(Json{{"metadata", Json{{"name", name}}}});
        const v1::CmdId cmdId = request.cmdId;
        const std::string deviceId = request.deviceExternalId;
        enqueue(callFor(m_settings, "PUT", "/clip/v2/resource/device/" + deviceId, body, true),
                [this, cmdId, deviceId, name](net::HttpClient::Result result) {
                    if (!result.ok) {
                        answerCommand(cmdId, failureStatus(result),
                                      failureText(result, "Rename request failed"));
                        return;
                    }
                    const auto it = m_snapshot.devices.find(deviceId);
                    if (it != m_snapshot.devices.end()) {
                        it->second.device.name = name;
                        announce(it->second);
                    }
                    answerCommand(cmdId, v1::CmdStatus::Success, {});
                });
    }

    void onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        if (m_lifecycle != Lifecycle::Running || !m_http) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Instance is not running");
            return;
        }
        const auto deviceIt = m_snapshot.devices.find(request.deviceExternalId);
        if (deviceIt == m_snapshot.devices.end() || deviceIt->second.light.resourceId.empty()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument,
                          "No Hue light resource for device");
            return;
        }
        const DeviceEntry &entry = deviceIt->second;
        const std::string effectId = str::trimmed(request.effectId);
        const v1::DeviceEffectDescriptor *descriptor = nullptr;
        for (const v1::DeviceEffectDescriptor &candidate : entry.device.effects) {
            if (!effectId.empty() && candidate.id == effectId) {
                descriptor = &candidate;
                break;
            }
            if (descriptor == nullptr && request.effect != v1::DeviceEffect::None
                && candidate.effect == request.effect)
                descriptor = &candidate;
        }
        const Json descriptorMeta = descriptor ? parseObject(descriptor->metaJson) : Json::object();
        std::string hueEffect = jsonString(descriptorMeta, "hueEffect");
        if (hueEffect.empty())
            hueEffect = effectId;
        if (hueEffect.empty())
            hueEffect = hueEffectNameFor(request.effect);
        if (hueEffect.empty()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument,
                          "Unsupported effect for this device");
            return;
        }
        std::string category = jsonString(descriptorMeta, "hueEffectCategory");
        if (category.empty())
            category = "effects";
        const Json params = parseObject(request.paramsJson);
        Json body = Json::object();
        if (category == "timed_effects") {
            Json timed = Json{{"effect", hueEffect}};
            if (params.contains("duration"))
                timed["duration"] = params.at("duration");
            body["timed_effects"] = timed;
        } else {
            body["effects"] = Json{{"effect", hueEffect}};
        }
        const v1::CmdId cmdId = request.cmdId;
        enqueue(callFor(m_settings, "PUT", "/clip/v2/resource/light/" + entry.light.resourceId,
                        dump(body), true),
                [this, cmdId, hueEffect](net::HttpClient::Result result) {
                    if (!result.ok) {
                        answerCommand(cmdId, failureStatus(result),
                                      failureText(result, "Hue effect request failed"));
                        return;
                    }
                    v1::CmdResponse response = responseFor(cmdId, v1::CmdStatus::Success, {});
                    response.finalValue = hueEffect;
                    sendCommand(response);
                });
    }

    void onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        if (m_lifecycle != Lifecycle::Running || !m_http) {
            answerCommand(request.cmdId, v1::CmdStatus::TemporarilyOffline, "Instance is not running");
            return;
        }
        if (request.sceneExternalId.empty()) {
            answerCommand(request.cmdId, v1::CmdStatus::InvalidArgument, "sceneExternalId missing");
            return;
        }
        const std::string action = str::toLower(str::trimmed(request.action));
        std::string recallAction = "active";
        if (action == "deactivate")
            recallAction = "inactive";
        else if (action == "dynamic")
            recallAction = "dynamic_palette";
        Json recall = Json{{"action", recallAction}};
        const std::string groupId = str::trimmed(request.groupExternalId);
        if (!groupId.empty())
            recall["target"] = Json{{"rid", groupId}, {"rtype", "zone"}};
        const v1::CmdId cmdId = request.cmdId;
        enqueue(callFor(m_settings, "PUT", "/clip/v2/resource/scene/" + request.sceneExternalId,
                        dump(Json{{"recall", recall}}), true),
                [this, cmdId](net::HttpClient::Result result) {
                    if (result.ok)
                        answerCommand(cmdId, v1::CmdStatus::Success, {});
                    else
                        answerCommand(cmdId, failureStatus(result),
                                      failureText(result, "Scene invocation failed"));
                });
    }

private:
    enum class Lifecycle { Idle, Running, Paused, Stopped };

    struct Queued {
        net::HttpClient::Call call;
        net::HttpClient::Done done;
    };

    // --- one request at a time --------------------------------------------

    /// Commands and poll steps share the client, in order. A command does
    /// not wait behind a whole poll: a poll is one request at a time too.
    void enqueue(net::HttpClient::Call call, net::HttpClient::Done done)
    {
        m_queue.push_back({std::move(call), std::move(done)});
        pump();
    }

    void pump()
    {
        if (!m_http || m_http->busy() || m_queue.empty())
            return;
        Queued next = std::move(m_queue.front());
        m_queue.pop_front();
        net::HttpClient::Done done = std::move(next.done);
        const bool issued = m_http->send(std::move(next.call), [this, done](net::HttpClient::Result result) {
            if (done)
                done(std::move(result));
            pump();
        });
        if (!issued && done) {
            net::HttpClient::Result result;
            result.error = "request could not be issued";
            done(std::move(result));
            pump();
        }
    }

    void failQueued(const std::string &reason)
    {
        std::deque<Queued> queue = std::move(m_queue);
        m_queue.clear();
        if (m_http)
            m_http->cancel();
        for (Queued &entry : queue) {
            if (entry.done) {
                net::HttpClient::Result result;
                result.error = reason;
                entry.done(std::move(result));
            }
        }
        m_pollRunning = false;
    }

    static v1::CmdStatus failureStatus(const net::HttpClient::Result &result)
    {
        if (result.unauthorized || result.status == 403)
            return v1::CmdStatus::NotAuthorized;
        if (result.status >= 400)
            return v1::CmdStatus::Failure;
        return v1::CmdStatus::TemporarilyOffline;
    }

    static std::string failureText(const net::HttpClient::Result &result, const char *fallback)
    {
        const std::string fromBridge = bridgeErrorText(result.body);
        if (!fromBridge.empty())
            return fromBridge;
        return result.error.empty() ? fallback : result.error;
    }

    // --- the poll ---------------------------------------------------------

    void armPollTimer()
    {
        if (!m_loop || m_lifecycle != Lifecycle::Running)
            return;
        const std::chrono::milliseconds interval = m_streamActive
            ? std::max(std::chrono::milliseconds(m_pollIntervalMs),
                       std::chrono::duration_cast<std::chrono::milliseconds>(kPollWhileStreaming))
            : std::chrono::milliseconds(m_linkUp ? m_pollIntervalMs : m_retryIntervalMs);
        if (m_pollTimer && interval == m_pollTimerInterval)
            return;
        m_pollTimerInterval = interval;
        m_pollTimer = m_loop->timerEvery(interval, [this]() { beginPoll(); });
    }

    void stopPolling()
    {
        m_pollTimer.reset();
        m_pollTimerInterval = 0ms;
        m_eventPoll.reset();
        m_pollRunning = false;
    }

    void beginPoll()
    {
        if (m_lifecycle != Lifecycle::Running || !m_http || m_pollRunning)
            return;
        if (m_settings.address().empty()) {
            pollFailed("Bridge host is empty");
            return;
        }
        if (m_settings.appKey.empty()) {
            pollFailed("Hue application key missing");
            return;
        }
        m_pollRunning = true;
        m_pollResources = Resources{};
        m_pollFailures.clear();
        m_pollStep = 0;
        pollStep();
    }

    void pollStep()
    {
        const std::vector<std::string> &types = pollResourceTypes();
        if (m_pollStep >= types.size()) {
            finishPoll();
            return;
        }
        const std::string type = types[m_pollStep++];
        enqueue(callFor(m_settings, "GET", "/clip/v2/resource/" + type, {}, true),
                [this, type](net::HttpClient::Result result) {
                    if (!m_pollRunning)
                        return;
                    if (result.ok) {
                        const Json data = jsonValue(parseJson(result.body), "data");
                        if (data.is_array()) {
                            m_pollResources.byType[type] = data;
                            if (type == "button")
                                m_pollButtons = data;
                        } else {
                            m_pollFailures[type] = "response has no data array";
                        }
                    } else {
                        m_pollFailures[type] = failureText(result, "request failed");
                        if (resourceTypeRequired(type)) {
                            m_pollRunning = false;
                            pollFailed("Hue " + type + ": " + m_pollFailures[type]);
                            return;
                        }
                    }
                    pollStep();
                });
    }

    void finishPoll()
    {
        m_pollRunning = false;
        // What could not be fetched is said, once per poll, and then left
        // exactly as it was: a resource type this poll knows nothing about
        // takes nothing away.
        if (!m_pollFailures.empty()) {
            std::string what;
            for (const auto &[type, error] : m_pollFailures)
                what += (what.empty() ? "" : ", ") + type + " (" + error + ")";
            log(sdk::LogLevel::Warn, sdk::LogCategory::Network,
                "poll: some resources could not be fetched and were kept from the previous poll: "
                    + what,
                {}, "poll");
            std::cerr << "hue-ipc poll: kept " << what << '\n';
        }
        Snapshot next = buildSnapshot(m_pollResources);
        carryOver(m_snapshot, m_pollResources.missing(), next);
        publish(next);
        m_pollFailures.clear();
        m_pollResources = Resources{};
        m_pollFailedBefore.clear();
        setLinkUp(true);
        armPollTimer();
    }

    void pollFailed(const std::string &error)
    {
        m_pollRunning = false;
        if (error != m_pollFailedBefore) {
            m_pollFailedBefore = error;
            std::cerr << "hue-ipc poll failed: " << error << '\n';
            v1::Utf8String sendErr;
            sendError(sdk::LogCategory::Network, "poll: " + error, {}, "poll", {}, nowMs(), &sendErr);
        }
        if (!m_streamActive)
            setLinkUp(false);
        armPollTimer();
    }

    void schedulePollSoon()
    {
        if (!m_loop || m_lifecycle != Lifecycle::Running)
            return;
        m_eventPoll = m_loop->timerAfter(kEventPollDebounce, [this]() {
            m_eventPoll.reset();
            beginPoll();
        });
    }

    // --- publishing a snapshot --------------------------------------------

    void publish(Snapshot &next)
    {
        v1::Utf8String error;
        const std::int64_t ts = nowMs();

        for (const auto &[deviceId, entry] : m_snapshot.devices) {
            if (next.devices.count(deviceId))
                continue;
            sendDeviceRemoved(deviceId, &error);
            m_reported.forgetDevice(deviceId);
            m_definitionKeys.erase(deviceId);
        }

        int announced = 0;
        for (auto &[deviceId, entry] : next.devices) {
            const std::string key = entry.definitionKey();
            const auto known = m_definitionKeys.find(deviceId);
            if (known == m_definitionKeys.end() || known->second != key) {
                announce(entry);
                m_definitionKeys[deviceId] = key;
                ++announced;
            }
            for (const v1::Channel &channel : entry.channels) {
                if (channel.hasValue)
                    report(deviceId, channel.externalId, channel.lastValue, ts);
            }
            if (const auto color = colorOf(entry.light))
                reportColor(deviceId, *color, ts);
        }

        std::set<std::string> nextRooms;
        for (const v1::Room &room : next.rooms) {
            nextRooms.insert(room.externalId);
            const std::string key = roomKey(room);
            if (m_roomKeys[room.externalId] != key) {
                m_roomKeys[room.externalId] = key;
                sendRoomUpdated(room, &error);
            }
        }
        for (auto it = m_roomKeys.begin(); it != m_roomKeys.end();) {
            if (nextRooms.count(it->first)) {
                ++it;
                continue;
            }
            sendRoomRemoved(it->first, &error);
            it = m_roomKeys.erase(it);
        }

        std::set<std::string> nextGroups;
        for (const v1::Group &group : next.groups) {
            nextGroups.insert(group.externalId);
            const std::string key = groupKey(group);
            if (m_groupKeys[group.externalId] != key) {
                m_groupKeys[group.externalId] = key;
                sendGroupUpdated(group, &error);
            }
        }
        for (auto it = m_groupKeys.begin(); it != m_groupKeys.end();) {
            if (nextGroups.count(it->first)) {
                ++it;
                continue;
            }
            sendGroupRemoved(it->first, &error);
            it = m_groupKeys.erase(it);
        }

        std::set<std::string> nextScenes;
        for (const v1::Scene &scene : next.scenes) {
            nextScenes.insert(scene.externalId);
            const std::string key = sceneKey(scene);
            if (m_sceneKeys[scene.externalId] != key) {
                m_sceneKeys[scene.externalId] = key;
                sendSceneUpdated(scene, &error);
            }
        }
        for (auto it = m_sceneKeys.begin(); it != m_sceneKeys.end();) {
            if (nextScenes.count(it->first)) {
                ++it;
                continue;
            }
            sendSceneRemoved(it->first, &error);
            it = m_sceneKeys.erase(it);
        }

        if (announced > 0 || m_snapshot.devices.size() != next.devices.size()) {
            std::cerr << "hue-ipc devices: known=" << next.devices.size()
                      << " announced=" << announced << " rooms=" << next.rooms.size()
                      << " zones=" << next.groups.size() << " scenes=" << next.scenes.size() << '\n';
        }
        m_snapshot = std::move(next);
        rebuildButtonMap();
        if (announced > 0)
            reportBridgeReachable(ts);
    }

    void announce(const DeviceEntry &entry)
    {
        v1::Utf8String error;
        if (!sendDeviceUpdated(entry.device, entry.channels, &error))
            std::cerr << "failed to send deviceUpdated(" << entry.device.externalId << "): " << error
                      << '\n';
    }

    /// The bridge device has a connectivity channel core will never hear
    /// about again from an event, so it is said whenever the link comes up.
    void reportBridgeReachable(std::int64_t ts)
    {
        if (!m_linkUp)
            return;
        for (const auto &[deviceId, entry] : m_snapshot.devices) {
            if (entry.device.deviceClass != v1::DeviceClass::Gateway)
                continue;
            if (entry.channel("zigbee_status") == nullptr)
                continue;
            m_reported.forgetDevice(deviceId);
            report(deviceId, "zigbee_status",
                   static_cast<std::int64_t>(v1::ConnectivityStatus::Connected), ts);
        }
    }

    void rebuildButtonMap()
    {
        // Which button resource reports on which channel: from the button
        // resources by owner and control_id, which is what the snapshot
        // built the channels from.
        m_buttonChannelByResource.clear();
        const Json &buttons = m_pollButtons;
        std::map<std::string, int> countByDevice;
        for (const Json &button : buttons) {
            const std::string deviceId = ownerDeviceId(button);
            if (!deviceId.empty())
                ++countByDevice[deviceId];
        }
        for (const Json &button : buttons) {
            const std::string deviceId = ownerDeviceId(button);
            const std::string resourceId = jsonString(button, "id");
            if (deviceId.empty() || resourceId.empty())
                continue;
            const int controlId = jsonInt(jsonValue(button, "metadata"), "control_id", 0);
            m_buttonChannelByResource[resourceId] =
                buttonChannelId(controlId, countByDevice[deviceId] <= 1);
        }
    }

    // --- the event stream -------------------------------------------------

    void openStream()
    {
        if (m_lifecycle != Lifecycle::Running || !m_streamHttp || m_streamOpen)
            return;
        m_streamRetry.reset();
        if (m_settings.address().empty() || m_settings.appKey.empty()) {
            scheduleStreamRetry();
            return;
        }
        net::HttpClient::Call call = callFor(m_settings, "GET", "/eventstream/clip/v2", {}, true);
        for (net::Header &header : call.headers) {
            if (header.first == "Accept")
                header.second = "text/event-stream";
        }
        m_events.reset();
        m_streamOpen = m_streamHttp->stream(call, {
            .head = [this](int status, const std::vector<net::Header> &) {
                (void)status;
                std::cerr << "hue-ipc eventstream open\n";
                m_streamActive = true;
                m_streamRetries = 0;
                setLinkUp(true);
                armPollTimer();
            },
            .chunk = [this](std::string_view piece) { onStreamBytes(piece); },
            .done = [this](net::HttpClient::Result result) { onStreamEnded(result); },
        });
        if (!m_streamOpen)
            scheduleStreamRetry();
    }

    void closeStream()
    {
        if (m_streamHttp)
            m_streamHttp->cancel();
        m_streamOpen = false;
        m_streamActive = false;
        m_streamRetry.reset();
        m_events.reset();
    }

    void onStreamEnded(const net::HttpClient::Result &result)
    {
        m_streamOpen = false;
        const bool wasActive = m_streamActive;
        m_streamActive = false;
        if (result.ok) {
            std::cerr << "hue-ipc eventstream finished\n";
        } else {
            const std::string reason = failureText(result, "connection lost");
            std::cerr << "hue-ipc eventstream error: " << reason << '\n';
            if (reason != m_streamFailedBefore) {
                m_streamFailedBefore = reason;
                v1::Utf8String error;
                sendError(sdk::LogCategory::Network, "eventstream: " + reason, {}, "eventstream", {},
                          nowMs(), &error);
            }
            if (wasActive || result.unauthorized)
                setLinkUp(false);
        }
        scheduleStreamRetry();
        armPollTimer();
    }

    void scheduleStreamRetry()
    {
        if (!m_loop || m_lifecycle != Lifecycle::Running)
            return;
        std::chrono::milliseconds delay(m_retryIntervalMs);
        if (m_streamRetries < kStreamFastRetries) {
            ++m_streamRetries;
            delay = kStreamFastRetry;
        }
        m_streamRetry = m_loop->timerAfter(delay, [this]() {
            m_streamRetry.reset();
            openStream();
        });
    }

    void onStreamBytes(std::string_view piece)
    {
        const std::int64_t ts = nowMs();
        for (const Json &event : m_events.feed(piece)) {
            for (const EventChange &change : changesIn(event))
                applyChange(change, ts);
        }
    }

    void applyChange(const EventChange &change, std::int64_t ts)
    {
        if (change.type == "add" || change.type == "delete") {
            schedulePollSoon();
            return;
        }
        if (change.type != "update")
            return;
        const std::string &type = change.resourceType;
        if (type == "button") {
            handleButton(change.resource, ts);
            return;
        }
        if (type == "relative_rotary") {
            handleRotary(change.resource, ts);
            return;
        }
        if (type == "device" || type == "room" || type == "zone" || type == "scene"
            || type == "zigbee_device_discovery") {
            // What exists, or what it is called, changed: the poll answers
            // that, once the burst is over.
            schedulePollSoon();
            return;
        }
        const std::string deviceId = ownerDeviceId(change.resource);
        if (deviceId.empty())
            return;
        const auto deviceIt = m_snapshot.devices.find(deviceId);
        if (deviceIt == m_snapshot.devices.end()) {
            schedulePollSoon();
            return;
        }
        DeviceEntry &entry = deviceIt->second;
        for (const ChannelReport &value : reportsFor(type, change.resource)) {
            if (entry.channel(value.channelId) == nullptr)
                continue;
            report(deviceId, value.channelId, value.value, ts);
        }
        if (type == "light") {
            bool colorChanged = false;
            if (const auto xy = xyOf(change.resource)) {
                entry.light.xy = xy;
                colorChanged = true;
            }
            if (const auto brightness = brightnessOf(change.resource)) {
                entry.light.brightness = brightness;
                colorChanged = colorChanged || entry.light.xy.has_value();
            }
            if (colorChanged) {
                if (const auto color = colorOf(entry.light))
                    reportColor(deviceId, *color, ts);
            }
        }
    }

    void handleButton(const Json &resource, std::int64_t ts)
    {
        const std::string deviceId = ownerDeviceId(resource);
        const std::string resourceId = jsonString(resource, "id");
        if (deviceId.empty() || resourceId.empty())
            return;
        const Json button = jsonValue(resource, "button");
        const Json reportObj = jsonValue(button, "button_report");
        std::string eventName = jsonString(button, "last_event");
        if (eventName.empty())
            eventName = jsonString(reportObj, "event");
        const v1::ButtonEventCode code = buttonEventFor(eventName);
        if (code == v1::ButtonEventCode::None)
            return;
        std::int64_t eventTs = hueTimestampMs(jsonString(reportObj, "updated"));
        if (eventTs <= 0)
            eventTs = ts;

        std::string channelId;
        const auto mapped = m_buttonChannelByResource.find(resourceId);
        if (mapped != m_buttonChannelByResource.end()) {
            channelId = mapped->second;
        } else {
            // A button whose resource the last poll did not list: by its
            // control id if the device has that channel, else the one
            // button channel there is.
            const auto deviceIt = m_snapshot.devices.find(deviceId);
            const int controlId = jsonInt(jsonValue(resource, "metadata"), "control_id", 0);
            if (deviceIt != m_snapshot.devices.end()) {
                const std::string byControl = buttonChannelId(controlId, false);
                if (deviceIt->second.channel(byControl) != nullptr)
                    channelId = byControl;
                else if (deviceIt->second.channel("button") != nullptr)
                    channelId = "button";
            }
            if (channelId.empty())
                return;
        }

        const std::string key = deviceId + "|" + channelId;
        const sdk::ButtonPresses::Outcome outcome = m_presses.onEvent(key, code, eventTs);
        for (const sdk::ButtonPresses::Report &entry : outcome.report)
            report(deviceId, channelId, static_cast<std::int64_t>(entry.code), entry.tsMs, true);
        if (outcome.cancelWindow)
            m_pressWindows.erase(key);
        if (outcome.windowUntilMs) {
            const auto delay = std::chrono::milliseconds(
                std::max<std::int64_t>(1, *outcome.windowUntilMs - nowMs()));
            m_pressWindows[key] = m_loop->timerAfter(delay, [this, key, deviceId, channelId]() {
                m_pressWindows.erase(key);
                for (const sdk::ButtonPresses::Report &entry : m_presses.onWindowClosed(key))
                    report(deviceId, channelId, static_cast<std::int64_t>(entry.code), entry.tsMs, true);
            });
        }
    }

    void handleRotary(const Json &resource, std::int64_t ts)
    {
        const std::string deviceId = ownerDeviceId(resource);
        if (deviceId.empty())
            return;
        std::int64_t eventTs = 0;
        const int steps = rotarySteps(resource, &eventTs);
        if (steps == 0)
            return;
        if (eventTs <= 0)
            eventTs = ts;
        report(deviceId, "dial", static_cast<std::int64_t>(steps), eventTs, true);
        m_dialResets[deviceId] = m_loop->timerAfter(kDialResetDelay, [this, deviceId]() {
            m_dialResets.erase(deviceId);
            report(deviceId, "dial", static_cast<std::int64_t>(0), nowMs(), true);
        });
    }

    // --- reporting --------------------------------------------------------

    /// `always`: an event is news even when it repeats the last value - a
    /// second identical click is a second click.
    void report(const std::string &deviceId, const std::string &channelId,
                const v1::ScalarValue &value, std::int64_t ts, bool always = false)
    {
        if (!always && !m_reported.isNews(deviceId, channelId, value))
            return;
        if (always)
            m_reported.isNews(deviceId, channelId, value);
        v1::Utf8String error;
        if (!sendChannelStateUpdated(deviceId, channelId, value, ts, &error))
            std::cerr << "failed to send channelStateUpdated(" << channelId << "): " << error << '\n';
    }

    void reportColor(const std::string &deviceId, const v1::Color &color, std::int64_t ts)
    {
        if (!m_reported.isNewsColor(deviceId, color))
            return;
        v1::Utf8String error;
        if (!sendChannelColorStateUpdated(deviceId, "color", color.r, color.g, color.b, ts, &error))
            std::cerr << "failed to send channelColorStateUpdated: " << error << '\n';
    }

    void setLinkUp(bool up, bool force = false)
    {
        if (m_linkUp == up && !force)
            return;
        m_linkUp = up;
        std::cerr << "hue-ipc link " << (up ? "up" : "down") << '\n';
        v1::Utf8String error;
        if (!sendConnectionStateChanged(up, &error))
            std::cerr << "failed to send connectionStateChanged: " << error << '\n';
        m_linkSettle.reset();
        if (!up)
            return;
        reportBridgeReachable(nowMs());
        m_linkSettle = m_loop->timerAfter(kLinkSettle, [this]() { reportBridgeReachable(nowMs()); });
    }

    void forgetBridge()
    {
        m_snapshot = Snapshot{};
        m_definitionKeys.clear();
        m_roomKeys.clear();
        m_groupKeys.clear();
        m_sceneKeys.clear();
        m_reported.forget();
        m_presses.clear();
        m_pressWindows.clear();
        m_dialResets.clear();
        m_buttonChannelByResource.clear();
        m_pollButtons = Json::array();
        m_pollFailedBefore.clear();
        m_streamFailedBefore.clear();
    }

    // --- answering --------------------------------------------------------

    static v1::CmdResponse responseFor(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::CmdResponse response;
        response.id = cmdId;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        return response;
    }

    void sendCommand(const v1::CmdResponse &response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            std::cerr << "failed to send cmd result: " << error << '\n';
    }

    void sendAction(const v1::ActionResponse &response)
    {
        v1::Utf8String error;
        if (!sendResult(response, &error))
            std::cerr << "failed to send action result: " << error << '\n';
    }

    void answerCommand(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        sendCommand(responseFor(cmdId, status, error));
    }

    void answerAction(v1::CmdId cmdId, v1::CmdStatus status, const std::string &error)
    {
        v1::ActionResponse response;
        response.id = cmdId;
        response.tsMs = nowMs();
        response.status = status;
        response.error = error;
        response.resultType = v1::ActionResultType::None;
        sendAction(response);
    }

    void releaseLoopResources()
    {
        stopPolling();
        closeStream();
        m_linkSettle.reset();
        m_pressWindows.clear();
        m_dialResets.clear();
        m_http.reset();
        m_streamHttp.reset();
        m_loop = nullptr;
    }

    // --- state ------------------------------------------------------------

    phi::runtime::Loop *m_loop = nullptr;
    Lifecycle m_lifecycle = Lifecycle::Idle;
    std::optional<net::HttpClient> m_http;
    std::optional<net::HttpClient> m_streamHttp;
    std::deque<Queued> m_queue;

    v1::Adapter m_info;
    Json m_meta = Json::object();
    ConnectionSettings m_settings;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;

    phi::runtime::Timer m_pollTimer;
    std::chrono::milliseconds m_pollTimerInterval{0};
    phi::runtime::Timer m_eventPoll;
    bool m_pollRunning = false;
    std::size_t m_pollStep = 0;
    Resources m_pollResources;
    std::map<std::string, std::string> m_pollFailures;
    std::string m_pollFailedBefore;
    Json m_pollButtons = Json::array();

    bool m_streamOpen = false;
    bool m_streamActive = false;
    int m_streamRetries = 0;
    phi::runtime::Timer m_streamRetry;
    std::string m_streamFailedBefore;
    EventStreamParser m_events;

    bool m_linkUp = false;
    phi::runtime::Timer m_linkSettle;

    Snapshot m_snapshot;
    std::map<std::string, std::string> m_definitionKeys;
    std::map<std::string, std::string> m_roomKeys;
    std::map<std::string, std::string> m_groupKeys;
    std::map<std::string, std::string> m_sceneKeys;
    ReportedValues m_reported;
    std::map<std::string, std::string> m_buttonChannelByResource;
    sdk::ButtonPresses m_presses;
    std::map<std::string, phi::runtime::Timer> m_pressWindows;
    std::map<std::string, phi::runtime::Timer> m_dialResets;
};

} // namespace

std::unique_ptr<sdk::AdapterInstance> makeInstance()
{
    return std::make_unique<HueInstance>();
}

} // namespace phicore::hue::ipc
