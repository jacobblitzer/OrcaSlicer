# Pylon Injection — Touchpoints

> Living index of every file/line touched by the pylon-injection feature. Maintain by hand alongside each change. If you add a config key, a type-explicit accept list, an emitter branch, or a UI hook, add a row here.

Legend:
- `[code]` shipped feature code — must remain
- `[diag]` temporary diagnostic logging — **remove before merge to main**
- `[gap]` known untouched area that *probably* needs work — verify before claiming "done"

---

## 1. Configuration

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/PrintConfig.hpp` | PrintRegionConfig section | `pylon_enabled`, `pylon_injection_period`, `pylon_stagger_offset`, `pylon_injection_dwell_ms`, `pylon_descent_speed`, `pylon_extrude_speed`, `pylon_step_height` (7 keys) | Per-region settings, overridable on the pylon ModelVolume's config | `[code]` |
| `src/libslic3r/PrintConfig.hpp` | PrintObjectConfig section | `pylon_injection_filament`, `pylon_max_descent_depth` (2 keys) | Global per-process settings; live in PrintObjectConfig so they surface on TabPrint | `[code]` |
| `src/libslic3r/PrintConfig.cpp` | after `zaa_min_z` definitions | `this->add()` definitions for all 9 keys with label / tooltip / category / mode / default | Runtime config def | `[code]` |
| `src/libslic3r/Preset.cpp` | inside `s_Preset_print_options` | 9 entries appended | Without this, the print preset's config doesn't carry the keys → TabPrint crashes on lookup | `[code]` |

## 2. Model layer

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/Model.hpp` | `ModelVolumeType` enum | `PYLON_VOID` value appended (after `SUPPORT_ENFORCER`) | First-class type, distinct from `NEGATIVE_VOLUME` | `[code]` |
| `src/libslic3r/Model.hpp` | near other `is_*` helpers | `bool is_pylon() const` — checks `m_type == PYLON_VOID` OR legacy `NEGATIVE_VOLUME + pylon_enabled` flag | Backward-compat so old projects still detect pylons | `[code]` |
| `src/libslic3r/Model.hpp` | next line | `bool is_carving_volume() const` — true for `NEGATIVE_VOLUME` or `PYLON_VOID` | Convenience for slicer accept-list updates | `[code]` |
| `src/libslic3r/Model.cpp` | `type_from_string` | added `"pylon_void" → PYLON_VOID` | 3MF read path | `[code]` |
| `src/libslic3r/Model.cpp` | `type_to_string` | added `PYLON_VOID → "pylon_void"` | 3MF write path | `[code]` |

