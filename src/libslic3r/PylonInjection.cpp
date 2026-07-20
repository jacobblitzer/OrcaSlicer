#include "PylonInjection.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace PylonInjection {

bool Event::is_valid() const
{
    return pylon_id >= 0
        && radius > 0.0
        && z_top > z_bottom
        && step_height > 0.0
        && descent_speed > 0.0
        && extrude_speed > 0.0
        && filament_id >= 0
        && dwell_ms >= 0;
}

std::string to_json(const Event &e)
{
    nlohmann::json j;
    j["pylon_id"]      = e.pylon_id;
    j["x"]             = e.x;
    j["y"]             = e.y;
    j["z_bottom"]      = e.z_bottom;
    j["z_top"]         = e.z_top;
    j["radius"]        = e.radius;
    j["dE"]            = e.dE;
    j["filament_id"]   = e.filament_id;
    j["dwell_ms"]      = e.dwell_ms;
    j["descent_speed"] = e.descent_speed;
    j["extrude_speed"] = e.extrude_speed;
    j["step_height"]   = e.step_height;
    j["helix_radius"]  = e.helix_radius;
    return j.dump();
}

Event from_json(const std::string &s)
{
    Event e;
    try {
        const nlohmann::json j = nlohmann::json::parse(s);
        j.at("pylon_id"     ).get_to(e.pylon_id);
        j.at("x"            ).get_to(e.x);
        j.at("y"            ).get_to(e.y);
        j.at("z_bottom"     ).get_to(e.z_bottom);
        j.at("z_top"        ).get_to(e.z_top);
        j.at("radius"       ).get_to(e.radius);
        j.at("dE"           ).get_to(e.dE);
        j.at("filament_id"  ).get_to(e.filament_id);
        j.at("dwell_ms"     ).get_to(e.dwell_ms);
        j.at("descent_speed").get_to(e.descent_speed);
        j.at("extrude_speed").get_to(e.extrude_speed);
        j.at("step_height"  ).get_to(e.step_height);
        // helix_radius is post-V1 — older snapshots may not carry it. Tolerate absence.
        if (j.contains("helix_radius")) j.at("helix_radius").get_to(e.helix_radius);
    } catch (const nlohmann::json::exception &) {
        e.pylon_id = -1;  // signal parse failure
    }
    return e;
}

CustomGCode::Item to_item(const Event &e)
{
    CustomGCode::Item item;
    item.print_z  = e.z_top;
    item.type     = CustomGCode::PylonInject;
    item.extruder = e.filament_id + 1;  // 1-based per existing convention
    item.color    = "";
    item.extra    = to_json(e);
    return item;
}

Event from_item(const CustomGCode::Item &item)
{
    if (item.type != CustomGCode::PylonInject) {
        Event e;
        e.pylon_id = -1;
        return e;
    }
    return from_json(item.extra);
}

static nlohmann::json event_to_json_object(const Event &e)
{
    nlohmann::json j;
    j["pylon_id"]      = e.pylon_id;
    j["x"]             = e.x;
    j["y"]             = e.y;
    j["z_bottom"]      = e.z_bottom;
    j["z_top"]         = e.z_top;
    j["radius"]        = e.radius;
    j["dE"]            = e.dE;
    j["filament_id"]   = e.filament_id;
    j["dwell_ms"]      = e.dwell_ms;
    j["descent_speed"] = e.descent_speed;
    j["extrude_speed"] = e.extrude_speed;
    j["step_height"]   = e.step_height;
    j["helix_radius"]  = e.helix_radius;
    return j;
}

static Event event_from_json_object(const nlohmann::json &j)
{
    Event e;
    try {
        j.at("pylon_id"     ).get_to(e.pylon_id);
        j.at("x"            ).get_to(e.x);
        j.at("y"            ).get_to(e.y);
        j.at("z_bottom"     ).get_to(e.z_bottom);
        j.at("z_top"        ).get_to(e.z_top);
        j.at("radius"       ).get_to(e.radius);
        j.at("dE"           ).get_to(e.dE);
        j.at("filament_id"  ).get_to(e.filament_id);
        j.at("dwell_ms"     ).get_to(e.dwell_ms);
        j.at("descent_speed").get_to(e.descent_speed);
        j.at("extrude_speed").get_to(e.extrude_speed);
        j.at("step_height"  ).get_to(e.step_height);
        // helix_radius is post-V1 — older snapshots may not carry it. Tolerate absence.
        if (j.contains("helix_radius")) j.at("helix_radius").get_to(e.helix_radius);
    } catch (const nlohmann::json::exception &) {
        e.pylon_id = -1;
    }
    return e;
}

std::string to_json_batch(const std::vector<Event> &events)
{
    nlohmann::json j = nlohmann::json::array();
    for (const Event &e : events)
        j.push_back(event_to_json_object(e));
    return j.dump();
}

std::vector<Event> from_json_batch(const std::string &s)
{
    std::vector<Event> out;
    try {
        const nlohmann::json j = nlohmann::json::parse(s);
        if (!j.is_array())
            return out;
        out.reserve(j.size());
        for (const auto &je : j) {
            Event e = event_from_json_object(je);
            if (e.pylon_id >= 0)
                out.push_back(std::move(e));
        }
    } catch (const nlohmann::json::exception &) {
        out.clear();
    }
    return out;
}

CustomGCode::Item to_item_batch(const std::vector<Event> &events)
{
    CustomGCode::Item item;
    item.type = CustomGCode::PylonInject;
    if (events.empty()) {
        item.print_z = 0.0;
        item.extruder = 0;
        item.color = "";
        item.extra = "[]";
        return item;
    }
    item.print_z  = events.front().z_top;
    item.extruder = events.front().filament_id + 1;
    item.color    = "";
    item.extra    = to_json_batch(events);
    return item;
}

std::vector<Event> from_item_batch(const CustomGCode::Item &item)
{
    if (item.type != CustomGCode::PylonInject)
        return {};
    return from_json_batch(item.extra);
}

} // namespace PylonInjection
} // namespace Slic3r
