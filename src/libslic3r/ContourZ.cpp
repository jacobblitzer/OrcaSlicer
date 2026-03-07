#include "ContourZ.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "SLA/IndexedMesh.hpp"
#include "libslic3r.h"

#include <cmath>

namespace Slic3r {

// Subdivision step size in mm for sampling points along extrusion paths.
static constexpr double SUBDIVISION_STEP_MM = 0.1;

// Cast rays up and down from a point to find the nearest mesh surface Z.
// Returns the Z coordinate of the nearest surface hit, or NaN if no hit.
static float ray_cast_surface_z(const sla::IndexedMesh &mesh, double x, double y, double z)
{
    Vec3d origin(x, y, z);
    Vec3d dir_up(0, 0, 1);
    Vec3d dir_down(0, 0, -1);

    auto hit_up   = mesh.query_ray_hit(origin, dir_up);
    auto hit_down = mesh.query_ray_hit(origin, dir_down);

    bool up_valid   = hit_up.is_hit();
    bool down_valid = hit_down.is_hit();

    if (up_valid && down_valid) {
        // Return the closer surface
        if (hit_up.distance() <= hit_down.distance())
            return float(z + hit_up.distance());
        else
            return float(z - hit_down.distance());
    } else if (up_valid) {
        return float(z + hit_up.distance());
    } else if (down_valid) {
        return float(z - hit_down.distance());
    }

    return std::numeric_limits<float>::quiet_NaN();
}

// For external perimeters, compute a slope-based reduction factor.
// Surfaces steeper than the threshold angle get their Z adjustment reduced.
// Returns a factor in [0, 1] where 1 means full adjustment, 0 means no adjustment.
static float perimeter_slope_factor(
    const sla::IndexedMesh &mesh, double x, double y, double z,
    float threshold_deg)
{
    if (threshold_deg <= 0.f)
        return 0.f;
    if (threshold_deg >= 90.f)
        return 1.f;

    Vec3d origin(x, y, z);
    Vec3d dir_up(0, 0, 1);
    Vec3d dir_down(0, 0, -1);

    auto hit_up   = mesh.query_ray_hit(origin, dir_up);
    auto hit_down = mesh.query_ray_hit(origin, dir_down);

    // Pick the closer hit
    sla::IndexedMesh::hit_result hit(sla::IndexedMesh::hit_result::infty());
    if (hit_up.is_hit() && hit_down.is_hit())
        hit = (hit_up.distance() <= hit_down.distance()) ? hit_up : hit_down;
    else if (hit_up.is_hit())
        hit = hit_up;
    else if (hit_down.is_hit())
        hit = hit_down;
    else
        return 0.f;

    // Get the surface normal at the hit point
    Vec3d normal = hit.normal();
    // Compute angle between normal and vertical (Z axis)
    double cos_angle = std::abs(normal.z()) / normal.norm();
    double angle_deg = std::acos(std::clamp(cos_angle, 0.0, 1.0)) * 180.0 / M_PI;

    // angle_deg is the angle from vertical:
    //   0 deg = flat horizontal surface (normal points up) -> full adjustment
    //  90 deg = vertical wall (normal points sideways) -> no adjustment
    // We want to reduce adjustment when the surface is steeper than threshold.
    if (angle_deg <= threshold_deg)
        return 1.f;
    // Smooth falloff from threshold to 90 degrees
    float t = float((angle_deg - threshold_deg) / (90.0 - threshold_deg));
    return std::max(0.f, 1.f - t);
}

// Subdivide a polyline segment into steps of approximately SUBDIVISION_STEP_MM.
// Returns a list of points (in mm, unscaled) including start and end.
static std::vector<Vec3d> subdivide_segment(const Vec3d &a, const Vec3d &b)
{
    double len = (b - a).head<2>().norm(); // 2D distance in XY
    int n_steps = std::max(1, int(std::ceil(len / SUBDIVISION_STEP_MM)));
    std::vector<Vec3d> pts;
    pts.reserve(n_steps + 1);
    for (int i = 0; i <= n_steps; ++i) {
        double t = double(i) / double(n_steps);
        pts.push_back(a + t * (b - a));
    }
    return pts;
}

// Process a single ExtrusionPath, applying Z contouring.
static void contour_z_path(
    ExtrusionPath           &path,
    const sla::IndexedMesh  &mesh,
    float                    layer_print_z,
    float                    layer_bottom_z,
    float                    min_z,
    float                    perimeter_slope_threshold_deg)
{
    // Only process eligible extrusion roles
    ExtrusionRole role = path.role();
    if (role != erTopSolidInfill &&
        role != erIroning &&
        role != erExternalPerimeter &&
        role != erPerimeter)
        return;

    const Polyline &poly = path.polyline;
    if (poly.points.size() < 2)
        return;

    bool is_perimeter = (role == erExternalPerimeter || role == erPerimeter);
    float floor_z = layer_bottom_z + min_z;

    Points3 contour_pts;
    contour_pts.reserve(poly.points.size() * 2); // rough estimate after subdivision

    // Process each segment of the polyline
    for (size_t i = 0; i + 1 < poly.points.size(); ++i) {
        const Point &p0 = poly.points[i];
        const Point &p1 = poly.points[i + 1];

        // Convert to unscaled mm coordinates
        double x0 = unscale<double>(p0.x());
        double y0 = unscale<double>(p0.y());
        double x1 = unscale<double>(p1.x());
        double y1 = unscale<double>(p1.y());

        // Subdivide this segment
        Vec3d a(x0, y0, layer_print_z);
        Vec3d b(x1, y1, layer_print_z);
        auto sub_pts = subdivide_segment(a, b);

        // For the first segment, include the first point; otherwise skip it
        // (it was the last point of the previous segment).
        size_t start_idx = (i == 0) ? 0 : 1;
        for (size_t j = start_idx; j < sub_pts.size(); ++j) {
            const Vec3d &pt = sub_pts[j];

            // Ray-cast to find nearest surface Z
            float surface_z = ray_cast_surface_z(mesh, pt.x(), pt.y(), layer_print_z);

            float contour_z = layer_print_z;
            if (!std::isnan(surface_z)) {
                // Compute the offset: we want to follow the surface down (never up)
                float z_offset = surface_z - layer_print_z;
                if (z_offset > 0.f)
                    z_offset = 0.f; // Never raise Z above layer_print_z

                if (is_perimeter) {
                    // Apply slope-based reduction for perimeters
                    float factor = perimeter_slope_factor(mesh, pt.x(), pt.y(),
                        layer_print_z, perimeter_slope_threshold_deg);
                    z_offset *= factor;
                }

                contour_z = layer_print_z + z_offset;
            }

            // Clamp Z to valid range
            contour_z = std::max(floor_z, std::min(contour_z, layer_print_z));

            // Store as scaled coordinates
            contour_pts.emplace_back(
                coord_t(scale_(pt.x())),
                coord_t(scale_(pt.y())),
                coord_t(scale_(contour_z))
            );
        }
    }

    // Only mark as contoured if we actually produced contour points
    if (!contour_pts.empty()) {
        path.z_contoured = true;
        path.contour_polyline.points = std::move(contour_pts);
    }
}

// Recursively process an ExtrusionEntityCollection.
void contour_z_entity_collection(
    ExtrusionEntityCollection &collection,
    const sla::IndexedMesh    &mesh,
    float                      layer_print_z,
    float                      layer_bottom_z,
    float                      min_z,
    float                      perimeter_slope_threshold_deg)
{
    for (ExtrusionEntity *entity : collection.entities) {
        if (auto *path = dynamic_cast<ExtrusionPathSloped *>(entity)) {
            // Skip sloped paths (scarf joints already handle Z)
            continue;
        } else if (auto *path = dynamic_cast<ExtrusionPath *>(entity)) {
            contour_z_path(*path, mesh, layer_print_z, layer_bottom_z,
                min_z, perimeter_slope_threshold_deg);
        } else if (auto *multipath = dynamic_cast<ExtrusionMultiPath *>(entity)) {
            for (ExtrusionPath &p : multipath->paths) {
                contour_z_path(p, mesh, layer_print_z, layer_bottom_z,
                    min_z, perimeter_slope_threshold_deg);
            }
        } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity)) {
            for (ExtrusionPath &p : loop->paths) {
                contour_z_path(p, mesh, layer_print_z, layer_bottom_z,
                    min_z, perimeter_slope_threshold_deg);
            }
        } else if (auto *coll = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            contour_z_entity_collection(*coll, mesh, layer_print_z, layer_bottom_z,
                min_z, perimeter_slope_threshold_deg);
        }
    }
}

} // namespace Slic3r
