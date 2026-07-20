# Pylon Injection V1 Implementation Summary

> Working-tree only. Not committed. Reviewer-facing notes for Jake.

## What was built

The branch `feature/pylon-injection` (off `feature/custom-infill @ 415c7cb738`)
contains 11 commits implementing V1 of the pylon-injection feature, in the
order prescribed by the prompt:

| # | SHA          | Subject                                                                                  |
|---|--------------|------------------------------------------------------------------------------------------|
| 1 | `772ccca1f2` | add config keys (no behavior yet)                                                        |
| 2 | `93af42a425` | add `ModelVolume::is_pylon()` and `PrintObject::has_pylons()`                            |
| 3 | `bc037c172b` | per-layer pylon footprint cache                                                          |
| 4 | `b925dafe8f` | subtract pylon footprints from sparse infill                                             |
| 5 | `e7c1c17e0d` | extend `CustomGCode::Type` with `PylonInject` and event descriptor                       |
| 6 | `ae7fb96d3e` | scheduler with brick-bond, floor safety, support exclusion                               |
| 7 | `2fd74e909e` | route events through tool ordering with optional filament change                         |
| 8 | `19a02d7917` | G-code emitter with state-coherent injection block                                       |
| 9 | `e47dd54a51` | GUI menu entry for adding pylon cylinders                                                |
|10 | `8040b0d0b6` | Catch2 test for event emission and E bookkeeping                                         |
|11 | `dce4d13875` | V1 docs                                                                                   |

Each commit builds clean on Windows (Release, Visual Studio 17 2022). Final
ALL_BUILD time after all commits: incremental no-op finishes in seconds; from
a clean state it's the standard ~15–30 min slicer build.

The branch is **local only**: no `origin` push, `main` untouched
(`f8db5b8ba3`), `feature/custom-infill` untouched (`415c7cb738`).

## Architectural surprises that diverged from the recon

Two real findings that contradicted the recon, both flagged in commit messages
and resolved without restructuring the spec scope:

1. **`Model::custom_gcode_per_print_z` does not exist** as a single field.
   The recon claimed it; it's been commented out in this fork. Actual storage
   is `Model::plates_custom_gcodes` — a `map<plate_index, CustomGCode::Info>`
   (BBS's multi-plate split). The scheduler writes to
   `plates_custom_gcodes[curr_plate_index]`.

   **Consequence:** `PrintBase` got a new non-const `model()` accessor (Task 6
   commit). The existing `assign_custom_gcodes` already copies via
   `get_curr_plate_custom_gcodes()`, so the existing consumer was unaffected.

2. **`assign_custom_gcodes` consumes at most one `Item` per layer.**
   Looking at `ToolOrdering.cpp:1538-1574`, the loop's "skip past gcodes above
   midpoint" step combined with the single `lt.custom_gcode = &...` assignment
   means multiple events at the same `print_z` would be silently dropped to
   one.

   **Consequence:** Task 5 introduced a *batched* `Item` for pylons — events
   at the same snapped layer print_z are packed into one Item's `extra` as a
   JSON array (`PylonInjection::to_item_batch`/`from_item_batch`). The
   emitter unpacks and emits each. This sidesteps any change to LayerTools or
   `assign_custom_gcodes`.

Both findings are noted in commit `ae7fb96d3e`'s body in the precise text the
reviewer needs to see when reading the diff.

## TODO comments left in code

**None.** Per project conventions, no `// TODO` comments were added. V1
limitations and known gaps are documented in:

- `pylon-injection.md` (user-facing "Known V1 limitations" section)
- Per-commit log messages (architectural choices and their rationale)
- This file (judgment calls, see below)

## V1 scope judgment calls

Five places where the prompt was ambiguous or contradicted itself; the
resolution and rationale:

### 1. Pylon-vs-support: `support_layer->support_islands` vs. `SUPPORT_ENFORCER` volumes

The prompt asked for the scheduler to slot **between `posInfill` and
`posSupportMaterial`** *and* check `support_layer->support_islands` for
overlap. These conflict: `support_islands` is populated by
`generate_support_material()` running in `posSupportMaterial`, so a scheduler
running before that step can't read it.

**Resolution (Option 1, confirmed with Jake during implementation):** keep the
slot ordering as spec; the pylon-vs-support check uses user-declared
`SUPPORT_ENFORCER` ModelVolumes (XY-rect + Z-range overlap, conservative).
Logs a warning if auto-generated support (`enable_support`) is on, since
auto-support overlap is undetected. Documented as a known V1 limitation.

### 2. `pylon_stagger_offset` config vs. fixed half-event-height stagger

The prompt defined `pylon_stagger_offset` as a layer-count config key, but
later specified the brick-bond stagger as `(i % 2) * (pylon_event_height / 2)`
— a Z value, not a layer count, and a fixed 2-coloring.

**Resolution:** V1 implements the fixed Z-based 2-coloring as the prompt's
formula specifies. `pylon_stagger_offset` is declared as a config key (Task 1)
but **not yet read by the scheduler**; reserved for V2 multi-color staggering.
Documented in `pylon-injection.md`'s config table.

### 3. Filament fallback when `pylon_injection_filament = 0`

Spec says default 0 = "use `solid_infill_filament`". But
`solid_infill_filament` is per-region, and the scheduler resolves the
filament once per print (not per region) at scheduling time.

**Resolution:** V1 resolves to filament index 0 (first filament) when
`pylon_injection_filament == 0`. The `LayerTools::pylon_filament(print_config,
region)` accessor *does* fall back to `solid_infill_filament(region)` for
correctness in callers that go through that path — but the scheduler (which
needs a single filament for the whole print's events) doesn't go through the
region-aware accessor. This is the simplest semantically consistent V1.

### 4. Wipe-tower integration

The prompt's Task 7 step 3 said "implement by inserting the appropriate
filament ID in this layer's extruders and ensuring the post-event G-code
emits a tool change back to the prior filament" — but also said "if this
turns out to require restructuring the wipe-tower partition logic at
`ToolOrdering.cpp:863-882`, stop and consult Jake."

**Resolution:** Did **not** modify wipe-tower partition logic. Tool changes
happen directly in the emitter (via `GCodeWriter::set_extruder`), bypassing
the wipe tower. This means an unpurged tool change at each pylon event when
the pylon's filament differs from the layer's active filament. Documented as
a V1 limitation. The most common case (single-filament print, or
`pylon_injection_filament == 0` resolving to the same filament as solid
infill) has zero tool changes and no wipe-tower issues.

### 5. CLI slice-and-inspect verification in Task 4

The prompt's Task 4 step 2 said to "slice via CLI ... inspect the produced
G-code visually." The branch-wide constraint section says "Do not run the
slicer."

**Resolution:** Skipped the CLI verification per the global constraint.
Visual confirmation is replaced by the Catch2 test in Task 10, which slices
programmatically and asserts on the G-code output.

## Suggested manual test plan

Recommended end-to-end smoke test for Jake's local validation. The branch
hasn't been printed; all confirmation so far is at the slicer level.

### 1. Trivial sanity check (no GUI)

Build, then run the Catch2 tests:

```bat
cd build
ctest -C Release -R "PylonInjection" --verbose
```

Expect: 4 test cases pass (one positive marker test, two no-op tests, one dE
arithmetic test). If any fail, the scheduler or emitter is broken before any
physical print is attempted.

### 2. Visual G-code inspection (no print)

Load OrcaSlicer. Open any simple object, e.g. a 30 × 30 × 30 mm cube.

1. **Print Settings → Quality → set `pylon_max_descent_depth` to 8 mm.**
   (Adjust to your real nozzle clearance later; 8mm is safe for most printers
   with a 0.4mm nozzle.)
2. Object right-click → **Add Pylon (cylinder)**. A 10×20mm negative cylinder
   appears. Reposition to roughly the centre of the cube, lower its top so
   it's inside the cube.
3. Slice.

Expected G-code patterns to look for:

- Search the exported `.gcode` for `;PYLON_INJECT_START`. You should see
  multiple of them — roughly `(cube height inside pylon) / max_descent_depth`
  events per pylon, spaced `pylon_injection_period` layers apart.
- Each `;PYLON_INJECT_START` should be followed within a few lines by a
  pair of G1 moves (the dry XY+Z descent), a sequence of G1 moves with
  ascending Z and non-zero E (the extrusion), a `G4 P<dwell_ms>` line, and a
  `;PYLON_INJECT_END`.
- The infill of the cube on layers AT the pylon's Z range should have a
  visible cylindrical hole in the preview where the cylinder sat. Top and
  bottom solid surfaces should NOT have holes (they cap the pylon).

### 3. Safe-default smoke test

Same setup, but **leave `pylon_max_descent_depth` at 0** (the default).

Expected: G-code contains no `;PYLON_INJECT_*` markers. The slicer log shows:
> `pylon-injection: pylon volumes present but pylon_max_descent_depth is 0; all injection events disabled (safe failure mode).`

This is the safe failure mode that prevents nozzle crashes on prints sliced
without explicit clearance configuration.

### 4. Physical print (small test cube)

When ready to actually print:

- Use a **2cm × 2cm × 4cm** cube with a 5mm-radius pylon cylinder centred,
  vertical extent from `Z=3mm` to `Z=35mm`.
- 0.4mm nozzle, PLA, 0.2mm layer height. 3 walls, 15% gyroid infill, top/bottom
  3 layers.
- `pylon_max_descent_depth = 4mm`, `pylon_step_height = 0.3mm`, default
  speeds, `pylon_injection_dwell_ms = 1500`, `pylon_injection_period = 10`.
- `pylon_injection_filament = 0` (same filament as solid infill — keeps V1
  simple).

What to watch for during the print:

- Each injection event should be a deliberate, visible descent + slow
  extrude. You'll see the toolhead drop ~4mm into the cube, then climb back
  while extruding.
- After the dwell, normal printing should resume cleanly from the layer's
  next path. No spurious travel or retract.
- Watch the layer immediately above each injection for any visible
  deformation of the surrounding shell. If it's significant, increase wall
  count or increase `pylon_injection_dwell_ms`.

What to inspect after the print:

- Top of the cube should look normal — the top shell layers cap the pylon.
- Cut or X-ray the cube. The pylon volume should be a solid cylinder of plastic,
  not hollow. Surrounding sparse infill should be unaffected outside the pylon
  XY footprint.

## Out-of-scope follow-ups for V2

- Auto-support geometric overlap check (currently warning-only)
- Polygonal / box / mesh pylons (currently cylinder-only)
- Painted pylon placement
- Auto-generated pylon distribution
- Per-pylon filament variation with wipe-tower integration
- Sub-layer event scheduling
- Automatic shell-thickness increase near pylons
- Multi-color brick-bond beyond 2-coloring (uses the reserved
  `pylon_stagger_offset` config key)
- 3MF version-aware schema for pylon_enabled flag (currently relies on
  graceful degradation in older readers)