## 3. Slicer pipeline

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/Print.hpp` | `PrintObjectStep` enum | `posSchedulePylonInjection` value between `posContouring` and `posSupportMaterial` | New per-object step | `[code]` |
| `src/libslic3r/Print.hpp` | PrintObject member section | `std::vector<ExPolygons> m_pylon_footprints_per_layer` | Cache: per-layer pylon XY footprints intersected with `Layer::lslices` | `[code]` |
| `src/libslic3r/Print.hpp` | PrintObject public | `has_pylons()`, `compute_pylon_footprints()`, `schedule_pylon_injections()`, `pylon_footprints_at_layer(size_t)` declarations | Public API for pipeline + Hook B | `[code]` |
| `src/libslic3r/PrintObject.cpp` | ~lines 730–1130 | implementations of `has_pylons`, `compute_pylon_footprints`, `pylon_footprints_at_layer`, `schedule_pylon_injections` | Core feature logic | `[code]` |
| `src/libslic3r/PrintObject.cpp` | inside `PrintObject::infill()` | call to `compute_pylon_footprints()` BEFORE `Layer::make_fills` | So Hook B has fresh footprints | `[code]` |
| `src/libslic3r/PrintObject.cpp` | `compute_pylon_footprints` (~795 — degenerate bbox); `schedule_pylon_injections` (~857 — `pylon_max_descent_depth=0`, ~862 — auto-support enabled, ~900 — `e_per_mm3` unavailable, ~945 — degenerate `event_height`, ~978 — SUPPORT_ENFORCER overlap, ~992 — no floor below pylon, ~1009 — pylon top above print, ~1019 — degenerate Z range) | `BOOST_LOG_TRIVIAL(warning)` lines prefixed `pylon-injection:` reporting real user-setup problems that cause a pylon to be skipped | User-actionable warnings; intentionally kept after the V1 diag strip | `[code]` |
| `src/libslic3r/Print.cpp` | per-object pipeline driver | new loop calling `obj->schedule_pylon_injections()` between contouring and support_material loops | Schedule slot per the spec | `[code]` |
| `src/libslic3r/PrintBase.hpp` | near `const Model& model() const` | non-const `Model& model()` accessor | Scheduler needs to write to `m_model.plates_custom_gcodes` | `[code]` |
| `src/libslic3r/PrintApply.cpp` | `model_volume_solid_or_modifier` (line ~542) | added `PYLON_VOID` to the OR chain | Without this, `Print::apply()` filters pylons out of the slicer's m_model | `[code]` |
| `src/libslic3r/PrintApply.cpp` | line ~1015 path | replaced `volume.is_negative_volume()` → `volume.is_carving_volume()` so PYLON_VOID also gets the "carving" branch | Same root cause | `[code]` |
| `src/libslic3r/PrintApply.cpp` | line ~1394 `solid_or_modifier_types` `initializer_list<ModelVolumeType>` | added `PYLON_VOID` to the list | Used by `model_volume_list_changed` for change detection during re-apply | `[code]` |
| `src/libslic3r/PrintObjectSlice.cpp` | `model_volume_needs_slicing` (line ~128) | added `PYLON_VOID` to the OR chain | Otherwise the pylon mesh is never sliced | `[code]` |
| `src/libslic3r/PrintObjectSlice.cpp` | line ~182, ~417, ~424, ~428, ~430, ~603 | replaced `is_negative_volume()` with `is_carving_volume()` (with `/* Orca: NEGATIVE_VOLUME or PYLON_VOID */` annotation) | So PYLON_VOID participates in the same diff_ex carving as negative volumes | `[code]` |
| `src/libslic3r/Fill/Fill.cpp` | inside `Layer::make_fills` per-`SurfaceFill` loop | `if (is_internal && !is_bridge && !is_solid) { surface_fill.expolygons = diff_ex(surface_fill.expolygons, voids); … }` — Hook B | Carves sparse infill around the pylon | `[code]` |

## 4. Tool ordering / filament

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/GCode/ToolOrdering.hpp` | `LayerTools` class | `unsigned int pylon_filament(const PrintObjectConfig &, const PrintRegion &) const` declaration | Mirrors Jake's existing per-feature filament fork | `[code]` |
| `src/libslic3r/GCode/ToolOrdering.cpp` | after `bottom_surface_filament` definition | `pylon_filament` implementation: read `pylon_injection_filament`, fall back to `solid_infill_filament` | The resolver | `[code]` |

## 5. G-code emission

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/CustomGCode.hpp` | `enum Type` | `PylonInject` value inserted before `Unknown` sentinel | New per-Z event type | `[code]` |
| `src/libslic3r/CustomGCode.hpp` | `str2type` JSON map | `{"PylonInject", PylonInject}` row added | 3MF roundtrip via string | `[code]` |
| `src/libslic3r/PylonInjection.hpp` (NEW) | whole file | `struct Event` + `to_json/from_json/to_item/from_item/to_item_batch/from_item_batch/to_json_batch/from_json_batch` declarations | Event descriptor and CustomGCode::Item round-trip | `[code]` |
| `src/libslic3r/PylonInjection.cpp` (NEW) | whole file | Implementation of the above | | `[code]` |
| `src/libslic3r/GCode.cpp` | inside `emit_custom_gcode_per_print_z` (~line 4130–4232) | Early-branch on `CustomGCode::PylonInject` → parse batched JSON → for each event: optional set_extruder, retract, travel-to-xy, descend, unretract, ascending extrude segments, retract, G4 dwell, optional restore extruder. Block is bracketed by `;PYLON_INJECT_START` / `;PYLON_INJECT_END` comments | The actual synthesized injection block | `[code]` |
| `src/libslic3r/GCode.cpp` | inside emit per-event loop (~4137) | `BOOST_LOG_TRIVIAL(error) << "pylon-injection: dropping invalid event id=…"` before the `continue` | Real error — the scheduler should never produce invalid events; this catches it if it does | `[code]` |

## 6. G-code output stream

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/GCode.hpp` | `GCodeOutputStream` class | `void write(const std::string&)` declaration (no longer inline) | Force explicit-size overload | `[code]` |
| `src/libslic3r/GCode.cpp` | after `write(const char*)` | `void write(const std::string&)` definition: `fwrite(what.data(), 1, what.size(), this->f)` and `m_processor.process_buffer(what)` | Avoid `strlen()` truncation at the first embedded NUL | `[code]` |

