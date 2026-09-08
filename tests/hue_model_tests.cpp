// A Hue bridge's answers, and what they become.
//
// Everything here is pure conversion - a bridge payload in, a v1 type out, or
// a channel write in and the body the bridge expects out - so none of it
// needs a bridge. The one with a story is testAPollThatCouldNotFetchEverything.

#include <phi/adapter/testing/check.h>

#include "hue_events.h"
#include "hue_json.h"
#include "hue_model.h"
#include "hue_settings.h"

#include <cmath>
#include <string>

using namespace phicore::hue::ipc;
namespace v1 = phicore::adapter::v1;

namespace {

Json arrayOf(const char *text)
{
    return parseJson(text);
}

CommandValue scalar(v1::ScalarValue value)
{
    CommandValue out;
    out.scalar = std::move(value);
    return out;
}

Json bodyOf(const std::string &payload)
{
    return parseObject(payload);
}

const char *kDevices = R"json([
  {"id":"dev-lamp","type":"device","metadata":{"name":"Kitchen"},"product_data":{
     "manufacturer_name":"Signify Netherlands B.V.","model_id":"LCT007","product_name":"Hue color lamp",
     "product_archetype":"sultan_bulb","software_version":"1.104.2"},
   "services":[{"rid":"light-1","rtype":"light"}]},
  {"id":"dev-bridge","type":"device","metadata":{"name":"Hue Bridge"},"product_data":{
     "manufacturer_name":"Signify Netherlands B.V.","model_id":"BSB002","product_name":"Hue Bridge",
     "product_archetype":"bridge_v2","software_version":"1.66.1966060010"},
   "services":[{"rid":"zc-bridge","rtype":"zigbee_connectivity"}]},
  {"id":"dev-dial","type":"device","metadata":{"name":"EG Dial"},"product_data":{
     "manufacturer_name":"Signify Netherlands B.V.","model_id":"RDM002","product_name":"Hue tap dial switch",
     "product_archetype":"unknown_archetype","software_version":"2.59.19"},
   "services":[{"rid":"b1","rtype":"button"},{"rid":"b2","rtype":"button"},{"rid":"b3","rtype":"button"},
               {"rid":"b4","rtype":"button"},{"rid":"rr1","rtype":"relative_rotary"}]},
  {"id":"dev-plug","type":"device","metadata":{"name":"Media Plug"},"product_data":{
     "model_id":"LOM006","product_archetype":"plug"},"services":[{"rid":"light-2","rtype":"light"}]}
])json";

const char *kLights = R"json([
  {"id":"light-1","type":"light","owner":{"rid":"dev-lamp","rtype":"device"},
   "on":{"on":true},"dimming":{"brightness":50.0},
   "color_temperature":{"mirek":366,"mirek_schema":{"mirek_minimum":153,"mirek_maximum":500}},
   "color":{"xy":{"x":0.3127,"y":0.3290},"gamut":{"red":{"x":0.7,"y":0.3},"green":{"x":0.17,"y":0.7},"blue":{"x":0.15,"y":0.06}}},
   "effects":{"effect_values":["no_effect","candle","fire"]}},
  {"id":"light-2","type":"light","owner":{"rid":"dev-plug","rtype":"device"},"on":{"on":false}}
])json";

const char *kButtons = R"json([
  {"id":"b1","type":"button","owner":{"rid":"dev-dial","rtype":"device"},"metadata":{"control_id":1}},
  {"id":"b2","type":"button","owner":{"rid":"dev-dial","rtype":"device"},"metadata":{"control_id":2}},
  {"id":"b3","type":"button","owner":{"rid":"dev-dial","rtype":"device"},"metadata":{"control_id":3}},
  {"id":"b4","type":"button","owner":{"rid":"dev-dial","rtype":"device"},"metadata":{"control_id":4}}
])json";

const char *kConnectivity = R"json([
  {"id":"zc-bridge","type":"zigbee_connectivity","owner":{"rid":"dev-bridge","rtype":"device"},"status":"connected"},
  {"id":"zc-dial","type":"zigbee_connectivity","owner":{"rid":"dev-dial","rtype":"device"},"status":"connectivity_issue"}
])json";

