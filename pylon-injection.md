# Pylon Injection (V1)

VoxelFill-style cylindrical pylon voids carved out of sparse infill and filled by
periodic bottom-up extrusion events at layer boundaries. Manual placement,
cylinder-only, single dedicated reinforcement filament for the whole print,
layer-aligned events with brick-bond stagger.

## TL;DR

1. **Configure the global safety key first.** Set `pylon_max_descent_depth` to your
   measured nozzle clearance below the toolhead, **minus a safety margin**.
   With the default value of **0**, nothing happens — this is the safe default
   that prevents nozzle crashes on prints sliced before measuring clearance.
2. Add an object. Right-click → **Add Pylon (cylinder)**. A default 10mm-dia /
   20mm-tall cylinder is added as a `NEGATIVE_VOLUME` with `pylon_enabled = true`.
3. Position and scale the cylinder where you want the reinforced column.
   The cylinder's XY footprint is carved out of the parent object's sparse infill;
   top/bottom solid layers continue to seal it as caps.
4. (Optional) Tweak `pylon_injection_period`, `pylon_stagger_offset`,
   `pylon_step_height`, `pylon_descent_speed`, `pylon_extrude_speed`,
   `pylon_injection_dwell_ms` on the volume in the modifier-settings panel.
5. (Optional) Set `pylon_injection_filament` to a dedicated reinforcement filament
   (e.g. carbon-fiber PA-CF). Default 0 uses `solid_infill_filament`.
6. Slice. In the G-code preview, search for `;PYLON_INJECT_START` markers to
   confirm events fired.

## User workflow

```
GUI: object right-click menu
  → Add Pylon (cylinder)
     ⇩ creates a 10×20mm NEGATIVE_VOLUME on the selected object,
       flips pylon_enabled = true on its config
  → reposition / rescale the cylinder
  → tweak pylon_* settings on the volume (modifier-volume panel)

Print Settings (global)
  → pylon_max_descent_depth: SET THIS
  → pylon_injection_filament: optional

Slice → preview → look for ;PYLON_INJECT_START in G-code
```

## Configuration keys

### Per-region (per-volume override; live on `PrintRegionConfig`)

| Key                          | Type   | Default | Meaning                                                                                                                                                          |
|------------------------------|--------|---------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `pylon_enabled`              | bool   | false   | Marks a `NEGATIVE_VOLUME` as a pylon. Only effective when the volume's type is `Negative`. The "Add Pylon (cylinder)" menu sets this for you.                    |
| `pylon_injection_period`     | int    | 20      | Minimum number of layers between successive injection events for the same pylon. Higher = more cooling time between events.                                      |
| `pylon_stagger_offset`       | int    | 10      | Reserved for V2 (multi-color brick-bond). V1 uses a fixed 2-coloring with half-event-height Z stagger between alternating pylons; this key is currently unused. |
| `pylon_injection_dwell_ms`   | int    | 2000    | Milliseconds the toolhead dwells after each injection event so the deposited plug can solidify before normal printing resumes. Emitted as `G4 P####`.            |
| `pylon_descent_speed`        | float  | 300     | mm/min for the dry descent into the pylon void (no extrusion).                                                                                                   |
| `pylon_extrude_speed`        | float  | 60      | mm/min for the ascending extrusion segments inside the void. Lower = less back-pressure; better bonding to cooled walls.                                         |
| `pylon_step_height`          | float  | 0.4     | Δz per ascending extrude segment (mm). Smaller = finer control; larger = less G-code volume.                                                                     |

### Global (per-print; live on `PrintConfig`)

| Key                          | Type   | Default | Meaning                                                                                                                                                                                                                              |
|------------------------------|--------|---------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `pylon_injection_filament`   | int    | 0       | 1-based filament index used for every pylon event. `0` (default) means use `solid_infill_filament`. Currently V1 uses the same filament for all pylons in the print.                                                                |
| `pylon_max_descent_depth`    | float  | 0       | **Safety key.** Maximum vertical distance, in mm, the nozzle may descend below the current top print Z to reach an injection event's z_bottom. The scheduler clamps each event's height to this value (step-aligned). **`0` disables all injection.** |

## Safety: `pylon_max_descent_depth`

This key is the headline safety mechanism. If it is `0`, the scheduler emits a
warning to the log and skips all pylon events — the print slices and prints as
if pylons were ordinary negative volumes (i.e., you get unfilled cylindrical
holes).

This is the safe default for prints sliced before you've measured your nozzle's
true clearance, or for shared profiles where the safe value varies by machine.

**To enable injection: measure your nozzle clearance below the toolhead (the
distance you can descend before hitting any other hardware: fan shrouds, ABL
probes, brushes, etc.), subtract a generous safety margin (5mm+), and set
`pylon_max_descent_depth` to that value.** Each pylon event will then descend
at most that far below the current top print Z.

## How it works (sketch)

1. **Slicing.** Pylon negative volumes are sliced through the normal pipeline
   (`PrintObject::slice_volumes()`), carving cylindrical holes from the sparse
   infill. The XY footprint is intersected with `Layer::lslices` per Z so the
   footprint is never larger than the actual model at that height.
