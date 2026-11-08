#include "PylonDebugSnapshot.hpp"

#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "../slic3r/GUI/I18N.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PylonInjection.hpp"
#include "libslic3r/Utils.hpp"
// libslic3r_version.h is generated into the libslic3r build dir; including libslic3r.h pulls it.
#include "libslic3r/libslic3r.h"

#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/platinfo.h>
#include <wx/stdpaths.h>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Slic3r { namespace GUI {

// ---------- helpers ----------------------------------------------------------

static std::string type_label(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::INVALID:            return "INVALID";
    case ModelVolumeType::MODEL_PART:         return "MODEL_PART";
    case ModelVolumeType::NEGATIVE_VOLUME:    return "NEGATIVE_VOLUME";
    case ModelVolumeType::PARAMETER_MODIFIER: return "PARAMETER_MODIFIER";
    case ModelVolumeType::SUPPORT_BLOCKER:    return "SUPPORT_BLOCKER";
    case ModelVolumeType::SUPPORT_ENFORCER:   return "SUPPORT_ENFORCER";
    case ModelVolumeType::PYLON_VOID:         return "PYLON_VOID";
    }
    return "?";
}

static std::string custom_gcode_type_label(CustomGCode::Type t)
{
    switch (t) {
    case CustomGCode::ColorChange: return "ColorChange";
    case CustomGCode::PausePrint:  return "PausePrint";
    case CustomGCode::ToolChange:  return "ToolChange";
    case CustomGCode::Template:    return "Template";
    case CustomGCode::Custom:      return "Custom";
    case CustomGCode::PylonInject: return "PylonInject";
    case CustomGCode::Unknown:     return "Unknown";
    }
    return "?";
}

static FILE *open_for_write(const boost::filesystem::path &p)
{
    return boost::nowide::fopen(p.string().c_str(), "wb");
}

static bool write_str(const boost::filesystem::path &p, const std::string &s)
{
    FILE *f = open_for_write(p);
    if (f == nullptr) return false;
    const size_t n = ::fwrite(s.data(), 1, s.size(), f);
    ::fclose(f);
    return n == s.size();
}

static std::string timestamp_compact()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::ostringstream s;
    s << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return s.str();
}

static std::string serialize_expolygons_compact(const ExPolygons &eps)
{
    // Returns "count=N min=(x,y) max=(x,y)" — enough to know IF anything was stamped and roughly where.
    if (eps.empty()) return "count=0";
    BoundingBox bb;
    for (const ExPolygon &e : eps)
        bb.merge(e.contour.bounding_box());
    std::ostringstream s;
    s << "count=" << eps.size()
      << " bbox=(" << unscale<double>(bb.min.x()) << "," << unscale<double>(bb.min.y())
      << " .. " << unscale<double>(bb.max.x()) << "," << unscale<double>(bb.max.y()) << ")";
    return s.str();
}

// ---------- 01_environment.txt ----------------------------------------------

static std::string render_environment()
{
    std::ostringstream s;
    s << "OrcaSlicer pylon debug snapshot\n";
    s << "================================\n";
    s << "SoftFever_VERSION      = " << SoftFever_VERSION << "\n";
    s << "SLIC3R_VERSION         = " << SLIC3R_VERSION    << "\n";
    s << "SLIC3R_BUILD_ID        = " << SLIC3R_BUILD_ID   << "\n";
    s << "GIT_COMMIT_HASH        = " << GIT_COMMIT_HASH   << "\n";
    s << "\n";
    s << "Platform / OS\n";
    s << "-------------\n";
    const wxPlatformInfo pi = wxPlatformInfo::Get();
    s << "OS family       = " << pi.GetOperatingSystemFamilyName().ToStdString() << "\n";
    s << "OS name         = " << pi.GetOperatingSystemIdName().ToStdString()     << "\n";
    s << "OS version      = " << pi.GetOSMajorVersion() << "." << pi.GetOSMinorVersion() << "\n";
    s << "wxWidgets       = " << wxVERSION_NUM_DOT_STRING << "\n";
    s << "Architecture    = " << pi.GetBitnessName().ToStdString() << "\n";
    s << "Endianness      = " << pi.GetEndiannessName().ToStdString() << "\n";
    return s.str();
}

// ---------- 02_print_config.ini ---------------------------------------------