const char *kPower = R"json([
  {"id":"dp-dial","type":"device_power","owner":{"rid":"dev-dial","rtype":"device"},"power_state":{"battery_level":87}}
])json";

Resources fullResources()
{
    Resources resources;
    resources.byType["device"] = arrayOf(kDevices);
    resources.byType["light"] = arrayOf(kLights);
    resources.byType["motion"] = Json::array();
    resources.byType["tamper"] = Json::array();
    resources.byType["temperature"] = Json::array();
    resources.byType["light_level"] = Json::array();
    resources.byType["device_power"] = arrayOf(kPower);
    resources.byType["button"] = arrayOf(kButtons);
    resources.byType["relative_rotary"] = arrayOf(R"json([{"id":"rr1","type":"relative_rotary","owner":{"rid":"dev-dial","rtype":"device"}}])json");
    resources.byType["zigbee_connectivity"] = arrayOf(kConnectivity);
    resources.byType["room"] = arrayOf(R"json([{"id":"room-1","type":"room","metadata":{"name":"Kitchen"},"children":[{"rid":"dev-lamp","rtype":"device"}]}])json");
    resources.byType["zone"] = Json::array();
    resources.byType["scene"] = arrayOf(R"json([{"id":"scene-1","type":"scene","metadata":{"name":"Relax"},"group":{"rid":"room-1","rtype":"room"}}])json");
    resources.byType["zigbee_device_discovery"] = arrayOf(R"json([{"id":"disc-1"}])json");
    return resources;
}

void testWhatABridgeTurnsInto()
{
    const Snapshot snapshot = buildSnapshot(fullResources());
    PHI_CHECK(snapshot.devices.size() == 4);
    PHI_CHECK(snapshot.discoveryResourceId == "disc-1");

    const DeviceEntry &lamp = snapshot.devices.at("dev-lamp");
    PHI_CHECK(lamp.device.name == "Kitchen");
    PHI_CHECK(lamp.device.deviceClass == v1::DeviceClass::Light);
    PHI_CHECK(lamp.device.model == "LCT007");
    PHI_CHECK(lamp.light.resourceId == "light-1");
    PHI_CHECK(lamp.light.on.has_value() && *lamp.light.on);
    PHI_CHECK(lamp.channel("on") != nullptr && lamp.channel("bri") != nullptr
              && lamp.channel("ct") != nullptr && lamp.channel("color") != nullptr);
    PHI_CHECK(lamp.channel("ct")->minValue == 153 && lamp.channel("ct")->maxValue == 500);
    PHI_CHECK(lamp.channel("bri")->hasValue && std::get<double>(lamp.channel("bri")->lastValue) == 50.0);
    PHI_CHECK(parseObject(lamp.channel("color")->metaJson).contains("colorCapabilities"));
    PHI_CHECK(lamp.device.effects.size() == 2 && lamp.device.effects[0].id == "candle");
    PHI_CHECK(lamp.channelSource.at("on") == "light");
    const auto color = colorOf(lamp.light);
    PHI_CHECK(color.has_value());

    // The bridge itself is a gateway, not a sensor.
    const DeviceEntry &bridge = snapshot.devices.at("dev-bridge");
    PHI_CHECK_MSG(bridge.device.deviceClass == v1::DeviceClass::Gateway, "the bridge is class %d",
                  static_cast<int>(bridge.device.deviceClass));
    PHI_CHECK(bridge.channel("zigbee_status") != nullptr);
    PHI_CHECK(bridge.channel("zigbee_status")->hasValue
              && std::get<std::int64_t>(bridge.channel("zigbee_status")->lastValue)
                  == static_cast<std::int64_t>(v1::ConnectivityStatus::Connected));

    const DeviceEntry &plug = snapshot.devices.at("dev-plug");
    PHI_CHECK(plug.device.deviceClass == v1::DeviceClass::Plug);

    // Four buttons and a dial, numbered by control id.
    const DeviceEntry &dial = snapshot.devices.at("dev-dial");
    PHI_CHECK(dial.device.deviceClass == v1::DeviceClass::Button);
    for (const char *id : {"button1", "button2", "button3", "button4", "dial", "battery", "zigbee_status"})
        PHI_CHECK_MSG(dial.channel(id) != nullptr, "the dial has no %s channel", id);
    PHI_CHECK(dial.channel("button2")->name == "Button 2");
    PHI_CHECK(v1::hasFlag(dial.device.flags, v1::DeviceFlag::Battery));
    PHI_CHECK(std::get<std::int64_t>(dial.channel("battery")->lastValue) == 87);
    PHI_CHECK(std::get<std::int64_t>(dial.channel("zigbee_status")->lastValue)
              == static_cast<std::int64_t>(v1::ConnectivityStatus::Limited));
    PHI_CHECK(dial.channelSource.at("button1") == "button");
    PHI_CHECK(dial.channelSource.at("dial") == "relative_rotary");

    PHI_CHECK(snapshot.rooms.size() == 1 && snapshot.rooms[0].deviceExternalIds.size() == 1);
    PHI_CHECK(snapshot.scenes.size() == 1 && snapshot.scenes[0].scopeExternalId == "room-1");

    // The same answers give the same key; a renamed device does not.
    Resources renamed = fullResources();
    renamed.byType["device"][0]["metadata"]["name"] = "Kitchen 2";
    const Snapshot again = buildSnapshot(fullResources());
    const Snapshot changed = buildSnapshot(renamed);
    PHI_CHECK(again.devices.at("dev-lamp").definitionKey() == lamp.definitionKey());
    PHI_CHECK(changed.devices.at("dev-lamp").definitionKey() != lamp.definitionKey());
    // A value change is not a definition change.
    Resources dimmed = fullResources();
    dimmed.byType["light"][0]["dimming"]["brightness"] = 10.0;
    PHI_CHECK(buildSnapshot(dimmed).devices.at("dev-lamp").definitionKey() == lamp.definitionKey());
}

