#include "hue_events.h"

namespace phicore::hue::ipc {

std::vector<Json> EventStreamParser::feed(std::string_view bytes)
{
    std::vector<Json> events;
    for (const char c : bytes) {
        if (c != '\n') {
            m_line.push_back(c);
            continue;
        }
        if (!m_line.empty() && m_line.back() == '\r')
            m_line.pop_back();
        if (m_line.empty()) {
            // An empty line ends the event.
            if (!m_data.empty()) {
                const Json parsed = parseJson(m_data);
                if (!parsed.is_null())
                    events.push_back(parsed);
                m_data.clear();
            }
            continue;
        }
        if (m_line.rfind("data:", 0) == 0) {
            std::string_view payload(m_line);
            payload.remove_prefix(5);
            if (!payload.empty() && payload.front() == ' ')
                payload.remove_prefix(1);
            if (!m_data.empty())
                m_data.push_back('\n');
            m_data.append(payload);
        }
        // `id:`, `event:`, `:keepalive` comments: not used.
        m_line.clear();
    }
    return events;
}

void EventStreamParser::reset()
{
    m_line.clear();
    m_data.clear();
}

std::vector<EventChange> changesIn(const Json &event)
{
    std::vector<EventChange> out;
    const auto addEvent = [&out](const Json &one) {
        if (!one.is_object())
            return;
        const std::string type = jsonString(one, "type");
        const Json data = jsonValue(one, "data");
        if (!data.is_array())
            return;
        for (const Json &resource : data) {
            if (!resource.is_object())
                continue;
            EventChange change;
            change.type = type;
            change.resourceType = jsonString(resource, "type");
            change.resource = resource;
            out.push_back(std::move(change));
        }
    };
    if (event.is_array()) {
        for (const Json &one : event)
            addEvent(one);
    } else {
        addEvent(event);
    }
    return out;
}

} // namespace phicore::hue::ipc
