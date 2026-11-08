#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PylonInjection.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <regex>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// Helper: count non-overlapping occurrences of needle in haystack.
static size_t count_substring(const std::string &haystack, const std::string &needle)
{
    if (needle.empty())
        return 0;
    size_t count = 0, pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// Helper: configure a print with one 20mm cube and (optionally) a pylon-enabled cylinder
// negative volume. Returns the produced G-code.
//
// If `add_pylon` is true, places a 6mm-dia, 14mm-tall cylinder centred near the cube's middle,
// lifted 2mm off the bed so at least one solid layer exists below the pylon bottom (floor safety).
static std::string slice_with_optional_pylon(
    double pylon_max_descent_depth,
    bool   add_pylon)
{
    Print print;
    Model model;
    Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, {
        { "layer_height",                   0.2 },
        { "first_layer_height",             0.2 },
        { "first_layer_extrusion_width",    0 },
        { "gcode_comments",                 true },
        { "start_gcode",                    "" },
        { "pylon_max_descent_depth",        pylon_max_descent_depth },
    });

    if (add_pylon) {
        REQUIRE(model.objects.size() == 1);
        ModelObject *mo = model.objects[0];

        TriangleMesh cylinder_mesh = make_cylinder(3.0 /*radius mm*/, 14.0 /*height mm*/);
        // make_cylinder is centred on its base at origin with height +Z. Lift bottom to z=2 so
        // there's at least one printed layer of cube below the pylon (floor-safety gate in scheduler).
        cylinder_mesh.translate(0.f, 0.f, 2.f);

        ModelVolume *mv = mo->add_volume(std::move(cylinder_mesh), ModelVolumeType::NEGATIVE_VOLUME);
        REQUIRE(mv != nullptr);
        mv->config.set("pylon_enabled",             true);
        mv->config.set("pylon_injection_period",    3);     // dense enough for the test cube
        mv->config.set("pylon_step_height",         0.4);
        mv->config.set("pylon_descent_speed",       300.0);
        mv->config.set("pylon_extrude_speed",       60.0);
        mv->config.set("pylon_injection_dwell_ms",  1000);

        // Re-apply with the model now carrying the pylon volume.
        DynamicPrintConfig cfg = print.full_print_config();
        print.apply(model, cfg);
        print.validate();
    }

    return Slic3r::Test::gcode(print);
}

TEST_CASE("Pylon injection: enabled pylon emits paired markers", "[PylonInjection][.]")
{
    const double max_descent = 4.0;
    std::string gcode = slice_with_optional_pylon(max_descent, /*add_pylon=*/true);
    REQUIRE_FALSE(gcode.empty());

    const size_t start_count = count_substring(gcode, ";PYLON_INJECT_START");
    const size_t end_count   = count_substring(gcode, ";PYLON_INJECT_END");

    INFO("PYLON_INJECT_START count: " << start_count);
    INFO("PYLON_INJECT_END   count: " << end_count);
    REQUIRE(start_count > 0);
    REQUIRE(start_count == end_count);
}

TEST_CASE("Pylon injection: zero max_descent_depth disables all events", "[PylonInjection][.]")
{
    std::string gcode = slice_with_optional_pylon(/*max_descent=*/0.0, /*add_pylon=*/true);
    REQUIRE_FALSE(gcode.empty());

    REQUIRE(count_substring(gcode, ";PYLON_INJECT_START") == 0);
    REQUIRE(count_substring(gcode, ";PYLON_INJECT_END")   == 0);
}

TEST_CASE("Pylon injection: no pylon volume produces no events", "[PylonInjection][.]")
{
    std::string gcode = slice_with_optional_pylon(/*max_descent=*/4.0, /*add_pylon=*/false);
    REQUIRE_FALSE(gcode.empty());

    REQUIRE(count_substring(gcode, ";PYLON_INJECT_START") == 0);
    REQUIRE(count_substring(gcode, ";PYLON_INJECT_END")   == 0);
}

TEST_CASE("Pylon injection: declared dE matches pi*r^2*h * e_per_mm3 within tolerance", "[PylonInjection][.]")
{
    // Re-run the basic positive case and parse each START marker's dE/radius/z_bottom/z_top.
    // For each event, recompute the expected dE = pi*r^2*(z_top - z_bottom) * e_per_mm3 and
    // confirm the declared dE matches within a tight relative tolerance. The volumetric
    // formula is the scheduler's; this test validates the scheduler's arithmetic, not the
    // emitter's per-segment splitting (which is covered indirectly by the markers test).
    const double max_descent = 4.0;
    std::string gcode = slice_with_optional_pylon(max_descent, /*add_pylon=*/true);
    REQUIRE_FALSE(gcode.empty());

    // Default test config uses 1.75mm filament and no per-filament flow override → ratio=1.
    const double diameter = 1.75;
    const double cross_section = M_PI * (diameter * 0.5) * (diameter * 0.5);
    const double e_per_mm3 = 1.0 / cross_section;

    std::regex re(R"(;PYLON_INJECT_START id=(\d+) x=([\-0-9.eE+]+) y=([\-0-9.eE+]+) z_bottom=([\-0-9.eE+]+) z_top=([\-0-9.eE+]+) dE=([\-0-9.eE+]+))");
    auto begin_it = std::sregex_iterator(gcode.begin(), gcode.end(), re);
    auto end_it   = std::sregex_iterator();

    size_t event_count = 0;
    for (auto it = begin_it; it != end_it; ++it) {
        const std::smatch &m = *it;
        const double z_bot     = std::stod(m[4].str());
        const double z_top     = std::stod(m[5].str());
        const double declared  = std::stod(m[6].str());
        const double radius    = 3.0;  // matches slice_with_optional_pylon's cylinder
        const double expected  = M_PI * radius * radius * (z_top - z_bot) * e_per_mm3;

        INFO("event " << event_count << " z=(" << z_bot << ".." << z_top << ") declared=" << declared
             << " expected=" << expected);
        REQUIRE_THAT(declared, Catch::Matchers::WithinRel(expected, 1e-3));
        ++event_count;
    }
    REQUIRE(event_count > 0);
}