2. **Footprint subtraction.** During `Layer::make_fills`, for any sparse internal
   surface (`stInternal`, not bridge, not solid), the per-layer pylon footprint
   is `diff_ex`-subtracted from the surface region before pattern generation.
   Top/bottom and solid surfaces are deliberately left alone so they cap the
   pylon.
3. **Scheduling.** A new pipeline step (`posSchedulePylonInjection`, between
   `posInfill` and `posSupportMaterial`) walks each pylon volume × instance,
   computes Z-spaced events using `pylon_max_descent_depth` as the
   step-aligned event height, applies brick-bond 2-coloring stagger, snaps each
   event to a layer print_z, computes `dE` from `π·r²·h·e_per_mm3`, and writes
   batched `CustomGCode::Item` (type `PylonInject`) entries onto the print's
   `plates_custom_gcodes[curr_plate_index]`.
4. **Emission.** At each affected layer's emission, the new handler in
   `emit_custom_gcode_per_print_z` parses the JSON batch and synthesises the
   per-event block: optional tool change → retract → travel to (x,y) → descend
   to z_bottom → unretract → ascending extrude segments to z_top → retract →
   `G4 P<dwell_ms>` → optional tool change back. All moves go through
   `GCodeWriter` so its cached position, retract state, and feedrate stay
   coherent with the surrounding normal printing.

## G-code markers

Each event is wrapped:

```gcode
;PYLON_INJECT_START id=<n> x=<X> y=<Y> z_bottom=<Zb> z_top=<Zt> dE=<E> filament=<F>
… synthesised G1 / G4 lines …
;PYLON_INJECT_END
```

These are stable for tool/preview integration; future versions may extend the
key/value list but will keep `id`, `x`, `y`, `z_bottom`, `z_top`, `dE`,
`filament` present.

## Pylon vs. support

V1 enforces *one-way* exclusion: pylons are skipped if they overlap a user-
declared `SUPPORT_ENFORCER` volume (XY rectangle + Z range overlap, logged as a
warning per pylon). The check uses the model's enforcer volumes directly so the
scheduler can run before `generate_support_material()`.

**V1 limitation:** auto-generated overhang support is **not** geometrically
checked against pylons. If your print enables auto-support and a pylon happens
to sit under an overhang, the support material may collide with the pylon
footprint. A general warning is logged when both pylons and `enable_support`
are present.

**Mitigation in V1:** place a `SUPPORT_BLOCKER` volume around the pylon, or
manually verify your pylon placement is clear of overhang regions in the
preview.

## Floor safety

A pylon's first injection event is only scheduled when there's at least one
fully printed layer below the pylon's `z_bottom`. If a pylon starts at or below
the first printed layer, all events are skipped and a warning is logged. This
prevents trying to inject into the bed.

## Known V1 limitations

- **Cylinder shape only.** Adding a polygonal pylon is on the V2 roadmap.
- **Manual placement only.** No painting, no auto-generated pylon distribution.
- **Single global pylon filament.** Mixing filaments across different pylons in
  one print requires multiple manual sections (V2).
- **No wipe-tower trip on pylon-driven filament change.** If
  `pylon_injection_filament` differs from the layer's active filament, expect
  an unpurged tool change at each pylon event. Same-filament pylons (most
  common case) are unaffected.
- **Auto-support overlap detection.** Only `SUPPORT_ENFORCER` volumes
  participate in overlap checking; auto-generated overhang support is flagged
  via warning only. Place pylons clear of overhangs manually.
- **Layer-boundary scheduling only.** No sub-layer event triggering.
- **Single-coloring brick-bond.** `pylon_stagger_offset` is reserved for V2; V1
  uses a fixed 2-coloring with half-event-height Z stagger between alternating
  pylons.
- **No automatic shell-thickness adjustment near pylons.** Thicker walls around
  pylons (which JanTec's experiments suggest help with injection-induced
  deformation) must be done manually via per-region modifier volumes.

## Backward compatibility

Pylons are encoded as `NEGATIVE_VOLUME` + a `pylon_enabled` config flag on the
volume. Older slicers (PrusaSlicer, upstream OrcaSlicer, prior Jake-fork
versions) reading a 3MF with pylons will see ordinary negative volumes — they
carve the cylindrical hole correctly and skip injection. The slice will print
with unfilled pylons; nothing crashes.

## Files of interest

- `src/libslic3r/PylonInjection.{hpp,cpp}` — Event descriptor + JSON round-trip
  with `CustomGCode::Item` (single-event and batched).
- `src/libslic3r/PrintObject.cpp` — `compute_pylon_footprints()` and
  `schedule_pylon_injections()`.
- `src/libslic3r/Fill/Fill.cpp` — Hook B subtraction inside
  `Layer::make_fills()`.
- `src/libslic3r/GCode.cpp` — `emit_custom_gcode_per_print_z()` PylonInject
  branch (the actual synthesised block).
- `src/libslic3r/GCode/ToolOrdering.{hpp,cpp}` — `LayerTools::pylon_filament()`
  accessor (mirrors Jake's existing per-feature filament fork pattern).
- `src/slic3r/GUI/GUI_Factories.cpp` — "Add Pylon (cylinder)" menu entry.
- `tests/fff_print/test_pylon_injection.cpp` — Catch2 tests for markers, gates,
  and dE arithmetic.