static std::string render_print_config(const DynamicPrintConfig &cfg)
{
    std::ostringstream s;
    s << "# Full effective DynamicPrintConfig snapshot (key = serialized value).\n\n";
    const auto keys = cfg.keys();
    for (const std::string &k : keys) {
        const ConfigOption *o = cfg.option(k);
        if (o == nullptr) continue;
        s << k << " = " << o->serialize() << "\n";
    }
    return s.str();
}

// ---------- 03_model_state.txt (source model = what the user edits) ---------

static std::string render_model_state(const Model &model, const char *label)
{
    std::ostringstream s;
    s << label << " — ModelObjects: " << model.objects.size() << "\n";
    s << "================================\n\n";
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject *mo = model.objects[oi];
        if (mo == nullptr) { s << "[" << oi << "] (null)\n\n"; continue; }
        s << "[" << oi << "] name='" << mo->name << "'"
          << "  volumes=" << mo->volumes.size()
          << "  instances=" << mo->instances.size()
          << "\n";
        for (size_t ii = 0; ii < mo->instances.size(); ++ii) {
            const ModelInstance *mi = mo->instances[ii];
            if (mi == nullptr) continue;
            const Vec3d off = mi->get_offset();
            s << "    instance[" << ii << "] world_offset=("
              << off.x() << "," << off.y() << "," << off.z() << ")\n";
        }
        for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
            const ModelVolume *mv = mo->volumes[vi];
            if (mv == nullptr) { s << "    volume[" << vi << "] (null)\n"; continue; }
            const BoundingBoxf3 lb = mv->mesh().bounding_box();
            const Vec3d voff = mv->get_offset();
            s << "    volume[" << vi << "] name='" << mv->name << "'"
              << " type=" << int(mv->type()) << " (" << type_label(mv->type()) << ")"
              << " is_pylon=" << (mv->is_pylon() ? "true" : "false")
              << " is_carving=" << (mv->is_carving_volume() ? "true" : "false")
              << "\n";
            s << "        local_bbox    min=(" << lb.min.x() << "," << lb.min.y() << "," << lb.min.z() << ")"
              << " max=(" << lb.max.x() << "," << lb.max.y() << "," << lb.max.z() << ")\n";
            s << "        volume_offset (" << voff.x() << "," << voff.y() << "," << voff.z() << ")\n";
            // World bbox of this volume's first instance (most informative for placement debugging).
            if (!mo->instances.empty() && mo->instances.front() != nullptr) {
                const Transform3d trafo = mo->instances.front()->get_matrix() * mv->get_matrix();
                const BoundingBoxf3 wb = lb.transformed(trafo);
                s << "        world_bbox    min=(" << wb.min.x() << "," << wb.min.y() << "," << wb.min.z() << ")"
                  << " max=(" << wb.max.x() << "," << wb.max.y() << "," << wb.max.z() << ")\n";
            }
            const auto keys = mv->config.keys();
            std::vector<std::string> pkeys;
            for (const auto &k : keys)
                if (k.find("pylon_") == 0) pkeys.push_back(k);
            if (!pkeys.empty()) {
                s << "        pylon_* overrides:\n";
                for (const auto &k : pkeys) {
                    const ConfigOption *o = mv->config.option(k);
                    if (o != nullptr) s << "            " << k << " = " << o->serialize() << "\n";
                }
            }
        }
        s << "\n";
    }
    return s.str();
}

// ---------- 04_plates_custom_gcodes.txt -------------------------------------

