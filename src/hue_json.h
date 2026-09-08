#pragma once

// JSON for this adapter: nlohmann, plus the handful of accessors QJsonObject
// used to provide. The same choice phi-core and the other Qt-free adapters
// made, so a value read here reads the way it reads there.
//
// Two differences from Qt JSON that matter at call sites:
//  - A default-constructed Json is null, not an empty object. Json::object()
//    is what "{}" means.
//  - There is no "undefined". A missing key reads as null; code that needs to
//    tell "absent" from "null" asks contains() first.

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace phicore::hue::ipc {

using Json = nlohmann::json;

/// Parses anything. Malformed text gives null.
Json parseJson(std::string_view text);

/// Parses an object. Empty, malformed or non-object text all give `{}` - the
/// caller has nothing to do differently for any of them.
Json parseObject(std::string_view text);

/// Compact UTF-8. Invalid UTF-8 is replaced rather than thrown on: a payload
/// assembled from broker bytes must not be able to abort the serializer.
std::string dump(const Json &value);

/// The value at `key`, or null. Never throws, never inserts.
Json jsonValue(const Json &obj, std::string_view key);

/// Empty unless the key holds a string. Trimmed when `trim`.
std::string jsonString(const Json &obj, std::string_view key, bool trim = true);

/// Integer at `key`, or `fallback`. A numeric string counts, as does a float.
int jsonInt(const Json &obj, std::string_view key, int fallback);

/// Number at `key`, or `fallback`. A numeric string counts.
double jsonDouble(const Json &obj, std::string_view key, double fallback);

/// True only for a JSON true; anything else, missing included, is `fallback`.
bool jsonBool(const Json &obj, std::string_view key, bool fallback = false);

/// The value as a string the way QJsonValue::toVariant().toString() read it:
/// strings as they are, numbers and booleans rendered, everything else empty.
std::string jsonScalarText(const Json &value);

} // namespace phicore::hue::ipc