/// The button flicker, as a test. A poll whose `button` fetch failed used to
/// yield a snapshot with no button channels; core removed them and the next
/// poll created them again with new ids. Now what could not be fetched is
/// carried over from the last poll that could.
void testAPollThatCouldNotFetchEverything()
{
    const Snapshot before = buildSnapshot(fullResources());

    Resources partial = fullResources();
    partial.byType.erase("button");
    partial.byType.erase("device_power");
    partial.byType.erase("room");
    const std::vector<std::string> missing = partial.missing();
    PHI_CHECK(missing.size() == 3);

    Snapshot next = buildSnapshot(partial);
    const DeviceEntry &bare = next.devices.at("dev-dial");
    PHI_CHECK_MSG(bare.channel("button1") == nullptr, "a poll without buttons produced buttons");
    PHI_CHECK(next.rooms.empty());

    carryOver(before, missing, next);
    const DeviceEntry &dial = next.devices.at("dev-dial");
    for (const char *id : {"button1", "button2", "button3", "button4", "battery"})
        PHI_CHECK_MSG(dial.channel(id) != nullptr, "%s was lost to a failed fetch", id);
    PHI_CHECK(dial.channelSource.at("button1") == "button");
    PHI_CHECK(v1::hasFlag(dial.device.flags, v1::DeviceFlag::Battery));
    PHI_CHECK(dial.channel("dial") != nullptr);
    PHI_CHECK(next.rooms.size() == 1);
    PHI_CHECK_MSG(dial.definitionKey() == before.devices.at("dev-dial").definitionKey(),
                  "a carried-over device would have been re-announced");

    // A device that is new to this poll gets nothing carried over: there is
    // nothing to carry.
    PHI_CHECK(next.devices.at("dev-lamp").channel("on") != nullptr);
}

