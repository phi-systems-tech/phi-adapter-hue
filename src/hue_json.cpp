#include "hue_json.h"

#include "phi/runtime/str.h"

namespace phicore::hue::ipc {

namespace str = phi::str;

Json parseJson(std::string_view text)
{
    if (text.empty())
        return Json();
    const Json parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded())
        return Json();
    return parsed;
}

Json parseObject(std::string_view text)
{
    const Json parsed = parseJson(text);
    if (!parsed.is_object())
        return Json::object();
    return parsed;
}

std::string dump(const Json &value)
{
    return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

Json jsonValue(const Json &obj, std::string_view key)
{
    if (!obj.is_object())
        return Json();
    const auto it = obj.find(key);
    if (it == obj.end())
        return Json();
    return *it;
}

std::string jsonString(const Json &obj, std::string_view key, bool trim)
{
    const Json value = jsonValue(obj, key);
    if (!value.is_string())
        return {};
    return trim ? str::trimmed(value.get<std::string>()) : value.get<std::string>();
}

int jsonInt(const Json &obj, std::string_view key, int fallback)
{
    const Json value = jsonValue(obj, key);
    if (value.is_number_integer())
        return value.get<int>();
    if (value.is_number_float())
        return static_cast<int>(value.get<double>());
    if (value.is_string()) {
        bool ok = false;
        const int parsed = str::toInt(str::trimmed(value.get<std::string>()), &ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

double jsonDouble(const Json &obj, std::string_view key, double fallback)
{
    const Json value = jsonValue(obj, key);
    if (value.is_number())
        return value.get<double>();
    if (value.is_string()) {
        bool ok = false;
        const double parsed = str::toDouble(str::trimmed(value.get<std::string>()), &ok);
        if (ok)
            return parsed;
    }
    return fallback;
}

bool jsonBool(const Json &obj, std::string_view key, bool fallback)
{
    const Json value = jsonValue(obj, key);
    if (value.is_boolean())
        return value.get<bool>();
    return fallback;
}

std::string jsonScalarText(const Json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer())
        return str::number(value.get<long long>());
    if (value.is_number_float())
        return str::number(value.get<double>());
    return {};
}

} // namespace phicore::hue::ipc
