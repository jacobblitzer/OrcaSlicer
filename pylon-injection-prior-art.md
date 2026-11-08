---
title: "Z-axis reinforcement by void injection: prior art, evidence and patents"
date: 2026-11-07
tags:
  - research
status: completed
project: pylon-injection
component: libslic3r/PylonInjection
related-bugs: []
related-decisions: []
---

# Z-axis reinforcement by void injection: prior art, evidence and patents

## Research Question

Has anyone else successfully built Z-axis reinforcement ("z pylons", z-pins, rebar,
vertical reinforcement injection) into an FDM slicer, and does the technique actually
work? Specifically:

1. Does a slicer-integrated implementation exist, and did it get accepted?
2. What does the published literature demonstrate, with numbers?
3. What has the maker community tried, and what is the counterargument?
4. What is the patent position?

## Context

`feature/pylon-injection` was written in May and July 2026, never merged, and never
built. It was ported onto 2.6.0-dev on 2026-11-07 (commits `8bcb6f00d6`, `9a9cac85f9`)
and now compiles and ships in the local build. Before investing further it is worth
knowing whether this ground is already covered, whether the physics supports the
approach, and whether anyone owns it.

The design under comparison: a user-marked cylindrical `PYLON_VOID` volume type carves
the parent like a negative volume; per-layer footprints are cached and subtracted from
sparse infill; injection events are scheduled bottom-up in a staggered brick-bond
pattern with a floor-safety descent limit and support exclusion; events emit as custom
G-code with an optional filament change.

## Sources Consulted

Searched and read: GitHub (OrcaSlicer, PrusaSlicer, CuraEngine, BambuStudio, Slic3r,
SuperSlicer and forks; issues, PRs, discussions, code search), the ORNL/UTK z-pinning
paper series via OSTI and the UT Austin SFF proceedings, TU Delft repository, ASME
proceedings, Google Patents, CNC Kitchen's instrumented test blog, Hackaday (articles
and comments), RepRap blog, Bambu Lab forum, trade press (CompositesWorld, Metal AM,
engineering.com, Fabbaloo, 3druck, Adafruit).

Not reachable, and therefore gaps in this report: Reddit (refused the fetch agent),
Voron Discord, YouTube comment bodies, `web.archive.org`, one Fabbaloo article (HTTP
403), and — notably — the 2005 RepRap blog post that anchors the "decades old" claim
(socket closed on repeated attempts; its content here is second-hand from a search
index).

Verified directly from the GitHub API rather than taken on trust: the state and size of
PRs #12771, #14122 and #8181, and that `SoftFever/OrcaSlicer` now resolves to
`OrcaSlicer/OrcaSlicer`. Everything else is relayed from research agents and should be
re-checked before being relied on, particularly the patent readings.

## Findings

### Slicer implementations

The repository moved from `SoftFever/OrcaSlicer` to `OrcaSlicer/OrcaSlicer`
(16,372 stars, default branch `main`). Searches against the old name silently return
nothing, which is likely why this looked like unexplored ground. Our `upstream` remote
still uses the old URL and works only via GitHub's redirect.

Two slicer-integrated implementations exist. Both are in OrcaSlicer, both were built in
2026 independently of each other, both are open and unmerged, and **neither has
received a single maintainer review**.

| | #12771 Z-pinning | #14122 Magma | Onorato post-processor |
|---|---|---|---|
| Slicer-integrated | Yes | Yes | No (CAD + post-process) |
| Size | 1,615 / −238, 27 files | 12,037 / −427, 107 files | 3 Python modules |
| Mechanical data | +30–47% Z, self-reported | **None** | Yes, MSc thesis |
| Prints successfully | Claimed | Yes, with documented failures | Yes (airbrush nozzle) |
| Material change | No | Yes, "mostly untested" | No |
| State | OPEN, 0 reviews, since 2026-03-14 | OPEN **draft**, 0 reviews, since 2026-06-09 | standalone, 0 stars |