void testWhatAChannelWriteBecomes()
{
    std::string error;
    Json on = bodyOf(lightCommandBody("on", scalar(true), &error));
    PHI_CHECK(on["on"]["on"] == true);

    // Setting a brightness also switches the lamp on; dimming to nothing
    // switches it off rather than leaving a lamp lit at zero.
    Json dim = bodyOf(lightCommandBody("bri", scalar(40.0), &error));
    PHI_CHECK(dim["dimming"]["brightness"] == 40.0);
    PHI_CHECK(dim["on"]["on"] == true);
    Json off = bodyOf(lightCommandBody("bri", scalar(0.0), &error));
    PHI_CHECK(off["on"]["on"] == false);

    Json ct = bodyOf(lightCommandBody("ct", scalar(static_cast<std::int64_t>(300)), &error));
    PHI_CHECK(ct["color_temperature"]["mirek"] == 300);
    Json clamped = bodyOf(lightCommandBody("ct", scalar(5.0), &error));
    PHI_CHECK(clamped["color_temperature"]["mirek"] == 100);

    CommandValue red;
    red.json = parseObject(R"({"r":255,"g":0,"b":0})");
    Json color = bodyOf(lightCommandBody("color", red, &error));
    PHI_CHECK(color.contains("color") && color["color"]["xy"]["x"].get<double>() > 0.6);
    Json hex = bodyOf(lightCommandBody("color", scalar(std::string("#00ff00")), &error));
    PHI_CHECK(hex["color"]["xy"]["y"].get<double>() > 0.5);

    PHI_CHECK(lightCommandBody("color", scalar(std::string("red")), &error).empty());
    PHI_CHECK(error == "Invalid color payload");
    PHI_CHECK(lightCommandBody("volume", scalar(1.0), &error).empty());
    PHI_CHECK(lightCommandBody("on", scalar(std::string("maybe")), &error).empty());

    // Colour round trip stays close.
    double x = 0.0;
    double y = 0.0;
    rgbToXy(0.2, 0.6, 0.9, &x, &y);
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    xyToRgb(x, y, 1.0, &r, &g, &b);
    PHI_CHECK(b > g && g > r);
}

void testWhatTheStreamSays()
{
    EventStreamParser parser;
    std::vector<Json> events = parser.feed(": hi\n\ndata: [{\"type\":\"update\",\"data\":[{\"id\":\"b1\",\"type\":\"button\",");
    PHI_CHECK(events.empty());
    events = parser.feed("\"owner\":{\"rid\":\"dev-dial\",\"rtype\":\"device\"},\"button\":{\"button_report\":{\"event\":\"short_release\",\"updated\":\"2026-09-08T12:00:00.250Z\"}}}]}]\n\n");
    PHI_CHECK(events.size() == 1);
    const std::vector<EventChange> changes = changesIn(events[0]);
    PHI_CHECK(changes.size() == 1 && changes[0].type == "update" && changes[0].resourceType == "button");
    PHI_CHECK(ownerDeviceId(changes[0].resource) == "dev-dial");
    PHI_CHECK(buttonEventFor(jsonString(jsonValue(jsonValue(changes[0].resource, "button"), "button_report"), "event"))
              == v1::ButtonEventCode::ShortPressRelease);
    PHI_CHECK(hueTimestampMs("2026-09-08T12:00:00.250Z") == 1788868800250LL);

    // Two events in one chunk, one split across lines with a CRLF.
    events = parser.feed("data: [{\"type\":\"update\",\"data\":[{\"type\":\"light\",\"owner\":{\"rid\":\"dev-lamp\",\"rtype\":\"device\"},\"on\":{\"on\":false},\"dimming\":{\"brightness\":12.5}}]}]\r\n\r\ndata: [{\"type\":\"delete\",\"data\":[{\"type\":\"device\",\"id\":\"x\"}]}]\n\n");
    PHI_CHECK(events.size() == 2);
    const std::vector<EventChange> lightChanges = changesIn(events[0]);
    PHI_CHECK(lightChanges.size() == 1);
    const std::vector<ChannelReport> reports = reportsFor("light", lightChanges[0].resource);
    PHI_CHECK(reports.size() == 2);
    PHI_CHECK(reports[0].channelId == "on" && std::get<bool>(reports[0].value) == false);
    PHI_CHECK(reports[1].channelId == "bri" && std::get<double>(reports[1].value) == 12.5);
    PHI_CHECK(changesIn(events[1])[0].type == "delete");

    const Json rotary = parseObject(R"({"type":"relative_rotary","owner":{"rid":"dev-dial","rtype":"device"},
      "relative_rotary":{"rotary_report":{"updated":"2026-09-08T12:00:01Z","rotation":{"direction":"counter_clock_wise","steps":24}}}})");
    std::int64_t at = 0;
    PHI_CHECK(rotarySteps(rotary, &at) == -24);
    PHI_CHECK(at == 1788868801000LL);
    PHI_CHECK(buttonChannelId(3, false) == "button3");
    PHI_CHECK(buttonChannelId(3, true) == "button");
}