## 7. GUI

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/slic3r/GUI/GUI_Factories.cpp` | line ~894 | `{ ModelVolumeType::PYLON_VOID, _L("Pylon") }` row in the type-label list | Right-click → Change Type shows "Pylon" label | `[code]` |
| `src/slic3r/GUI/GUI_Factories.cpp` | inside `append_menu_items_add_volume` (~line 712) | new `append_menu_item` "Add Pylon (cylinder)": calls `load_generic_subobject(L("Cylinder"), PYLON_VOID)`, then `mv->name="Pylon"`, re-centers volume, calls `plater()->update()` | Single-click pylon creation | `[code]` |
| `src/slic3r/GUI/GUI_Factories.cpp` | same lambda (~line 752) | `wxGetApp().obj_list()->update_name_for_items();` after `mv->name = "Pylon"` — refreshes the cached tree node name (so sidebar shows "Pylon" not "Generic-Cylinder") | Sidebar label fix | `[code]` |
| `src/slic3r/GUI/Tab.cpp` | inside `TabPrint::build()`'s Quality page (after `Z Contouring` group) | new `Pylon Injection` optgroup with 8 `append_single_option_line` calls | Surfaces pylon settings in Print Settings | `[code]` |
| `src/slic3r/GUI/MainFrame.cpp` | line 40 include + `generate_help_menu()` line ~2554 | `#include "PylonDebugSnapshot.hpp"` and one `append_menu_item` for "Export Pylon Debug Snapshot…" wired to `Slic3r::GUI::export_pylon_debug_snapshot(wxGetApp().mainframe)` | Debug snapshot entry point | `[code]` |
| `src/slic3r/GUI/PylonDebugSnapshot.hpp` | whole file | Declares `Slic3r::GUI::export_pylon_debug_snapshot(wxWindow*)`; header comment enumerates the dump set (see .cpp for the authoritative list) | Snapshot public API | `[code]` |
| `src/slic3r/GUI/PylonDebugSnapshot.cpp` | whole file | Implementation writing **10** files into a timestamped subfolder under a user-chosen parent: `01_environment.txt`, `02_print_config.ini`, `03_model_state__source.txt`, `03b_model_state__slicer.txt` (per-plate `Print::model`), `03c_model_state__legacy_plater_member.txt` (often stale, kept for compare), `04_plates_custom_gcodes__source.txt`, `04b_plates_custom_gcodes__slicer.txt`, `05_screenshot.png.skipped` (V2), `06_log_tail.txt` (last 5000 lines of newest `debug_*.log`), `07_pylon_status.txt` (source-model + slicer-side per-PrintObject `has_pylons` + per-layer footprint cache summary), `08_gcode_pylon_blocks.txt` (PYLON_INJECT_START..._END blocks ± 2 lines of context from the newest `.gcode` under temp `orcaslicer_model`) | The actual snapshot work | `[code]` |

## 8. Build

| File | Line(s) | What | Why |
|---|---|---|---|
| `src/libslic3r/CMakeLists.txt` | lines 363–364 | `PylonInjection.cpp`, `PylonInjection.hpp` added | Build the new translation unit into libslic3r | `[code]` |
| `src/slic3r/CMakeLists.txt` | lines 376–377 (`SLIC3R_GUI_SOURCES`) | `GUI/PylonDebugSnapshot.cpp`, `GUI/PylonDebugSnapshot.hpp` added | Build the snapshot module into libslic3r_gui | `[code]` |
| `tests/fff_print/CMakeLists.txt` | line 15 | `test_pylon_injection.cpp` added | Pylon Catch2 tests in the fff_print binary | `[code]` |
| `tests/fff_print/test_pylon_injection.cpp` (NEW) | whole file | 4 Catch2 cases tagged `[PylonInjection][.]` (markers, no-op cases, dE arithmetic) | Pylon test coverage; currently blocked by pre-existing `arrange_objects` bug in this fork's test harness | `[code]` |

## 9. Open gaps (verify before declaring done)