**[#12771 "Add Z-pinning for inter-layer mechanical interlocking"](https://github.com/OrcaSlicer/OrcaSlicer/pull/12771)**
by `Deitiker`. Architecturally the closest to ours: a new `Fill/FillZPin`, void
subtraction from internal fill surfaces only (`m_z_pin_void_indices` per layer), and a
"Bridson scheduler" that places pins at minimum spacing and staggers expiry by graph
colouring — our brick bond, solved differently. Config keys `enable_z_pins`,
`z_pin_spacing`, `z_pin_depth`, `z_pin_diameter`, `z_pin_fill_pct`, `z_pin_feedrate`,
`z_pin_style`. Differs from ours in that pins are an **automatic grid** over the whole
part, with modifier meshes only able to *suppress* them; no material change; no
floor-safety or support exclusion. Author reports ASTM D638 Type IV PLA dogbones at
+30% and "average 47%" Z-strength, self-reported and not replicated.

**[#14122 "Add Magma vertical reinforcement infill system"](https://github.com/OrcaSlicer/OrcaSlicer/pull/14122)**
by `MGunlogson`, project page <https://mgunlogson.github.io/magma/>. An order of
magnitude larger. Triangular-lattice infill with sealed vertical **U-tube** channels: a
solver pairs each cell with a shared-edge neighbour and cuts a window in the wall
between them at the bottom, so the nozzle dives into one leg and **air escapes the
other**. Adds Google OR-Tools CP-SAT for tube assignment, 44 new settings, multi-material
and dual-extruder injection. Runs on a stock Ender 3 with a 0.6 mm E3D V6. ~100 prints,
cutaways and photos — and **zero strength data**, which the README says outright:
"Help wanted: Strength numbers: Magma vs solid infill at the same mass." The author
filed a CC0 defensive publication (6 May 2026) before opening the PR.

**[z-pinning G-code post-processor](https://github.com/LorenzoOnorato/z-pinning-gcode-post-processor)**
— Lorenzo Onorato, TU Delft MSc thesis. The only *validated* implementation. Injects at
`;LAYER_CHANGE`; three phases per pin (travel with Z-hop → nozzle dive into deposited
material → pin extrusion → wipe); parameters include `flow_ratio` up to 165%,
`diving_mode`, `geometrical_extrusion_enabled` for a rivet shape, and `stagger_params`
described as eliminating "aligned seams (weak points) in the load path". Critically it
**does not create the cavities** — that must be done in CAD beforehand — and it needs an
airbrush nozzle to enter them. Validated on Vectra A950 LCP, modified Ultimaker 2+.

**[PinLoK](https://github.com/The-Dorkknight/a-bonding-experience)** — simulation plus
post-processor, not slicer-integrated. The author states up front that every strength
number for his own ideas is **theoretical, from a 2-D simulation**, not measured.
Reported +16–27% vs solid infill at equal weight is simulated. One finding worth
stealing: pins terminating **mid-layer rather than on a layer line** did far better in
simulation.

**Clean negatives.** Nothing in PrusaSlicer, Cura, Bambu Studio, Slic3r, SuperSlicer,
IdeaMaker or Simplify3D. [CuraEngine#1034](https://github.com/Ultimaker/CuraEngine/issues/1034)
"Feature Request: Z pinning" has been open since 2019-04-11 — Ultimaker devs engaged,
identified the ORNL paper, and dropped it; seven years, zero code. OrcaSlicer
[#7119](https://github.com/OrcaSlicer/OrcaSlicer/issues/7119) "Injection molding while
printing" (2024) described our concept almost exactly and was closed `NOT_PLANNED` by a
stale bot, not on merit. GitHub code search for `z_pin_spacing` / `z_pinning` returns no
other slicer hits.

**Appetite signal.** The [Brick Layers PR #8181](https://github.com/OrcaSlicer/OrcaSlicer/pull/8181)
has been open since 2025-01-25 and was still being touched on 2026-11-08 — 22 months
unmerged. PrusaSlicer's z-dither PR (#10391, real precedent for deliberately moving the
nozzle below the current layer top) died after 10 months. No Orca maintainer has engaged
with any Z-strength feature.

### Published literature

"Z-pinning" is the correct term. The line is ORNL plus University of Tennessee
(Chad Duty's group), 2017–2023, DOE-funded. The peer-reviewed anchor:

> Duty, Failla, Kim, Smith, Lindahl, Kunc (2019), "Z-Pinning approach for 3D printing
> mechanically isotropic materials," *Additive Manufacturing* 27:175–184,
> DOI 10.1016/j.addma.2019.03.007

**The published mechanism is software-only on stock hardware.** Holes are left by
skipping extrusion at aligned x–y locations; before the next layer the head parks
**above** a hole and **remains stationary** while extruding a measured volume, and the
melt falls to the bottom. Pins are staggered A/B so seams never align. ORNL ran this on
a bone-stock MakerGear M2 with hand-written MATLAB G-code, and states the process
"can be accomplished with only software modifications to the slicing" software.
Nomenclature: "n-i pins" = pin n layers long, deposited every i layers; 8-4 is the
workhorse. Fill factor = extruded volume / hole volume, 80–160%; above 100% only works
in sparse infill, where overflow squeezes between layers and interlocks.

Bonding is **mechanical interlocking, not re-melt diffusion** — the 2017 paper says the
extrudate interlocks with the scalloped wall "regardless of whether any traditional
re-melt molecular bonding occurs." Hence the proposal for dissimilar materials. A
"hot pin" extruded 20 °C hotter bought a further +10%.

Headline results, on **35% sparse infill**:

| Study | Material | Result |
|---|---|---|
| 2019 *Addit. Manuf.* | PLA and CF-PLA | **3.5× UTS_z**, >4× toughness at 120% fill |
| 2019 *Addit. Manuf.* | PLA | 52% X-to-Z drop → **<10%, statistically isotropic** |
| 2018 SFF | 15% CF-PLA | 2.8 → 7.2 MPa (+157%); **>20× toughness** at 160% fill |
| 2017 SFF | neat PLA | +20% UTS_z, +100% toughness, stiffness unchanged |
| 2023 FE model | CF-PLA | +40% modulus and strength |

Independent work is thin: one ASME conference paper (Clemon & Christopher 2021, UTS —
G-code post-processing on a consumer printer, up to +25% failure stress and +300%
toughness in Z, but flexural strength unchanged once normalised by mass) and two TU Delft
theses, **one of which is a null result** (Ulyanov 2023: z-pinning did not meaningfully
toughen LCP, attributed to the pin's sub-optimal post-extrusion shape; matchstick and
sawtooth approaches beat it). No independent peer-reviewed journal replication.

### Where the technique loses

This is the part the secondary coverage omits, and it is decisive.

- **The 3.5× is against 35% sparse infill, not against printing solid.** The 2019 paper's
  own words: the pinned sample's toughness "was more than 4x higher than the unpinned
  sample, but was still ~3x lower than the solid infill control." CF-PLA at 120% pins
  still had lower UTS_z than plain neat PLA.
- **At equal mass, denser infill won.** 2017, neat PLA: an unpinned **57% infill** sample
  at the same mass was **~60% stronger than any pinned sample**. This reverses for
  CF-PLA (pins ~50% better than equal-mass infill), so the fibre matters.
- **At BAAM scale, no pins at all won.** 2018 CF-ABS beams, 3-point bend: solid 5,089 lb;
  5 circular pins 4,252 lb; **an infill pattern that merely mimicked the pin geometry,
  with no pins, 8,984 lb** — 61% better than pinned.
- **On solid cross-sections the dwell time eats the benefit.** 2019 SFF, CF-PLA: plain
  solid 33 MPa; a time-matched **pause-only** control (55 s every 4th layer) collapsed to
  **12 MPa**; pinned recovered to 17 MPa — about half the plain solid print. The pin pass
  cost >50% on top of a 1 m 54 s layer.
- **Settings recover most of the Z deficit for free.** CNC Kitchen's instrumented tests:
  extrusion temperature alone takes PLA layer adhesion from **20 MPa at 190 °C to ~40 MPa
  at 230 °C**; slow cooling with fine layers takes PETG from 65% to **75–93%** layer
  adhesion; extrusion width peaks around 150%. Annealing, by contrast, does **not** fix
  layer adhesion — 42 kg before, 42 kg after, repeatedly.

### Design envelope and failure modes

Kim et al. 2020 (SAMPE, DOI 10.33599/s.20.0169) cross-sectioned 60 CF-PLA samples and
produced a printability map. Hole width and depth are normalised by nozzle diameter:

```
80% pin volume          100% pin volume         120% pin volume
depth\width 3  4  5  6  depth\width 3  4  5  6  depth\width 3   4   5   6
    4      OK  G  G  G      4        G  G Gov Gov   4        B   G  Gov Gov
    6      OK  G  G  G      6       OK  G  G   G    6       N/A OK  Gov Gov
    8       B OK  G  G      8        B  G  G   G    8        B   B   G  Gov
   10          B  B  G     10           B  B   G   10        B   B   B   B
```

A 3× hole is already marginal and goes bad past depth ~6; a **6× hole (3 mm on a 0.5 mm
nozzle) is needed to reach depth 10**, and even that fails at 120% fill. Raising pin
volume turns marginal pins bad via overflow.

Two canonical defects, named in the papers:

- **"Rope in a bucket"** (underflow) — hole too wide for the nozzle, the extrudate loses
  Z-alignment and coils, leaving a string of voids down the pin.
- **"Short shot"** (bridging) — hole too deep or narrow, the extrudate touches the side
  wall before reaching the bottom, bridges across and **blocks further penetration**.

Observed consequences: overflow builds a hardened nodule, **the nozzle runs into it, the
build steps and the print falls over**; overfill gets dragged between pin locations as
strings; at BAAM scale pins **pulled out intact** rather than shearing; the **seam between
stacked pins is always the weak link** — "the resulting bond can be no better than the
bond between successive layers in a traditional print" — which is simultaneously the
strength ceiling and the toughness mechanism, since the crack stair-steps from seam to
seam.

**Depth is limited by time, not geometry.** Nasirov 2023, verbatim: "the adhesion strength
at the interfaces decreases with the increase in hole depth as the material is being
extruded after larger time intervals. Also, the defects in z-pins increase with the
increase in hole depth." Their FE model shows depth as neutral only because it assumes
constant adhesion and zero defects, which they state.

Corroborating this from the implementation side: Magma's ~100 prints put the cliff at
**1.5 s injection clean, 2 s deforms the lattice, 3 s destroys it**, with nozzle-heat
seal failure as the dominant mode, capping tube height at 3–4 mm. JanTec Engineering
(Feb 2025) found independently that a deep empty cylinder **cannot be filled in one
pass** — the melt freezes before reaching the bottom, so it must be filled periodically
bottom-up — and that hotter injection fills better but deforms the print.

### Patents

**Treat this section as a pointer for a proper FTO review, not as a legal reading.**

- **US 2018/0311891 A1** "Z-axis improvement in additive manufacturing" (UT-Battelle;
  Duty, Kim, Kunc et al.; filed 2018-04-27, priority 2017-04-28) — the core z-pinning
  process. **Listed as abandoned**, ~2020. Note its Examples describe *lowering the
  nozzle into each void and extruding*, which conflicts with the papers' "remains
  stationary".
- **US 11,623,395 B2** "Penetrating and actuating nozzle for extrusion-based 3D printing"
  (UT-Battelle) — **granted 2023-04-11, active, expiry ~2038**. Claim 1 covers a nozzle
  with a penetrating extension sized substantially equal to the void diameter, inserted
  into the void, depositing material **as the nozzle is withdrawn**. Claim 12: extension
  length ≥50% of void depth. Also covers heating the extension to reflow walls, rotating
  it, and an extraction platen.
- **US 12,576,581 B2** (Nanjing University of Aeronautics and Astronautics, granted
  2026-03-17) — pressure-injected thermoset into a continuous-fibre grid.
- **EP 4100235 B1** — AIM3D's VoxelFill, the commercial near-isotropy process
  (claims 80–100% of in-print-direction tensile strength, vendor figures only). JanTec
  explicitly disclaimed against it.
- Not relevant despite search hits: US 11,566,349 B2 is US Army, a sacrificial shell plus
  anneal.

## Analysis

**The idea is old and the evidence is narrower than its reputation.** The 2005 RepRap blog
post proposing blind Z holes filled by the write head, explicitly likening varying hole
shape to "rebar in concrete", is the anchor of the folklore — and it was a suggestion
with no test. Twenty-one years later there is exactly one peer-reviewed paper series,
from one group, plus one conference paper and two theses independently.

**Two implementations work; neither is validated and integrated at once.** #12771 has
numbers but no venting story and no material change. Magma has the architecture, venting
and multi-material but zero strength data and a known seal-melting failure. Onorato has
rigorous validation but no slicer integration and needs an airbrush nozzle. The specific
gap nobody has closed is **a slicer feature plus break tests showing a net win over solid
infill at equal mass**.

**Our descent is the risky part, in two senses.** Physically, the literature's method
keeps the nozzle above the hole and lets gravity fill it; descending is the variant that
needed custom hardware (a 5 mm dispensing needle) and has no published mechanical
results. Legally, descent is what the *live* patent claims, while the *abandoned* one
covers the stationary-above method. `pylon_max_descent_depth` is therefore both our
safety valve and our exposure.

**Our differentiators are real but unproven.** A user-marked `PYLON_VOID` answers by
construction the objection #12771 was hammered on by an active Orca contributor — "it
makes no sense to inject material into the thick part of the model… but it makes sense to
strengthen the thin elements." Neither PR has floor-safety limits or support exclusion.
Only Magma attempts a material change, untested. These are genuine, and none of them is
the thing that would change a maintainer's mind, which is data.

**Upstreaming prospects are poor on current evidence.** Brick Layers: 22 months. #12771:
8 months, zero reviews. PrusaSlicer's z-dither: dead. The lever is measured data plus the
differentiators, and coordinating with `Deitiker` and `MGunlogson` — both active, both
responsive to feedback, both asking for testers — is probably higher-value than a third
independent PR.

## Conclusions

1. **Someone else has done this, twice, in OrcaSlicer, in 2026.** Not shipped, not
   reviewed, not validated.
2. **The technique's honest claim is isotropy at reduced mass, not stronger-than-solid.**
   Its own authors' solid-infill controls beat it.
3. **Equal-mass infill beat pins in two separate ORNL comparisons**, and settings alone
   recover 75–93% isotropy for free. Any pitch for this feature has to answer that.
4. **Venting a blind cylinder is unsolved.** Asked in 2024, still unanswered except by
   Magma's U-tube and a slot proposal.
5. **Depth is a thermal/time budget, not a layer count** — and overflow nodules cause
   nozzle collisions that topple prints.
6. **The core process patent is abandoned; the plunging-nozzle patent is live to ~2038**
   and is nearer our mechanism. Needs a real FTO look before this goes public.

## Recommended Actions

- [ ] Get a break test. Pylon vs solid infill **at equal mass**, same machine, same day.
      Without it this feature cannot answer its strongest rebuttal, and with it the branch
      becomes the only validated slicer-integrated implementation in existence.
- [ ] Solve venting. Decide between a Magma-style U-tube, pvt02's slot ("Z-canyoning"),
      or a vent channel — a blind cylinder has nowhere for displaced air to go.
- [ ] Re-derive `pylon_max_descent_depth` from a time/thermal budget rather than layers,
      using Magma's 1.5 s / 2 s / 3 s cliff as the starting envelope.
- [ ] Key void sizing off **grid cell size, not infill density**, and handle Orca's infill
      multiline — a single-line grid wall deforms under injection pressure. This exact
      objection was put to #12771 and will be put to us.
- [ ] Hold the published envelope: hole 4–6× nozzle diameter, depth ≤8–10×, fill ≤100%
      on anything that is not sparse, seams staggered A/B.
- [ ] Try pins terminating mid-layer rather than on a layer line (PinLoK's simulation
      found this materially better; cheap to test in the stagger logic).
- [ ] FTO review of US 11,623,395 B2 before publishing anything, given our descent.
      Magma's CC0 defensive publication (2026-05-06) is prior art to point at.
- [ ] Update the `upstream` remote to `OrcaSlicer/OrcaSlicer`; the old URL only works via
      GitHub's redirect, and searches against the old name return nothing.
- [ ] Consider contacting `Deitiker` (#12771) and `MGunlogson` (#14122) rather than
      opening a third independent PR.
- [ ] Close the research gaps: Reddit, Voron Discord and the 2005 RepRap post were all
      unreachable here and remain unchecked.

## Related

- Bugs: []
- Decisions: []
- Specs: [pylon-injection.md](pylon-injection.md),
  [pylon-injection-touchpoints.md](pylon-injection-touchpoints.md),
  [pylon-injection-impl-summary.md](pylon-injection-impl-summary.md),
  [feature-pylon-injection-recon.md](feature-pylon-injection-recon.md)
- Commits: `8bcb6f00d6` (port onto 2.6.0-dev), `9a9cac85f9` (build fix)
