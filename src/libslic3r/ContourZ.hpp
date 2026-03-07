#pragma once

#include "ExtrusionEntity.hpp"

namespace Slic3r {

namespace sla { class IndexedMesh; }

// Process an ExtrusionEntityCollection in-place, applying Z contouring
// to eligible extrusion paths by ray-casting against the mesh surface.
void contour_z_entity_collection(
    ExtrusionEntityCollection &collection,
    const sla::IndexedMesh    &mesh,
    float                      layer_print_z,
    float                      layer_bottom_z,
    float                      min_z,
    float                      perimeter_slope_threshold_deg);

} // namespace Slic3r