| Area | Status | Notes |
|---|---|---|
| 3MF read/write — does `pylon_void` survive a save → close → reopen? | unverified | `Format/3mf.cpp` and `Format/bbs_3mf.cpp` use `ModelVolume::type_to_string` indirectly. Should work but untested. | `[gap]` |
| Project auto-save / auto-restore | unverified | We've observed pylons "disappear" between rebuild restarts. May be the same code path as 3MF or it may be a separate snapshot mechanism. | `[gap]` |
| `take_snapshot` (undo/redo) interaction | unverified | Does adding a pylon get tracked in the undo stack? Probably yes via the `Plater::SingleSnapshot` inside `load_generic_subobject`, but `set_type` after-the-fact and `mv->name = "Pylon"` are NOT inside a snapshot. | `[gap]` |
| Multi-plate handling | unverified | Scheduler writes to `m_model.plates_custom_gcodes[curr_plate_index]`. If user has multiple plates and slices "all plates", does each plate get its own events? Not tested. | `[gap]` |
| Auto-support overlap | known V1 limitation | Pylon-vs-support check only validates user-declared `SUPPORT_ENFORCER` volumes; auto-generated overhang support overlap is logged-only. | `[gap]` |
| Wipe-tower / filament-change ordering | known V1 limitation | If `pylon_injection_filament` differs from current layer's filament, we emit a `set_extruder` directly without wipe-tower trip. Expect unpurged tool changes. | `[gap]` |
| `ModelVolume::get_extruders()` (`Model.cpp:2479`) | unverified | Currently returns empty for `INVALID/NEGATIVE_VOLUME/SUPPORT_BLOCKER/SUPPORT_ENFORCER`. `PYLON_VOID` falls through and tries to look up an extruder. May or may not cause issues. | `[gap]` |
| `GUI_ObjectList.cpp::load_generic_subobject` `Generic-` prefix at line ~2444 | works around it | We're using `update_name_for_items()` to refresh the tree, but the underlying volume DOES briefly carry the wrong name. Cosmetic only. | `[gap]` |

## 10. Branch / commit context

- Working branch: `feature/pylon-injection` (off `feature/custom-infill`)
- 11 V1 commits (already in branch): `772ccca…` (config keys) through `dce4d138…` (V1 docs)
- Subsequent fixes are uncommitted in working tree (sidebar fix, snapshot feature, type filters in PrintApply/PrintObjectSlice, etc.) — squash or commit logically before any merge.

## 11. Diagnostic-strip pass (pre-merge)

All `[diag]` rows that previously appeared in §3, §5, §6, §7 have been removed from the code in a single pass. What was stripped:

- Every `BOOST_LOG_TRIVIAL(info)` line prefixed `pylon-injection:` — code-path tracing (has_pylons inspecting volumes, compute_pylon_footprints per-volume bbox dump, scheduling event counts, GCode emit branch entered / events parsed / block produced, write(string) byte trace, TBB output-filter "PYLON_INJECT survives", GCodeProcessor.cpp `run_post_process` per-line trace, GUI_Factories.cpp Add Pylon menu state log + auto-set max_descent log).
- The dedicated emit-block dump in `GCode.cpp::emit_custom_gcode_per_print_z` that wrote `<data_dir>/log/pylon_emit_blocks.txt`. Snapshot file `09_pylon_emit_blocks.txt` was removed in lockstep — `08_gcode_pylon_blocks.txt` (extracted from the final `.gcode`) covers the same info.
- Dead local counters that only fed those logs (`pylon_volume_count`, `pylon_instance_count`, `footprint_stamps`, `layers_in_z`, `layers_with_footprint`, `skipped_floor`, `skipped_support`, `total_events`).
- Internal-state `BOOST_LOG_TRIVIAL(warning)` lines in the Add Pylon menu lambda that reported should-never-happen Orca paths (no object selected after `load_generic_subobject`, `model_object` null, etc.) — kept the bail-out `return`s.

What survived intentionally (and is now `[code]`):

- `BOOST_LOG_TRIVIAL(warning)` lines in `PrintObject.cpp` that report real user-setup problems for which the scheduler skips a pylon: degenerate bbox, `pylon_max_descent_depth=0`, auto-support enabled, `e_per_mm3` unavailable, degenerate `event_height`, SUPPORT_ENFORCER overlap, no floor below pylon, pylon top above the print, degenerate Z range.
- `BOOST_LOG_TRIVIAL(error)` in `GCode.cpp` emit per-event loop reporting an invalid event being dropped.

These are not plumbing diagnostics — they are warnings the user can act on (fix their model/preset). The legend in this doc still defines `[diag]` as "temporary diagnostic logging — remove before merge to main"; nothing currently in-tree matches that definition.