static std::string render_plates_custom_gcodes(const Model &model, const char *label)
{
    std::ostringstream s;
    s << label << " — plates_custom_gcodes entries: " << model.plates_custom_gcodes.size()
      << "  curr_plate_index=" << model.curr_plate_index << "\n";
    s << "================================\n\n";
    for (const auto &kv : model.plates_custom_gcodes) {
        const int plate = kv.first;
        const CustomGCode::Info &info = kv.second;
        s << "[plate " << plate << "]  mode=" << int(info.mode) << "  items=" << info.gcodes.size() << "\n";
        for (size_t i = 0; i < info.gcodes.size(); ++i) {
            const CustomGCode::Item &it = info.gcodes[i];
            s << "  item[" << i << "]"
              << " type=" << custom_gcode_type_label(it.type)
              << " print_z=" << it.print_z
              << " extruder=" << it.extruder
              << " color='" << it.color << "'"
              << " extra.size=" << it.extra.size()
              << "\n";
            if (it.type == CustomGCode::PylonInject) {
                const std::vector<PylonInjection::Event> events = PylonInjection::from_item_batch(it);
                s << "    PylonInject events: " << events.size() << "\n";
                for (size_t ei = 0; ei < events.size(); ++ei) {
                    const PylonInjection::Event &ev = events[ei];
                    s << "      [" << ei << "] id=" << ev.pylon_id
                      << " xy=(" << ev.x << "," << ev.y << ")"
                      << " z=" << ev.z_bottom << ".." << ev.z_top
                      << " r=" << ev.radius
                      << " dE=" << ev.dE
                      << " fil=" << ev.filament_id
                      << " dwell_ms=" << ev.dwell_ms
                      << " descent_speed=" << ev.descent_speed
                      << " extrude_speed=" << ev.extrude_speed
                      << " step_h=" << ev.step_height
                      << "\n";
                }
            }
        }
        s << "\n";
    }
    return s.str();
}

// ---------- 08_gcode_pylon_blocks.txt ---------------------------------------
//
// Walk the most recent gcode file in the orcaslicer_model temp tree and extract
// every ;PYLON_INJECT_START..._END block (with ±2 lines of context) so we can
// see exactly what the slicer wrote — including the helix G1 lines that the
// gcode previewer should be rendering.

static fs::path find_newest_gcode()
{
    const fs::path temp_root = fs::path(boost::filesystem::temp_directory_path()) / "orcaslicer_model";
    if (!fs::exists(temp_root)) return {};
    fs::path newest;
    std::time_t newest_t = 0;
    try {
        for (fs::recursive_directory_iterator it(temp_root), end; it != end; ++it) {
            if (!fs::is_regular_file(it->status())) continue;
            if (it->path().extension() != ".gcode") continue;
            const std::time_t t = fs::last_write_time(it->path());
            if (t > newest_t) { newest_t = t; newest = it->path(); }
        }
    } catch (const std::exception &) { /* permissions etc — fall through */ }
    return newest;
}

static std::string render_gcode_pylon_blocks()
{
    const fs::path gc = find_newest_gcode();
    std::ostringstream s;
    if (gc.empty()) {
        s << "no .gcode file under " << (fs::path(boost::filesystem::temp_directory_path()) / "orcaslicer_model").string()
          << "\n(if you've never sliced in this session, click Slice first and re-run the snapshot.)\n";
        return s.str();
    }
    s << "# inspecting newest gcode: " << gc.string() << "\n";
    s << "# size = " << fs::file_size(gc) << " bytes\n\n";

    boost::nowide::ifstream in(gc.string(), std::ios::in);
    if (!in) { s << "could not open " << gc.string() << "\n"; return s.str(); }

    std::deque<std::string> tail; // 2-line lookbehind for context
    std::string line;
    bool in_block = false;
    int total_blocks = 0;
    int lines_in_block = 0;
    while (std::getline(in, line)) {
        if (!in_block) {
            tail.push_back(line);
            if (tail.size() > 2) tail.pop_front();
        }
        if (!in_block && line.find("PYLON_INJECT_START") != std::string::npos) {
            ++total_blocks;
            s << "\n=================== PYLON BLOCK #" << total_blocks << " ===================\n";
            for (const std::string &t : tail) s << "  [ctx] " << t << "\n";
            s << "  [>>>] " << line << "\n";
            in_block = true; lines_in_block = 1;
            continue;
        }
        if (in_block) {
            s << "  [   ] " << line << "\n";
            ++lines_in_block;
            if (line.find("PYLON_INJECT_END") != std::string::npos) {
                s << "  (block had " << lines_in_block << " lines)\n";
                in_block = false; lines_in_block = 0;
                tail.clear();
            }
        }
    }
    s << "\n# total ;PYLON_INJECT_START blocks found: " << total_blocks << "\n";
    return s.str();
}

// ---------- 06_log_tail.txt --------------------------------------------------

