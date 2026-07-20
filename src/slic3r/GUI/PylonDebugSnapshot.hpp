#ifndef slic3r_PylonDebugSnapshot_hpp_
#define slic3r_PylonDebugSnapshot_hpp_

// Orca: pylon-injection debug snapshot.
//
// Triggered from Help → "Export Pylon Debug Snapshot...". Asks the user for a parent
// folder and writes a timestamped subfolder containing the dumps below. Captured set:
//
//   01_environment.txt                          build, commit, OS, wx, arch
//   02_print_config.ini                         full effective DynamicPrintConfig as key=value
//   03_model_state__source.txt                  Plater::model — what the GUI sees
//   03b_model_state__slicer.txt                 per-plate Print::model — what the slicer sees
//   03c_model_state__legacy_plater_member.txt   Plater::fff_print() — often stale, kept to diff
//   04_plates_custom_gcodes__source.txt         plates_custom_gcodes from the source Model
//   04b_plates_custom_gcodes__slicer.txt        plates_custom_gcodes from the slicer Model
//                                               (incl. expanded PylonInject Events)
//   05_screenshot.png.skipped                   placeholder; capture deferred to V2
//   06_log_tail.txt                             last 5000 lines of the newest debug_*.log
//   07_pylon_status.txt                         per-object has_pylons + per-layer footprint cache summary
//   08_gcode_pylon_blocks.txt                   PYLON_INJECT_START..._END blocks (±2 ctx lines) from
//                                               the newest .gcode under temp orcaslicer_model/
//
// Returns true if the user picked a folder and every file was written. Cancellation
// is a silent no-op (returns false, no error dialog). Real failures pop a single wxMessageBox.

class wxWindow;

namespace Slic3r { namespace GUI {

bool export_pylon_debug_snapshot(wxWindow *parent);

} } // namespace Slic3r::GUI

#endif // slic3r_PylonDebugSnapshot_hpp_
