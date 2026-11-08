# Pylon Injection Feature: OrcaSlicer Codebase Reconnaissance

> Read-only investigation. No source modifications made. Working-tree document only.
> Branch state at recon: `feature/custom-infill` @ `415c7cb738` (caught up to upstream/main + Jake's per-feature filament commits).

---

## Summary

**Feasibility: high.** The major plumbing the feature needs already exists in mature form:
- **Modifier meshes** (`Model.hpp:341` ModelVolumeType enum) are the natural carrier for "pylon volumes" — Negative, ParameterModifier, SupportEnforcer/Blocker all demonstrate the boolean-subtraction + per-region-config patterns we need.
- **Per-Z event scheduling** is already a first-class concept via `CustomGCode::Item` (`CustomGCode.hpp:14`, consumed in `GCode.cpp:4102`). It supports an open-ended `Type` enum we can extend (ColorChange, PausePrint, ToolChange, Template, Custom, Unknown) and an `extra` string payload we can use as a JSON descriptor.
- **Flow → filament-length math** (`Flow::mm3_per_mm` at `Flow.cpp:201`, `Extruder::e_per_mm` at `Extruder.hpp:39`) is reusable as-is for computing E-mm-per-pylon-event from `π r² h`.
- **Per-feature filament routing** (Jake's existing `outer_wall_filament`/`top_surface_filament`/`bottom_surface_filament` extensions at `ToolOrdering.cpp:101-123`) demonstrates how to add `pylon_injection_filament` as a fourth role-keyed filament selector — config-level lookup, no ExtrusionRole enum changes required.

**Biggest unknowns:**
1. **In-layer Z descent has no precedent in the codebase.** Existing "out-of-band Z moves" (timelapse at `GCode.cpp:4966-5024`, retraction z-hop at `GCodeWriter.cpp:613`) all lift *above* current print_z. No code lowers the toolhead into already-printed volume and extrudes. The cooling buffer (`CoolingBuffer.cpp`) and spiral-vase post-processor (`GCode/SpiralVase.cpp`) are the closest templates for "rewrite the layer's G-code after emission with custom Z behavior."
2. **Per-pylon-per-Z scheduling at finer than layer granularity.** `CustomGCode::Item` fires at `print_z` matched to *layer boundaries* (`GCode.cpp:4677` injection site). True brick-bond staggering (pylon A fills at 5/15/25 mm, pylon B fills at 10/20/30 mm) maps cleanly onto layer boundaries if injection events happen at the layer whose `print_z` equals the pylon top, but if you want sub-layer triggering you have to build new infrastructure.
3. **Heat-management dwell insertion mid-layer.** `CoolingBuffer` slows down speed when a layer is too fast; it does **not** auto-emit `G4 P####` dwell commands (`CoolingBuffer.cpp:60, 534-536` parses G4 in user-injected gcode but never generates it). Letting injected plastic settle before resuming would need new code.

**Recommended general approach (high-level):**
1. Add `ModelVolumeType::PYLON_VOID` (or co-opt `NEGATIVE_VOLUME` with a per-volume flag) so the user can place pylon cylinders/boxes as part-level modifier volumes, sliced through the existing pipeline at `PrintObjectSlice.cpp:259` (`slices_to_regions`).
2. Subtract pylon footprints from sparse infill input ExPolygons at `Fill.cpp:1216-1307` inside `Layer::make_fills`, after `group_fills` aggregation but before `Fill::fill_surface_extrusion` (Hook 2 below). Skip if the surface is solid/top/bottom or bridged.
3. Walk the layer stack once per object during a new `posSchedulePylonInjection` step (slot it between `posInfill` and `posSupportMaterial` in `Print.hpp:94`) to compute the per-pylon (X, Y, Z_bottom, Z_top, dE) injection events, brick-bond staggered.
4. Surface those events as a new `CustomGCode::Type::PylonInject` `Item` per event (or a parallel injection-event list on `Print`) consumed by `ProcessLayer::emit_custom_gcode_per_print_z` at `GCode.cpp:4677`. Emit synthesized G-code: travel-to-XY, retract, descend Z, extrude bottom-up, return Z, unretract, resume layer.
5. Add `pylon_injection_filament` config (config-level only — no enum changes) and route via a new `LayerTools::pylon_filament()` accessor in `ToolOrdering.cpp` keyed on the new event type, with fallback to `solid_infill_filament`.

**Showstoppers: none identified.** The one architectural friction point is that any G-code emitted for a pylon injection must update `GCodeWriter`'s cached position state (esp. Z) so subsequent moves don't generate spurious travels — the timelapse insertion code at `GCode.cpp:4995` shows exactly this pattern (`GCodeProcessor::get_last_z_from_gcode()` post-injection sync).

---

## 1. Infill Generation Pipeline

### Fill class hierarchy (`src/libslic3r/Fill/`)

- **Base**: `Fill` at `FillBase.hpp:109-227`. Fields: `layer_id`, `z`, `spacing`, `overlap`, `angle`, `link_max_length`, `loop_clipping`, `bounding_box`, `adapt_fill_octree`. Key virtual methods: `fill_surface()`, `fill_surface_arachne()`, `fill_surface_extrusion()`, `_fill_surface_single()`, `_layer_angle()`, `_infill_direction()`.

| Pattern | File:Line | What it produces |
|---|---|---|
| `FillRectilinear` | `FillRectilinear.hpp:13` | Parallel lines, rotated per layer |
| `FillGrid` | `FillRectilinear.hpp:67` | Crossed rectilinear (constant 90°) |
| `FillTriangles` / `FillStars` | `FillRectilinear.hpp:92, 105` | 3-way / 5-way crossing |
| `FillCubic` / `FillQuarterCubic` | `FillRectilinear.hpp:118, 132` | Cubic lattice projection |
| `FillHoneycomb` | `FillHoneycomb.hpp:6` | Hex cells |
| `Fill3DHoneycomb` | `Fill3DHoneycomb.hpp:7` | Stacked hex |
| `FillGyroid` | `FillGyroid.hpp:6` | TPMS gyroid |
| `FillTpmsD` / `FillTpmsFK` | `FillTpmsD.hpp` / `FillTpmsFK.hpp` | TPMS D-surface / FK-surface |
| `FillConcentric` / `FillConcentricInternal` | `FillConcentric.hpp` / `FillConcentricInternal.hpp:5` | Inward loops |
| `FillLightning` | `FillLightning.hpp` | Octree-anchored branches (Z-aware) |
| `FillAdaptive` | `FillAdaptive.hpp` | Octree adaptive cubic |
| `FillLine` | `FillLine.hpp:6` | Unidirectional lines |
| `FillCrossHatch` | `FillCrossHatch.hpp:6` | Alternating rows |
| `FillPlanePath` | `FillPlanePath.hpp` | Hilbert/Archimedean/Octagram |
| `FillMonotonic`/`MonotonicLine(s)` | `FillRectilinear.hpp:49, 58, 167` | Monotonic rectilinear |
| `FillZigZag` / `FillCrossZag` / `FillLockedZag` | `FillRectilinear.hpp:192, 201, 210` | Sawtooth / cross / split variants |
| `FillLateralLattice` / `FillLateralHoneycomb` | `FillRectilinear.hpp:80, 144` | Lateral variants |
| `FillSupportBase` | `FillRectilinear.hpp:153` | Rectilinear support base |

### Entry point chain

```
PrintObject::infill()                                   PrintObject.cpp:670
  ↓
Layer::make_fills(adaptive_oct, support_oct, light)     Fill.cpp:1195
  ↓
group_fills(layer, lock_param) → vector<SurfaceFill>    Fill.cpp:830-1193
  (aggregates fill_surfaces by identical SurfaceFillParams; sets pattern/density/angle/role)
  ↓
per-SurfaceFill loop                                    Fill.cpp:1216-1339
  Fill *f = Fill::new_from_type(pattern);
  f->layer_id = …, f->z = layer.print_z, f->angle = …
  for each ExPolygon in surface_fill.expolygons:
    surface_fill.surface.expolygon = expoly;
    f->fill_surface_extrusion(surface, params, region_fills);
  ↓
Fill::fill_surface_extrusion()                          FillBase.cpp:133-191
  → Fill::fill_surface() or fill_surface_arachne()
  → subclass _fill_surface_single()
  → Polylines → ExtrusionEntityCollection → LayerRegion::fills.entities
```

### Sparse vs solid vs top/bottom decision

`group_fills` at `Fill.cpp:905-918` reads `surface.surface_type` (set during slicing in `PrintObject::prepare_infill` at `PrintObject.cpp:531`). The role assignment:

```cpp
params.extrusion_role = erInternalInfill;            // default sparse
if (is_bridge)         params.extrusion_role = surface.is_internal_bridge() ? erInternalBridgeInfill : erBridgeInfill;
else if (surface.is_solid()) {
    if (surface.is_top())    params.extrusion_role = erTopSolidInfill;
    else if (surface.is_bottom()) params.extrusion_role = erBottomSurface;
    else                          params.extrusion_role = erSolidInfill;
}
```

### Candidate hooks for pylon-footprint subtraction

| Hook | File:Line | Available context | Trade-off |
|---|---|---|---|
| **A. Inside `group_fills` after aggregation** | `Fill.cpp:1044` (after `diff_ex` that produces `fill.expolygons`) | `surface_fill.expolygons`, `surface_fill.params.pattern`, surface type | Operates on aggregated regions; can filter by role |
| **B. Inside `Layer::make_fills` before pattern instantiation** ★ recommended | `Fill.cpp:~1307` (inside per-surface_fill loop, before `f->fill_surface_extrusion(...)`) | `surface_fill.expolygons[i]`, `surface_fill.region_id`, `this->id()`, `this->print_z`, pattern | Per-expolygon control with full layer context; simplest to gate on `surface.is_internal() && !bridge` |
| **C. Inside `Fill::fill_surface` base impl** | `FillBase.cpp:108` (replace/augment `offset_ex` call) | `surface->expolygon`, `surface->surface_type`, `this->layer_id`, `this->z` | Centralized but loses region_id and config context |

**Recommended: Hook B.** Pre-compute pylon footprints at layer slicing time; subtract per-expolygon for `surface.is_internal() && !surface.is_bridge()` cases only.

---

## 2. Extrusion Entity Data Structures

### Hierarchy (`src/libslic3r/ExtrusionEntity.hpp`)

| Class | Line | Key fields | Notable methods |
|---|---|---|---|
| `ExtrusionEntity` (abstract) | 105 | — | `role()`, `is_collection()`, `is_loop()`, `can_reverse()`, `first_point()`, `last_point()`, `length()`, `min_mm3_per_mm()`, `total_volume()`, `polygons_covered_by_width()` |
| `ExtrusionPath` | 150 | `polyline` (Polyline3), `mm3_per_mm`, `width`, `height`, `overhang_degree`, `curve_degree`, `smooth_speed`, `z_contoured`, `m_role`, `m_no_extrusion`, `m_can_reverse` | overrides `role()`, `reverse()`, `clone()`, `clone_move()` |
| `ExtrusionPathContoured` | 308 | `z_diffs` (vector<double>) | overrides `simplify*`, `reverse()` |
| `ExtrusionPathSloped` | 325 | `slope_begin`, `slope_end` (Slope: `z_ratio`, `e_ratio`) | `interpolate()`, `is_flat()` |
| `ExtrusionPathOriented` | 361 | — | `can_reverse() = false` |
| `ExtrusionMultiPath` | 374 | `paths` (vector<ExtrusionPath>) | `role()` returns front path's role |
| `ExtrusionLoop` | 439 | `paths`, `m_loop_role` (ExtrusionLoopRole) | `is_loop() = true`, `can_reverse() = false` |
| `ExtrusionLoopSloped` | 524 | `starts`, `ends` (vector<ExtrusionPathSloped>) | `clip_slope()`, `clip_end()`, `clip_front()`, `slope_path_length()` |
| `ExtrusionEntityCollection` | `ExtrusionEntityCollection.hpp:25` | `entities`, `no_sort`, `is_reverse` | `role()` aggregates (erMixed if heterogeneous), `has_perimeters()`, `has_infill()`, `has_solid_infill()` |

### `ExtrusionRole` enum — complete (`ExtrusionEntity.hpp:20-43`)

```cpp
enum ExtrusionRole : uint8_t {
    erNone,                          // 0
    erPerimeter,                     // 1 internal perimeter
    erExternalPerimeter,             // 2 outer perimeter
    erOverhangPerimeter,             // 3 overhang
    erInternalInfill,                // 4 sparse infill
    erSolidInfill,                   // 5 internal solid
    erTopSolidInfill,                // 6 top surface
    erBottomSurface,                 // 7 bottom surface
    erIroning,                       // 8
    erBridgeInfill,                  // 9
    erInternalBridgeInfill,          // 10
    erGapFill,                       // 11
    erSkirt,                         // 12
    erBrim,                          // 13
    erSupportMaterial,               // 14
    erSupportMaterialInterface,      // 15
    erSupportTransition,             // 16 BBS addition
    erWipeTower,                     // 17
    erCustom,                        // 18 (user-defined)
    erMixed,                         // 19 (collection-only)
    erCount                          // 20 sentinel
};
```

Helpers at `ExtrusionEntity.hpp:56-103`: `is_perimeter`, `is_internal_perimeter`, `is_external_perimeter`, `is_infill`, `is_solid_infill`, `is_top_surface`, `is_bridge`.

### Role assignment sites

- **Fill subclasses** via `params.extrusion_role` in `Fill.cpp:905-918` (see §1).
- **Perimeter generator**: `PerimeterGenerator.cpp:117` — `role = is_external ? erExternalPerimeter : erPerimeter`. Overhang detect at line 460 emits `erOverhangPerimeter`.
- **Support**: `Support/SupportCommon.cpp:589, 694, 746` → `erSupportMaterial`; line 1058 → `erSupportMaterialInterface`. `TreeSupport.cpp:1515` switches on `interface_as_base`.

### Role inspection during emission

- **`LayerTools::extruder`** (`ToolOrdering.cpp:126-166`) branches on `erTopSolidInfill`, `erBottomSurface`, `is_solid_infill`, `erExternalPerimeter`. This is the filament-selection chokepoint.
- **`GCode.cpp:6330-6451`** branches on role for:
  - Acceleration/jerk: `sparse_infill_acceleration` (erInternalInfill), `inner_wall_acceleration` (internal perimeter), `outer_wall_acceleration` (external perimeter), `top_surface_acceleration` (top).
  - Fan-speed processor tags (lines 6378-6451).
  - Per-role speed selection (lines 6391-6450).

### Per-feature filament wiring (Jake's fork) — config-level, **no enum change**

**Commit:** `93f4990` "Add per-feature filament assignment for outer walls, top/bottom surfaces" (2026-02-11).

New config keys (`PrintConfig.hpp:1137, 1145-1146`):
```cpp
((ConfigOptionInt, outer_wall_filament))     // 0 = fallback to wall_filament
((ConfigOptionInt, top_surface_filament))    // 0 = fallback to solid_infill_filament
((ConfigOptionInt, bottom_surface_filament)) // 0 = fallback to solid_infill_filament
```

New `LayerTools` methods (`ToolOrdering.cpp:101-123`):
```cpp
unsigned int LayerTools::outer_wall_filament(const PrintRegion &region) const {
    int v = region.config().outer_wall_filament.value;
    if (v == 0) return wall_filament(region);
    return ((this->extruder_override == 0) ? v : this->extruder_override) - 1;
}
// top_surface_filament and bottom_surface_filament follow the same pattern,
// falling back to solid_infill_filament(region).
```

`LayerTools::extruder` (`ToolOrdering.cpp:134-156`) dispatches on the first-non-erNone path role inside an `ExtrusionEntityCollection`:
- `erTopSolidInfill` → `top_surface_filament(region)`
- `erBottomSurface` → `bottom_surface_filament(region)`
- `erExternalPerimeter` → `outer_wall_filament(region)`
- `is_solid_infill` → `solid_infill_filament`
- else (infill) → `sparse_infill_filament`; else (perimeter) → `wall_filament`

**Approach confirmed: config-level lookup keyed on existing roles. Enum unchanged.** This is the pattern we should clone for `pylon_injection_filament`.

### `Flow` class — volume math

- `Flow` definition: `Flow.hpp:52-101`. Fields: `m_width`, `m_height`, `m_spacing`, `m_nozzle_diameter`, `m_bridge`.
- `Flow::mm3_per_mm()`: `Flow.cpp:201-212`:
  ```cpp
  double Flow::mm3_per_mm() const {
      float res = m_bridge ?
          float((m_width * m_width) * 0.25 * PI) :
          float(m_height * (m_width - m_height * (1. - 0.25 * PI)));
      return res;
  }
  ```
- `Extruder::e_per_mm(double mm3_per_mm)`: `Extruder.hpp:39` returns `mm3_per_mm * m_e_per_mm3`. `m_e_per_mm3` cached in `Extruder.cpp:9-19` from `filament_flow_ratio() / filament_crossection()`.
- Filament cross-section: `Extruder.cpp:61` — `filament_diameter² × 0.25 × π`. Diameter from `filament_diameter.get_at(m_id)` at `Extruder.cpp:148-150`.

**For pylon injection:** volume = `π r² h` (pylon dims, mm³); filament length = `volume * m_e_per_mm3` for the chosen filament. No reuse barrier.

---

## 3. Layer & Region Model

### Class hierarchy

- **`LayerRegion`** (`Layer.hpp:33`): per-`PrintRegion` geometry within a `Layer`. Fields: `slices` (SurfaceCollection, line 44), `raw_slices` (ExPolygons, line 48), `fill_surfaces` (SurfaceCollection, line 59), `fill_expolygons` (ExPolygons, line 57), `perimeters` (ExtrusionEntityCollection, line 72), `fills` (ExtrusionEntityCollection, line 76). Private: `m_layer`, `m_region`.
- **`Layer`** (`Layer.hpp:123`): Fields `lslices` (ExPolygons, line 156 — **merged model footprint at this Z**), `lslices_extrudable` (line 157), `lslices_bboxes` (line 158), `print_z` (line 136), `slice_z` (line 135), `height` (line 137), `m_regions` (LayerRegionPtrs, line 272). `make_slices()` at line 170 merges per-region slices into `lslices`.
- **`SupportLayer`** (`Support/SupportLayer.hpp:280`): inherits Layer; adds `support_islands` (line 285), `support_fills` (line 287), `support_type` (line 288), `base_areas`/`roof_areas`/`floor_areas` (lines 291-317).
- **`PrintObject`** (`Print.hpp:313`): owns `m_layers` (line 558), `m_support_layers` (line 559), `m_shared_regions` (line 555). `get_layer_at_printz()` at line 382, `slicing_parameters()` at line 419.
- **`Print`** (`Print.hpp:886`): owns `m_objects` (line 1139), `m_print_regions` (line 1140).

### Per-object pipeline order

Steps declared in `Print.hpp:94` (`PrintObjectStep` enum). Invalidation graph in `PrintObject::invalidate_step` at `PrintObject.cpp:1433`. Execution order:

```
posSlice          → slice_volumes()      PrintObjectSlice.cpp:1139
                  → slices_to_regions()  PrintObjectSlice.cpp:259
                  → Layer::make_slices() Layer.cpp                (merges to Layer::lslices)
posPerimeters     → make_perimeters()    PrintObject.cpp:424
posPrepareInfill  → prepare_infill()     PrintObject.cpp:531      (classifies sparse/solid/top/bottom)
posInfill         → infill()             PrintObject.cpp:670
                  → Layer::make_fills()  Fill.cpp:1195
posIroning        → ironing()            PrintObject.cpp:699
posSupportMaterial→ generate_support_material()
```

**Pylon scheduling step would slot here**: between `posInfill` and `posSupportMaterial` (i.e., after fills are known so we know where the void is geometrically valid, but before support so support generation can be informed by injection paths if needed).

### Layer slice geometry at a given Z

- **`Layer::lslices` (ExPolygons, `Layer.hpp:156`)** — definitive cross-sectional model footprint, merged across regions. **Query this** to intersect with pylon footprints.
- Per-region typed slices: `LayerRegion::slices` (SurfaceCollection at `Layer.hpp:44`).
- Per-region fillable area: `LayerRegion::fill_expolygons` (`Layer.hpp:57`), `LayerRegion::fill_surfaces` (`Layer.hpp:59`).

### Modifier volume slicing & per-region config

- `ModelVolumeType` enum: `Model.hpp:341`:
  ```cpp
  enum class ModelVolumeType : int {
      INVALID = -1, MODEL_PART = 0, NEGATIVE_VOLUME,
      PARAMETER_MODIFIER, SUPPORT_BLOCKER, SUPPORT_ENFORCER
  };
  ```
- `slice_volumes()` → `slice_volumes_inner()` at `PrintObjectSlice.cpp:1139, 1161` produces `objSliceByVolume` (one entry per `ModelVolume`).
- `slices_to_regions()` at `PrintObjectSlice.cpp:259` walks `PrintObjectRegions::LayerRangeRegions::volume_regions` (line 298) and assigns slices to regions. Negative-volume subtraction at lines 415-431 via `diff_ex` against preceding positive volumes (BBox-tested).
- Per-region config resolution: `region_config_from_model_volume()` at `PrintObject.cpp:3422-3453`:
  ```
  PrintRegionConfig = default                       // global PrintRegionConfig
    overlay ModelObject.config                      // object-level
    overlay ModelVolume.config                      // modifier-level (this is where painted/modifier settings live)
    overlay Material.config
    overlay LayerRange.config                       // per-Z-range overrides
  ```
  Called from `PrintApply.cpp:793-795` during region verification.

### Answer to key question

**Yes — pylon footprints can be obtained per-Z geometrically consistent with model existence.**

Pylons modeled as ModelVolumes pass through `slice_volumes_inner()` and produce per-Z `VolumeSlices`. After `slices_to_regions` runs, for each Z:

```
pylon_at_z = intersection_ex(pylon_volume_slices_at_z, Layer::lslices)
```

`Layer::lslices` is the post-merge model footprint, so the intersection is guaranteed to be inside the printed model. The `intersection_ex` primitive is in `ClipperUtils.hpp` and used dozens of places (e.g., `PrintObjectSlice.cpp:415-431` does the analogous `diff_ex`).

---

## 4. Tool Ordering & Layer Tools

### `LayerTools`

- Definition: `ToolOrdering.hpp:128-181`. Field `extruder_override` (used for prime tower / wipe-tower scheduling), `extruders` (vector of extruder IDs used by this layer), `custom_gcode` (pointer to a single `CustomGCode::Item` matched to this layer's print_z), `wipe_tower_partitions`.
- Filament-role accessors: `wall_filament` (line 83-87), `sparse_infill_filament` (89-93), `solid_infill_filament` (95-99), `outer_wall_filament` (101-107 ★ Jake), `top_surface_filament` (109-115 ★ Jake), `bottom_surface_filament` (117-123 ★ Jake).
- **`LayerTools::extruder(const ExtrusionEntityCollection&, const PrintRegion&)`** — the dispatch bottleneck — at `ToolOrdering.cpp:126-166`. Full body quoted in §2. Branches on first non-`erNone` role in the collection.

### Callers of `LayerTools::extruder()`

- `GCode.cpp:4872` — per-entity extruder selection during per-layer entity dispatch.

(No other callers found — the function is centralized.)

### Filament selection entry points (per feature)

| Feature | Config key | Selector | File:line |
|---|---|---|---|
| Outer wall | `outer_wall_filament` | `LayerTools::outer_wall_filament()` | `ToolOrdering.cpp:101` |
| Inner wall | `wall_filament` | `LayerTools::wall_filament()` | `ToolOrdering.cpp:83` |
| Sparse infill | `sparse_infill_filament` | `LayerTools::sparse_infill_filament()` | `ToolOrdering.cpp:89` |
| Solid infill | `solid_infill_filament` | `LayerTools::solid_infill_filament()` | `ToolOrdering.cpp:95` |
| Top surface | `top_surface_filament` | `LayerTools::top_surface_filament()` | `ToolOrdering.cpp:109` |
| Bottom surface | `bottom_surface_filament` | `LayerTools::bottom_surface_filament()` | `ToolOrdering.cpp:117` |
| Support | `support_filament` | direct config | `ToolOrdering.cpp:815` |
| Support interface | `support_interface_filament` | direct config | `ToolOrdering.cpp:816` |
| Skirt | first-layer extruder | layer marking | `ToolOrdering.cpp:1454-1487` |
| Brim | `PrintRegion::extruder(frExternalPerimeter)` | derived from outer wall | `Brim.cpp:131` |
| Prime/wipe tower | `wipe_tower_filament` | `insert_wipe_tower_extruder()` | `ToolOrdering.cpp:354-372` |
| Color change (M600) / Pause (M601) / Custom blocks | `CustomGCode::Item.extruder` | `assign_custom_gcodes()` | `ToolOrdering.cpp:1501-1563` |

### Multi-extruder mode

`FilamentChangeMode` (`ToolOrdering.hpp:186-190`): `SingleExt`, `MultiExtBest`, `MultiExtCurr`. Custom-gcode mode at `ToolOrdering.cpp:1511-1522`:
```cpp
mode = (num_filaments == 1) ? SingleExtruder
     : print.object_extruders().size() == 1 ? MultiAsSingle
     : MultiExtruder;
```

Support extruder logic dynamically picks least-flush extruder when `support_filament == 0` (`ToolOrdering.cpp:815-845`). SEMM vs Type1 wipe-tower branched at `ToolOrdering.cpp:1302`.

### Prime tower

`insert_wipe_tower_extruder()` at `ToolOrdering.cpp:354-372` — inserts the wipe-tower extruder into each layer's extruder list where `wipe_tower_partitions > 0`. Partition allocation at `ToolOrdering.cpp:863-882` (decrements when first-layer-extruder matches previous-layer-last-extruder). If the wipe-tower extruder insertion changes layer extruder topology, `reorder_extruders_for_minimum_flush_volume()` re-optimizes.

**For pylon injection**: if `pylon_injection_filament` differs from the surrounding shell's filament, each pylon event introduces an unscheduled filament change. Either (a) batch pylon events to one-filament-change-per-layer, (b) build a "near-pylon" prime/wipe routine that doesn't go to the main wipe tower for short purges, or (c) restrict pylon filament to be the same as surrounding shell (simplest).

---

## 5. G-code Generation

### Per-layer emit loop

Two paths to `process_layer`:
- Sequential print: `GCode.cpp:3292` → `GCode.cpp:3599 process_layers()` (one-object-at-a-time).
- Parallel print: `GCode.cpp:3375` → `GCode.cpp:3701 process_layers()` (multi-object).

Both ultimately call **`GCode::process_layer()` at `GCode.cpp:4387`**. Its high-level structure (lines 4467-4677):

```
;LAYER_CHANGE / ;Z:%g tags                                      4467-4475
before_layer_change_gcode (placeholder_parser_process)           4482-4489
change_layer(print_z)        // Z move + retract if needed       4500
layer_change_gcode                                                4527-4534
[skirt, brim] on first layer
[per-PrintObject loop]
  perimeters, infill, ironing, support
emit_custom_gcode_per_print_z(…)                                  4677
```

Post-processing pipeline (`GCode.cpp:3687-3694, 3744-3758`):
```
process_layer output
  → SpiralVase::process_layer    (if enabled)
  → PressureEqualizer::process_layer
  → CoolingBuffer::process_layer
  → AdaptivePAProcessor::process_layer
  → FanMover (if applicable)
  → file
```

### `ExtrusionPath` → `G1` trace

- **Dispatch**: `GCode::extrude_entity(const ExtrusionEntity&, …)` at `GCode.cpp:5994` — `dynamic_cast` cascade to `extrude_path` / `extrude_multi_path` / `extrude_loop`.
- **Path emission**: `GCode::extrude_path()` at `GCode.cpp:6010` → `GCode::_extrude(path, description, speed)` at `GCode.cpp:6250`. Params: `const ExtrusionPath&`, `std::string description`, `double speed`.
- **Inside `_extrude` (`GCode.cpp:6250-6951`)**:
  1. `travel_to(first_point, role, comment, z)` (line 6283) — handles **retraction + z-hop** internally.
  2. `travel_to_z(m_nominal_z, …)` (lines 6286-6305).
  3. `unretract()` (line 6313).
  4. `set_accel_and_jerk()` / `set_print_acceleration()` / `set_jerk_xy()` (lines 6316-6370).
  5. Flow rate `_mm3_per_mm` / `e_per_mm` (lines 6372-6418).
  6. Speed selection (lines 6420-6558).
  7. Adaptive PA tags (lines 6721-6747).
  8. Fan-speed processor markers (lines 6750-6827).
  9. Per-segment emission: `m_writer.extrude_to_xy(dest, dE, comment)` at `GCodeWriter.cpp:912` (or `extrude_to_xyz` at 956, or `extrude_arc_to_xy` for arcs).
- **G1 line format**: `GCodeWriter.cpp:926-931` — `G1 X… Y… E… F… ; comment`.

### Custom G-code insertion sites

| Hook | File:Line | Notes |
|---|---|---|
| `machine_start_gcode` | `GCode.cpp:3077-3096` | Pre-print preamble |
| `before_layer_change_gcode` | `GCode.cpp:4482-4489` | Before `change_layer()` Z move |
| `layer_change_gcode` | `GCode.cpp:4527-4534` | After Z move |
| `change_filament_gcode` | `GCode.cpp:971` (ProcessLayer ns) | At tool change |
| `machine_pause_gcode` (M601) | `GCode.cpp:4168` | At PausePrint event |
| `time_lapse_gcode` | `GCode.cpp:4966-5024` (`insert_timelapse_gcode` lambda) | At layer boundary, user template; **note the GCodeProcessor::get_last_z_from_gcode() sync at line 4995** |
| `machine_end_gcode` | `GCode.cpp:3441` | End of print |
| `custom_gcode_per_print_z` | `GCode.cpp:4677` (callsite), `4102-4186` (`emit_custom_gcode_per_print_z` body) | Per-Z dispatch on `Type` |

All hooks invoke `placeholder_parser_process(name, template, extruder_id, &config)` with a `DynamicConfig` carrying context vars.

### Placeholders

Set per-hook via `DynamicConfig::set_key_value`; no central registry. Common keys seen in `GCode.cpp`:
- `{layer_num}` (1- or 0-based depending on hook), `{layer_z}`, `{max_layer_z}`
- `{current_extruder}`, `{most_used_physical_extruder_id}`, `{color_change_extruder}` (line 4145)
- `{extrusion_role}` (string)
- `{relative_e_axis}` (bool, line 7775)
- `{old_retract_length}`, `{new_retract_length}` (filament-switch context, lines 895-897)

Parser core: `PlaceholderParser.cpp:2429-2436` — context built from `current_extruder_id` + `config_override`.

### E-axis mode

- Config: `use_relative_e_distances` (bool).
- Branched in: `GCode.cpp:1633, 1686` (position tracking), `3677, 3775` (post-processor flags), `7775` (placeholder export).
- G1 emission: `GCodeWriter::extrude_to_xy()` (`GCodeWriter.cpp:920`) calls `filament()->extrude(dE)` to update cached E state, then emits `w.emit_e(filament()->E())`. The emitted value is always the *current absolute E*; M82 vs M83 is determined by the printer firmware interpreting against the most recent mode-setting `G91`/`G90`/`M82`/`M83`. **For pylon injection: emit M83 before the injection block (or expect relative E throughout), do dE math against the local frame.**

### `custom_gcode_per_print_z` storage and consumption

- **Storage**: `Model::custom_gcode_per_print_z` of type `CustomGCode::Info` (`CustomGCode.hpp:14+`):
  ```cpp
  struct Info { Mode mode; std::vector<Item> gcodes; };
  struct Item { double print_z; Type type; int extruder; std::string color; std::string extra; };
  enum Type { ColorChange, PausePrint, ToolChange, Template, Custom, Unknown };
  ```
- **Consumption**: each `LayerTools` is associated with **a single** `CustomGCode::Item` (matched by print_z in tool ordering planning). `process_layer` retrieves it via `layer_tools.custom_gcode` and invokes `emit_custom_gcode_per_print_z()` at `GCode.cpp:4677`.

### Key question — cleanest injection hook

**Best hook for "travel to (X,Y) → descend Z → extrude N mm ascending → return Z → resume":**

`GCode::process_layer` at the end-of-layer point, *after* layer extrusions are emitted but *before* the next-layer `change_layer()`. Equivalent: emit inside `emit_custom_gcode_per_print_z` (`GCode.cpp:4102-4186`) by adding a `PylonInject` `Type`.

Synthesize the injection sequence directly via `GCodeWriter` to keep the writer's cached state coherent:
- `m_writer.travel_to_xy(p)` then `m_writer.travel_to_z(z_bottom)` (these do not consume E)
- M83 if not already relative
- Loop: `m_writer.extrude_to_xyz(Vec3d(X, Y, z_step), dE_step)` ascending by a small Δz per step
- `m_writer.travel_to_z(layer.print_z)` to restore
- Update `m_nominal_z` to layer print_z (or rely on next-layer `change_layer` to do it)

The timelapse code (`GCode.cpp:4966-5024`) is the precedent for synthesizing out-of-band Z/XY motion and re-syncing writer state via `GCodeProcessor::get_last_z_from_gcode()`.

---

## 6. Custom Per-Layer / Per-Print-Z G-code Hooks

### `custom_gcode_per_print_z`

- **Stored**: `Model::custom_gcode_per_print_z` (CustomGCode::Info).
- **Consumed**: per-layer in `ProcessLayer::emit_custom_gcode_per_print_z` (`GCode.cpp:4102-4186`), called from `process_layer` at `GCode.cpp:4677`.
- **Match granularity**: layer boundary (matched by `print_z`).
- **Types**: ColorChange, PausePrint, ToolChange, Template, Custom, Unknown (`CustomGCode.hpp:14-22`).
- **Item fields**: `print_z` (double), `type` (Type), `extruder` (int), `color` (string), `extra` (string — gcode text for Custom, message for PausePrint, JSON for Template).

### Layer-change placeholders

`DynamicConfig` keys set in `GCode.cpp:4482-4489` and `4527-4534`:
- `layer_num` (int)
- `layer_z` (float)
- `max_layer_z` (float)
- `current_extruder` (int, where applicable)

Other context-specific placeholders set per hook (see §5).

### Is there an existing scheduler?

**Yes for layer-boundary events**, **no for in-layer events**. `CustomGCode::Item` is a layer-boundary scheduler. Pylon injection events naturally happen at layer boundaries if you align the pylon top with a layer's `print_z`, so we can extend this scheduler with a new `Type::PylonInject` and store the per-event descriptor (X, Y, Z_bottom, dE, filament_id, …) in `extra` as JSON.

If sub-layer triggering is required, you'd need new infrastructure — but the brick-bond pattern described in the brief naturally aligns to layer boundaries (every Nth layer for pylon A, offset by N/2 for pylon B), so layer-boundary scheduling should suffice.

---

## 7. Existing Non-Planar / Cavity-Related Features

| Feature | Status | File:Line | Reusable? | Notes |
|---|---|---|---|---|
| **Spiral vase** | ✓ found | `GCode/SpiralVase.cpp`, `.hpp` (`process_layer` post-processor) | **Yes** | Closest template for per-layer G-code rewrite with continuous Z modulation. Parses G1 moves, interpolates Z across XY arc length. |
| **Z-pinning** | ✗ not present | — | — | No matches for `z[_-]?pin`/`zpin`. |
| **Z-hop with extrusion** | ✗ not present | `GCodeWriter.cpp:613, 641` | — | Z-hop is retract-only. Retract/unretract emitted as G10/G11 (`GCodeWriter.cpp:1014-1054`), never combined with E during Z motion. |
| **Non-planar slicing/extrusion** | ✗ not present | — | — | No `non[_-]?planar`/`nonplanar` hits in code paths. |
| **Fuzzy skin** | ✓ found | `Feature/FuzzySkin/FuzzySkin.cpp`, `.hpp:18-19` (`apply_fuzzy_skin`) | No | Modifies Polygon/ExtrusionLine *before* G-code emission. Wrong abstraction level for pylon injection. |
| **FillLightning (Z-aware fill)** | ✓ found | `Fill/FillLightning.cpp:17` (`getTreesForLayer`), `Fill/Lightning/Generator.cpp` | Maybe | Multi-layer octree, branches anchored to layers. Demonstrates pre-computed Z-dependent infill; pylon injection is *reactive* not *pre-computed*, so only the data structure is borrowable. |
| **FillAdaptive cubic** | ✓ found | `Fill/FillAdaptive.cpp` | Maybe | Same as Lightning re: octree pattern. |
| **Hollow / cavity** | ✗ not present (FDM) | — | — | `hollow`/`cavity` hits are SLA-only (`SLA/Hollowing.cpp`, `GLGizmoHollow.cpp`). |
| **Timelapse capture moves** ★ | ✓ found | `GCode.cpp:4966-5024` (`insert_timelapse_gcode` lambda) | **Yes** — direction inverted | Lifts toolhead **above** print at layer boundary, runs user G-code template, syncs writer Z via `GCodeProcessor::get_last_z_from_gcode()` at line 4995. Closest precedent for "out-of-band Z move with writer-state coherence." Pylon is the same mechanism in reverse. |
| **TimelapsePosPicker** | ✓ found | `GCode/TimelapsePosPicker.cpp` | Maybe | Picks safe-area XY for the lift. We don't need "safe" XY for pylons (we *want* to be at the pylon center), but the bbox-collision logic could be adapted for "no support material overhead" checks. |
| **Mid-print G4 dwell** | ✗ not auto-generated | `GCode/CoolingBuffer.cpp:60, 534-536` | No | CoolingBuffer parses user-injected G4 and respects timing, but never generates dwells. Speed-slowdown is its tool (`slow_down_layer_time`, `slow_down_min_speed` at lines 234-237). Heat-management dwell after injection must be built from scratch. |
| **CustomGCode scheduler** ★ | ✓ found | `CustomGCode.hpp:14-22`, `GCode.cpp:4102-4186, 4677`, `ToolOrdering.cpp:1501-1563 assign_custom_gcodes` | **Yes** — primary reuse | Already a per-print-Z action scheduler with open Type enum. Add `Type::PylonInject`. |

**Architectural recommendation surface from §7:** Extend `CustomGCode::Type` enum with `PylonInject`; use Item's `extra` field as JSON-encoded pylon event descriptor; emit synthesized G-code in `emit_custom_gcode_per_print_z` using `GCodeWriter` directly, modeled on `insert_timelapse_gcode`'s state-sync pattern.

---

## 8. Modifier Meshes & Per-Region Settings

### `ModelVolumeType` (`Model.hpp:341-348`)

Five active types: `MODEL_PART`, `NEGATIVE_VOLUME`, `PARAMETER_MODIFIER`, `SUPPORT_BLOCKER`, `SUPPORT_ENFORCER` (plus sentinel `INVALID = -1`).

3MF string mapping at `Model.cpp:2668-2680` (write) and `Format/3mf.cpp:361-372` (read). Read path has fallback to `MODEL_PART` for unknown types — meaning **older OrcaSlicer builds that don't know about a new type will silently fall back to a printed solid**. We need to be aware of this for backward-compat (e.g., if pylons are encoded as a new type, an older slicer will treat them as solid parts).

### GUI flow for adding modifier

- Menu definition: `GUI_Factories.cpp:330-333` (`ADD_VOLUME_MENU_ITEMS`).
- Menu entry: `GUI_Factories.cpp:537-568` (`append_submenu_add_generic`).
- Handler: `GUI_ObjectList.cpp:2325` (`load_modifier`) → `ModelObject::add_volume(mesh, type)` (`Model.hpp:414-418`).
- Display name map: `GUI_Factories.cpp:810-812`:
  ```cpp
  { ModelVolumeType::PARAMETER_MODIFIER, _L("Modifier") },
  { ModelVolumeType::SUPPORT_BLOCKER,    _L("Support Blocker") },
  ```
  Adding a new type requires three GUI changes: enum, type-string maps, menu array, name map.

### Negative-volume subtraction

Implemented at `PrintObjectSlice.cpp:415-431`. For each Z, walks through `temp_slices` in volume_regions order; when a `NEGATIVE_VOLUME` region's slice is processed, calls `diff_ex(temp_slices[preceding].expolygons, temp_slices[current].expolygons)` against all earlier non-negative regions whose XY bbox overlaps.

### Per-region config resolution

`region_config_from_model_volume()` at `PrintObject.cpp:3422-3453`:
```cpp
PrintRegionConfig config = default_region_config;
if (volume.is_model_part()) apply_to_print_region_config(config, volume.get_object()->config.get());
apply_to_print_region_config(config, volume.config.get());          // modifier-level overrides
if (! volume.material_id().empty())
    apply_to_print_region_config(config, volume.material()->config.get());
if (layer_range_config != nullptr)
    apply_to_print_region_config(config, *layer_range_config);
```

`ModelVolume::config` is a `ModelConfigObject` (a dynamic config). Any PrintRegionConfig key can be overridden — no allowlist hard-coding. So pylon-specific settings (e.g., `pylon_injection_period`, `pylon_diameter`, `pylon_filament_id`) can be added to `PrintRegionConfig` and surfaced as modifier-volume overrides for free.

### Adding a new ModelVolumeType — friction assessment

**Medium**, no architectural blocker. Required changes (all locations identified):
1. Enum: `Model.hpp:341`.
2. Type-string map: `Model.cpp:2668-2680` (write) + `Format/3mf.cpp:361-372` (read).
3. GUI menu array: `GUI_Factories.cpp:330-333`.
4. GUI display map: `GUI_Factories.cpp:810-812`.
5. Slicing handlers in `PrintObjectSlice.cpp:259, 415` (need to decide: does the pylon volume *carve* like Negative, or is it kept as a marker volume that's used only by the pylon scheduler?).

### Answer to key question: pylons as modifier-volume type — feasible?

**Yes.** Two viable encodings:
- **(a)** New `ModelVolumeType::PYLON_VOID` — clean semantics, but requires backward-compat handling for older slicers reading 3MF (they'll fall back to MODEL_PART = printed solid, which is *wrong* — they'd print a solid pylon).
- **(b)** Reuse `NEGATIVE_VOLUME` + a `ModelVolume.config` key like `pylon_injection_enabled` to mark intent. Older slicers will see a negative volume and correctly carve it out — they just won't do the injection step. **Recommended** for shipping.

---

## 9. Multi-Filament / Multi-Process Interaction

Jake's fork has `outer_wall_filament` / `top_surface_filament` / `bottom_surface_filament` config keys. The pattern (described in §2 and §4) is **config-level lookup keyed on the existing `ExtrusionRole`, dispatched in `LayerTools::extruder` at `ToolOrdering.cpp:126`**, with fallback chains (`outer_wall → wall_filament`, `top/bottom_surface → solid_infill_filament`).

### Recommended slotting for pylon injection filament

Add a new `pylon_injection_filament` PrintRegionConfig key (config-level only — **no `ExtrusionRole` enum extension required**). Add a `LayerTools::pylon_filament(const PrintRegion&)` accessor mirroring the existing per-feature accessors. Default fallback: `solid_infill_filament`.

Pylon injection events don't pass through `LayerTools::extruder()` (they're emitted outside the per-ExtrusionEntityCollection dispatch loop), so the new accessor is invoked directly from the new `Type::PylonInject` handler in `emit_custom_gcode_per_print_z`.

### Prime tower / wipe tower interactions

Significant. Each filament change for a pylon event requires either:
- A trip to the wipe tower (`ToolOrdering.cpp:354-372` `insert_wipe_tower_extruder`), which is XY-expensive.
- A near-pylon prime/purge maneuver (does not exist — would need to be built).
- Batching: collect all pylon events for one filament at one Z; if a different filament is used, do **one** filament change per layer per filament, not per pylon.

**Pragmatic default:** restrict `pylon_injection_filament` to be either the same as the surrounding region's filament (cheapest) or designate a single dedicated "reinforcement filament" used for all pylons. Avoid per-pylon filament variation in V1.

---

## 10. Build & Test Considerations

### Test layout (`tests/`)

- `tests/libslic3r/` — 21 test files; core geometry, file format, config, flow. Catch2-based.
- `tests/fff_print/` — 12 test files; layer generation, G-code mechanics. Catch2.
- `tests/sla_print/` — 4 test files; SLA-specific.
- `tests/libnest2d/` — nesting algorithm.
- `tests/slic3rutils/` — utilities.
- `tests/catch2/` — bundled framework.

Frameworks: **Catch2 v2** with custom verbose console reporter; discovered via CMake's `catch_discover_tests()`. Test data in `tests/data/`.

**No dedicated end-to-end "slice STL and inspect G-code" test exists.** Individual algorithms have unit tests (`test_elephant_foot_compensation.cpp`, etc.) but not full-pipeline.

### CLI mode (headless slicing)

- Entry: `CLI::run(int argc, char **argv)` at `src/OrcaSlicer.cpp:1180`.
- Slicing trigger: passing model files + a config; output written to `outputdir` (line 1271). No explicit `--export-gcode` flag — slicing is implied when actions are non-empty.
- Typical invocation form (inferred — verify on the binary): `orca-slicer.exe --load-settings <preset.json> --slice <model.3mf> --outputdir <dir>`.

### CMake targets (Windows builds we confirmed)

- `libslic3r` — core lib.
- `libslic3r_gui` — wxWidgets-aware extensions.
- `OrcaSlicer.dll` (95 MB) — main app body.
- `orca-slicer.exe` (270 KB) — launcher.
- `OrcaSlicer_profile_validator.exe` — profile validation utility.
- `libslic3r_tests` / `fff_print_tests` / `sla_print_tests` / `libnest2d_tests` — Catch2 binaries.

Full from-scratch wxWidgets + main app build on this machine: ~30 min (verified). Incremental main app build after a libslic3r change: 1–3 min typically.

---

## Cross-Cutting Observations

### Brick-bond staggering

The most distinctive VoxelFill behavior — pylon A fills at Z=5/15/25 mm, pylon B at Z=10/20/30 mm — maps cleanly onto the existing `CustomGCode::Item.print_z` field. For each pylon, schedule events at `print_z = pylon_top + n * stagger_offset` for `n = 0, 1, 2, …` until pylon top exceeds model top. No new "schedule event E at Z = f(X, Y, layer)" infrastructure needed beyond extending `Type`.

The scheduler can live in a new `PrintObjectStep::posSchedulePylonInjection` step (slot after `posInfill` in `Print.hpp:94`), iterating `m_layers` and appending `Item`s.

### Out-of-band Z move coherence

The only existing analogue is timelapse (`GCode.cpp:4966-5024`). The critical pattern is line 4995:

```cpp
double new_z = GCodeProcessor::get_last_z_from_gcode(timelapse_gcode);
if (new_z >= 0)
    m_writer.set_position_z(new_z);  // sync writer state
```

Any pylon injection sequence must do the same — either by writing the synthetic G-code through `GCodeWriter` directly (which updates state automatically) or by emitting raw text and then calling `m_writer.set_position_z(layer.print_z)` to restore.

### Heat management

No mid-layer dwell auto-generation. `CoolingBuffer.cpp:60, 534-536` only parses user G4. Slowdown is via `slow_down_layer_time` / `slow_down_min_speed` (lines 234-237) — it modulates F values, never inserts G4.

For pylon injection, we have three options:
1. **Configurable per-event dwell**: emit `G4 P####` at end of each injection event. Implement as a `pylon_injection_dwell_ms` config key consumed inside the new emit handler.
2. **Layer-time padding**: extend the existing CoolingBuffer to account for injection event time so the layer's natural cycle gives plastic time to solidify.
3. **Reduce stagger frequency**: simpler — every N layers between injections gives natural cooling time during normal printing.

Recommended: combine (1) configurable dwell + (3) configurable injection period.

### Volume → filament length math

Pre-built. Pylon volume per event = `π * (r_pylon)² * h_event` mm³. Filament length = `volume * Extruder::m_e_per_mm3` (cached value derived from `filament_diameter` at Extruder construction; see `Extruder.cpp:9-19, 61, 148-150`). No new math required.

```
double mm3 = PI * r * r * h;
double dE  = mm3 * extruder.m_e_per_mm3;   // E-mm to dispense
```

If injection happens during an ascending move (continuous extrude as Z rises), distribute dE across N small Δz sub-segments using `m_writer.extrude_to_xyz`.

---

## Recommended Next Steps for Implementation Planning

1. **Decide pylon encoding** — new `ModelVolumeType::PYLON_VOID` vs. `NEGATIVE_VOLUME` + `pylon_*` config flag on the ModelVolume. Recommendation: latter for backward-compat with 3MF readers (older slicers carve out the void correctly; just skip injection).
2. **Add config keys** to `PrintRegionConfig`: `pylon_enabled` (bool), `pylon_diameter` (mm), `pylon_injection_period` (layers between events for one pylon), `pylon_stagger_offset` (layers between adjacent pylons' events), `pylon_injection_filament` (int, fallback 0 = solid_infill), `pylon_injection_dwell_ms` (cooling pause), `pylon_min_model_z` and `pylon_max_model_z` (vertical extent override). Use the existing `PrintConfig.cpp` pattern (see ZAA settings at lines 4165-4210 from Jake's recent merge for a 5-key template).
3. **Implement footprint subtraction** at Hook B (`Fill.cpp:~1307`, inside `Layer::make_fills`). Pre-compute per-Z pylon footprints (cached on PrintObject), gate on `surface.is_internal() && !is_bridge()`, `diff_ex` from the per-expolygon fill region before pattern instantiation. Skip pattern for fully-consumed regions.
4. **Add `posSchedulePylonInjection` PrintObjectStep** in `Print.hpp:94`. Implement `PrintObject::schedule_pylon_injections()` to:
   - For each pylon ModelVolume, compute its sliced footprint per Z (intersect with `Layer::lslices`).
   - Generate event list with brick-bond stagger.
   - Append events as `CustomGCode::Item` (new `Type::PylonInject`) or to a parallel `Print`-level vector.
5. **Extend `CustomGCode::Type`** at `CustomGCode.hpp:14` with `PylonInject`. Update the read/write JSON maps at lines 50-58. Plumb through `ToolOrdering::assign_custom_gcodes` (`ToolOrdering.cpp:1501-1563`).
6. **Add `LayerTools::pylon_filament(const PrintRegion&)`** at `ToolOrdering.cpp` mirroring `top_surface_filament` (lines 109-115). No `ExtrusionRole` enum change.
7. **Implement the synthesized G-code emitter** in `ProcessLayer::emit_custom_gcode_per_print_z` (`GCode.cpp:4102-4186`) for the new `Type::PylonInject` case. Use `GCodeWriter` directly to keep state coherent (`travel_to_xy`, `set_extrusion_axis` to M83 if needed, loop `extrude_to_xyz` ascending, restore Z). Model on `insert_timelapse_gcode` (`GCode.cpp:4966-5024`) for state-sync pattern.
8. **Volume math**: a small free function `pylon_dE(double r_pylon, double h_event, const Extruder&)` → `π r² h × extruder.m_e_per_mm3`. Reuses `Extruder::e_per_mm3`.
9. **GUI surface for pylon placement** — start with the simplest viable: a new "Add Pylon" entry in `GUI_Factories.cpp:332` that drops a cylinder primitive marked as `NEGATIVE_VOLUME` with `pylon_enabled = true` in its config. Defer painted/auto-placed pylons to V2.
10. **CLI-driven test fixture**: copy a small test STL into `tests/data/`, run `orca-slicer.exe` with pylon-enabled preset, write a Catch2 test that asserts the G-code output contains expected `;PYLON_INJECT` markers and that `Extruder` E-axis bookkeeping closes (no drift). No E2E framework today, so this is greenfield (~50 LOC test infra).

---

## Open Questions for Jake

1. **Pylon-as-modifier vs. pylon-as-primitive**: do you want the user to *place* explicit pylon volumes (manual, predictable), to *paint* pylon regions onto the model (high-control, GUI-heavy), or to *auto-generate* pylons from a density parameter (zero-effort, less control)? Recommendation is to start with manual placement to validate the slicer-side plumbing, then add painted/auto in V2.

2. **Pylon cross-section**: cylinders only, or also boxes/polygons? Codebase has no preference — `ExPolygon` carries arbitrary shapes through the pipeline — but the UX implications differ (single primitive vs. polygon-edit). Cylinder-only for V1?

3. **Filament constraint policy**: do we allow per-pylon filament selection (expensive — wipe tower trips), per-print single dedicated reinforcement filament (cheap — one filament change scheduling per layer), or "must match surrounding region's filament" (cheapest, most limited)? Different design implications for §9.

4. **In-layer vs. layer-boundary injection events**: the brief mentions "periodic Z intervals" — does each injection event need to coincide with a layer change (simplest, fits the existing `CustomGCode::Item` scheduler), or do you want sub-layer triggering (requires new in-layer scheduling infrastructure)? Layer-aligned is much cheaper to build.

5. **Heat handling**: combination of (a) configurable post-injection dwell, (b) reduce injection frequency, (c) something else? JanTec's work suggests injection-induced heat deformation of surrounding shell is a real concern — does the slicer need to *increase* the wall count near a pylon, or do you leave that to the user via per-region modifier-volume settings?

6. **Pylon descent rate**: are we extruding *continuously* as Z descends and then ascends (vase-mode-style continuous extrude during Z change), or "descend dry → extrude bottom-up only" (cleaner but requires a non-extruding Z travel which OrcaSlicer doesn't currently emit as a single combined-axis move)? Recommend the latter for V1 — emit a non-extruding `G1 X Y Z F` then a series of `G1 Z E F` ascending segments.

7. **3MF backward compatibility**: if pylons are encoded in 3MF and a user opens that file in PrusaSlicer or older OrcaSlicer, what should happen? The `NEGATIVE_VOLUME + flag` approach degrades gracefully (you get an unfilled hole). New ModelVolumeType degrades catastrophically (printed solid pylons). Is graceful-degrade-to-hole the right behavior, or do you want hard breakage on older slicers (forcing the user to upgrade)?

8. **Pylon-vs-support interaction**: pylons inside an overhang area — does support generation know to avoid the pylon footprint? Currently `generate_support_material()` runs after `infill()`, so it could read the pylon footprint cache, but the integration isn't free. V1 scope decision: pylons forbidden in overhangs / support regions?

9. **G-code marker convention**: do you want `;PYLON_INJECT_START` / `;PYLON_INJECT_END` comments around each event for GCodeViewer recognition (and for our own E2E tests)? Easy to add; affects no firmware behavior.

10. **Coordinate-system bookkeeping during injection**: should the injection event use **absolute E** or **relative E**, regardless of the global config? Relative is far simpler for synthesized blocks (`G1 Z… E… F…` independent of prior state). Recommend forcing M83 for injection and restoring after if config is M82.