static std::string render_log_tail(size_t tail_lines = 5000)
{
    namespace fs = boost::filesystem;
    const fs::path log_dir = fs::path(Slic3r::data_dir()) / "log";
    if (!fs::exists(log_dir) || !fs::is_directory(log_dir))
        return std::string("no log directory at ") + log_dir.string() + "\n";

    fs::path newest;
    std::time_t newest_t = 0;
    try {
        for (fs::directory_iterator it(log_dir); it != fs::directory_iterator(); ++it) {
            if (!fs::is_regular_file(it->status())) continue;
            const std::string fn = it->path().filename().string();
            if (fn.rfind("debug_", 0) != 0) continue;
            const std::time_t t = fs::last_write_time(it->path());
            if (t > newest_t) { newest_t = t; newest = it->path(); }
        }
    } catch (const std::exception &e) {
        return std::string("error scanning log directory: ") + e.what() + "\n";
    }
    if (newest.empty())
        return std::string("no debug_*.log file found in ") + log_dir.string() + "\n";

    boost::nowide::ifstream in(newest.string(), std::ios::in);
    if (!in) return std::string("could not open ") + newest.string() + "\n";

    std::deque<std::string> ring;
    std::string line;
    while (std::getline(in, line)) {
        ring.push_back(std::move(line));
        if (ring.size() > tail_lines) ring.pop_front();
    }
    std::ostringstream s;
    s << "# last " << ring.size() << " lines of " << newest.string() << "\n\n";
    for (const auto &l : ring) s << l << "\n";
    return s.str();
}

// ---------- 07_pylon_status.txt ---------------------------------------------

static std::string render_pylon_status(const Model &source_model, const Print *print)
{
    std::ostringstream s;
    size_t total_pylons = 0;
    s << "Pylon detection summary (source model — what the GUI sees)\n";
    s << "==========================================================\n\n";
    for (size_t oi = 0; oi < source_model.objects.size(); ++oi) {
        const ModelObject *mo = source_model.objects[oi];
        if (mo == nullptr) continue;
        size_t obj_pylons = 0;
        for (const ModelVolume *mv : mo->volumes)
            if (mv != nullptr && mv->is_pylon()) { ++obj_pylons; ++total_pylons; }
        s << "object[" << oi << "] '" << mo->name << "': pylon volumes = " << obj_pylons << "\n";
    }
    s << "\nTotal pylon volumes in source model: " << total_pylons << "\n\n";

    if (print == nullptr) {
        s << "(No Print object available — has slicing started?)\n";
        return s.str();
    }

    s << "Slicer-internal state (Print::fff_print() — what the slicer sees)\n";
    s << "==================================================================\n\n";
    auto print_objects = print->objects();
    s << "PrintObjects: " << print_objects.size() << "\n";
    for (size_t oi = 0; oi < print_objects.size(); ++oi) {
        const PrintObject *po = print_objects[oi];
        if (po == nullptr) { s << "[" << oi << "] (null)\n"; continue; }
        const ModelObject *mo = po->model_object();
        s << "[" << oi << "] model_object='" << (mo ? mo->name : std::string("(null)")) << "'"
          << " has_pylons=" << (po->has_pylons() ? "true" : "false")
          << " layers=" << po->layer_count()
          << "\n";

        // Skip reasons — populated each time schedule_pylon_injections() ran. Empty means
        // every detected pylon was scheduled (or schedule hasn't run yet for this state).
        const std::vector<std::string> &skips = po->pylon_skip_reasons();
        if (!skips.empty()) {
            s << "    pylons that DID NOT get a helix this run (" << skips.size() << "):\n";
            for (const std::string &r : skips)
                s << "      - " << r << "\n";
        } else if (po->has_pylons()) {
            s << "    pylons that did not get a helix: none (all scheduled, or schedule not yet run)\n";
        }
        if (po->has_pylons() && po->layer_count() > 0) {
            // Summarise footprint cache per-layer.
            size_t layers_with_footprint = 0;
            for (size_t li = 0; li < po->layer_count(); ++li) {
                const ExPolygons &fp = po->pylon_footprints_at_layer(li);
                if (!fp.empty()) ++layers_with_footprint;
            }
            s << "    pylon footprint cache: " << layers_with_footprint
              << " / " << po->layer_count() << " layers carry a footprint\n";
            // Dump per-layer for the first 8 non-empty layers (don't drown the file).
            size_t shown = 0;
            for (size_t li = 0; li < po->layer_count() && shown < 8; ++li) {
                const ExPolygons &fp = po->pylon_footprints_at_layer(li);
                if (fp.empty()) continue;
                ++shown;
                s << "      layer[" << li << "] " << serialize_expolygons_compact(fp) << "\n";
            }
            if (shown == 0)
                s << "      (no layer has a pylon footprint — the pylon volume doesn't geometrically overlap any layer's printed area)\n";
        }
    }
    return s.str();
}