/// The configuration reaches the call: address, port, key, and a TLS that
/// verifies the bridge's certificate against Signify's root and expects the
/// bridge id as its name.
void testWhereTheCallsGo()
{
    v1::Adapter adapter;
    adapter.externalId = "001788fffe21b7bc";
    adapter.host = "00178821b7bc.local";
    adapter.ip = "192.168.1.25";
    adapter.port = 443;
    adapter.token = "app-key-1";
    adapter.flags = v1::AdapterFlag::UseTls;

    const ConnectionSettings settings = settingsFromAdapter(adapter, parseObject("{}"));
    PHI_CHECK(settings.address() == "192.168.1.25");
    PHI_CHECK(settings.baseUrl() == "https://192.168.1.25:443");
    PHI_CHECK(settings.serverName == "001788fffe21b7bc");
    PHI_CHECK(settings.caFile == bundledCaFile());
    PHI_CHECK(!settings.caFile.empty());

    const auto call = callFor(settings, "PUT", "clip/v2/resource/light/l1", R"({"on":{"on":true}})", true);
    PHI_CHECK(call.url == "https://192.168.1.25:443/clip/v2/resource/light/l1");
    PHI_CHECK(call.method == "PUT");
    PHI_CHECK(call.tls.verifyHostname);
    PHI_CHECK(call.tlsServerName == "001788fffe21b7bc");
    PHI_CHECK(call.tls.caFile == bundledCaFile());
    bool keyed = false;
    for (const auto &[name, value] : call.headers)
        keyed = keyed || (name == "hue-application-key" && value == "app-key-1");
    PHI_CHECK(keyed);

    // A probe does not know the bridge id: chain only, no name.
    ConnectionSettings probe = settings;
    probe.serverName.clear();
    PHI_CHECK(!callFor(probe, "GET", "/api", {}, false).tls.verifyHostname);

    // Meta overrides: port, useTls as a string, a CA of one's own.
    const ConnectionSettings plain = settingsFromAdapter(
        adapter, parseObject(R"({"port":80,"useTls":"false","tlsCaFile":"/etc/ssl/mine.pem"})"));
    PHI_CHECK(!plain.useTls);
    PHI_CHECK(plain.baseUrl() == "http://192.168.1.25:80");
    PHI_CHECK(plain.caFile == "/etc/ssl/mine.pem");
    PHI_CHECK(!plain.sameBridge(settings));

    PHI_CHECK(bridgeErrorText(R"({"errors":[{"description":"invalid value"}]})") == "invalid value");
    PHI_CHECK(bridgeErrorText(R"([{"error":{"type":101,"address":"/","description":"link button not pressed"}}])")
              == "Press the link button on the Hue bridge, then retry.");
    PHI_CHECK(bridgeErrorText("nonsense").empty());
}

} // namespace

int main()
{
    testWhatABridgeTurnsInto();
    testAPollThatCouldNotFetchEverything();
    testWhatAChannelWriteBecomes();
    testWhatTheStreamSays();
    testWhereTheCallsGo();
    return phi::testing::report("hue_model_tests");
}
