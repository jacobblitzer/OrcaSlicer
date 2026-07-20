#ifndef slic3r_PylonInjection_hpp_
#define slic3r_PylonInjection_hpp_

// Orca: pylon injection — descriptor for a single bottom-up injection event.
//
// A pylon injection event is scheduled at a specific (x, y, z_top) and tells
// the G-code emitter to: travel dry to (x, y), descend to z_bottom, then
// extrude bottom-up while ascending to z_top in step_height-sized segments,
// dispensing dE total filament length, then dwell for dwell_ms and resume.
//
// Events are transported through the existing CustomGCode::Item scheduler:
// the Event is JSON-serialised into Item::extra and the Item's print_z is
// set to z_top so it fires at the matching layer boundary.

#include "CustomGCode.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace PylonInjection {

struct Event
{
    int      pylon_id     {-1};   // unique within the print; -1 marks invalid
    double   x            {0.0};  // pylon center XY, world coordinates (mm)
    double   y            {0.0};
    double   z_bottom     {0.0};  // absolute Z, bottom of this event's fill range (mm)
    double   z_top        {0.0};  // absolute Z, top of this event's fill range; equals a Layer::print_z (mm)
    double   radius       {0.0};  // pylon radius (mm). Volume of this event = pi * r^2 * (z_top - z_bottom).
    double   dE           {0.0};  // total filament length to dispense across the ascent (mm)
    int      filament_id  {0};    // 0-based extruder/filament index for this event
    int      dwell_ms     {0};    // post-event G4 dwell (ms)
    double   descent_speed{0.0};  // mm/min for the dry travel down
    double   extrude_speed{0.0};  // mm/min for the ascending extrude segments
    double   step_height  {0.0};  // delta Z per ascending extrude segment (mm)
    double   helix_radius {0.0};  // XY radius of the helix path inside the void (mm).
                                  // Distinct from `radius` (the pylon's void radius). Scheduler
                                  // computes this as max(min_helix_radius, radius - pylon_helix_wall_offset).

    // Sanity check used by the emitter (Task 8).
    bool is_valid() const;
};

// JSON round-trip for transport through CustomGCode::Item::extra.
std::string to_json(const Event &e);
Event       from_json(const std::string &j);

// Convenience round-trip with CustomGCode::Item.
// to_item():   builds an Item with type=PylonInject, print_z=z_top, extruder=filament_id+1,
//              and extra=to_json(e).
// from_item(): expects item.type == PylonInject; reads extra as JSON. Returns an Event
//              with pylon_id == -1 on parse failure or wrong type.
CustomGCode::Item to_item(const Event &e);
Event             from_item(const CustomGCode::Item &item);

// Batched variant for layers that fire multiple pylon events at the same print_z.
// The existing assign_custom_gcodes pipeline consumes at most one CustomGCode::Item
// per layer, so the scheduler groups all per-layer events into one batched Item.
//   - to_item_batch(): all events share print_z (== events[0].z_top). extruder is
//     taken from events[0]; the batched JSON keeps each event's own filament_id.
//   - from_item_batch(): returns empty on parse failure or wrong type.
CustomGCode::Item       to_item_batch(const std::vector<Event> &events);
std::vector<Event>      from_item_batch(const CustomGCode::Item &item);
std::string             to_json_batch(const std::vector<Event> &events);
std::vector<Event>      from_json_batch(const std::string &j);

} // namespace PylonInjection
} // namespace Slic3r

#endif // slic3r_PylonInjection_hpp_