// ---------- entry point ------------------------------------------------------

bool export_pylon_debug_snapshot(wxWindow *parent)
{
    namespace fs = boost::filesystem;

    // Default parent dir: the user's Desktop. Within it, create a per-invocation subfolder.
    const wxString desktop = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Desktop);
    wxDirDialog dlg(parent,
                    _L("Choose a parent folder; a timestamped subfolder will be created inside it"),
                    desktop,
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return false;  // silent cancel

    const fs::path parent_dir = fs::path(dlg.GetPath().ToStdString());
    const std::string subname = std::string("pylon_snapshot_") + timestamp_compact();
    const fs::path out_dir = parent_dir / subname;
    try {
        fs::create_directories(out_dir);
    } catch (const std::exception &e) {
        wxMessageBox(wxString::FromUTF8(std::string("Cannot create snapshot folder: ") + e.what()),
                     _L("Pylon Debug Snapshot"), wxOK | wxICON_ERROR, parent);
        return false;
    }

    GUI_App &app = wxGetApp();
    Plater *plater = app.plater();
    if (plater == nullptr) {
        wxMessageBox(_L("No plater available."), _L("Pylon Debug Snapshot"),
                     wxOK | wxICON_ERROR, parent);
        return false;
    }

    const Model &source_model = plater->model();
    // Plater::fff_print() returns a stale member that's not what background_process actually slices —
    // PartPlate.cpp reassigns the slicer to the per-plate m_print when that plate becomes current.
    // Pull the per-plate Print so the snapshot reflects what was actually sliced.
    const Print &print = plater->get_partplate_list().get_current_fff_print();
    const Print &print_legacy_member = plater->fff_print();

    DynamicPrintConfig print_config;
    if (app.preset_bundle != nullptr)
        print_config = app.preset_bundle->full_config();

    std::vector<std::string> failed;
    auto wr = [&](const char *name, const std::string &content) {
        if (!write_str(out_dir / name, content)) failed.emplace_back(name);
    };

    wr("01_environment.txt",            render_environment());
    wr("02_print_config.ini",           render_print_config(print_config));
    wr("03_model_state__source.txt",    render_model_state(source_model, "SOURCE MODEL (Plater::model)"));
    wr("03b_model_state__slicer.txt",
        render_model_state(print.model(), "SLICER MODEL (PartPlateList::get_current_fff_print().model)"));
    wr("03c_model_state__legacy_plater_member.txt",
        render_model_state(print_legacy_member.model(),
            "LEGACY Plater::fff_print() member (often stale — kept for diagnostic comparison)"));
    wr("04_plates_custom_gcodes__source.txt",
        render_plates_custom_gcodes(source_model, "SOURCE MODEL (Plater::model)"));
    wr("04b_plates_custom_gcodes__slicer.txt",
        render_plates_custom_gcodes(print.model(),
            "SLICER MODEL (per-plate Print::model — this is where the scheduler writes events)"));
    wr("05_screenshot.png.skipped",
        "Screenshot capture deferred (V2). Use Win+Shift+S to capture the 3D view manually if needed.\n");
    wr("06_log_tail.txt",               render_log_tail());
    wr("07_pylon_status.txt",           render_pylon_status(source_model, &print));
    wr("08_gcode_pylon_blocks.txt",     render_gcode_pylon_blocks());

    if (!failed.empty()) {
        std::string msg = "Some snapshot files failed to write:\n  - " + boost::algorithm::join(failed, "\n  - ")
                        + "\n\nFolder: " + out_dir.string();
        wxMessageBox(wxString::FromUTF8(msg), _L("Pylon Debug Snapshot"),
                     wxOK | wxICON_WARNING, parent);
        return false;
    }

    std::string ok_msg = "Pylon debug snapshot written to:\n\n" + out_dir.string()
                       + "\n\nIf the scheduler hasn't run yet for the current state, "
                         "click Slice first and re-run this command.";
    wxMessageBox(wxString::FromUTF8(ok_msg), _L("Pylon Debug Snapshot"),
                 wxOK | wxICON_INFORMATION, parent);
    return true;
}

} } // namespace Slic3r::GUI
