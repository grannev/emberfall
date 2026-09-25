# Emberfall agent guide

Emberfall is a C11 + raylib side-view sandbox built around a large cellular
world. raylib is the platform, input, rendering, and audio backend; the
long-term boundary is a headless-testable gameplay core below those systems.

Start with `README.md`, then `docs/README.md`. `docs/adr/` records why the
current architecture is what it is, including approaches that were implemented,
measured, and rejected — read the relevant record before undoing one of them. Detailed system documents contain
the historical reasons behind non-obvious tuning and prior bug fixes; read the
relevant one before changing simulation, player movement, abilities, or
rendering. Performance measurements live in `docs/performance.md`. This file is
the maintained successor to the former `CODEX.md`.

## Developer commands

```sh
make                 # release
make run
make debug
make test            # headless regression suite
make bench           # deterministic 10-scenario benchmark plus lighting
make asan
make ubsan
make profile
make compile_commands.json   # for clangd
xvfb-run -a make run RUN_ARGS=--smoke-test
make run RUN_ARGS="--seed 0x1234"   # replay a reported world
```

## Invariants

- The gameplay simulation advances at a fixed 60 Hz step.
- The world wraps: its right edge is joined to its left, like a planet.
  `WorldIndex`/`WorldCell` take a column modulo the width (one unsigned
  compare on the fast path), `WorldInBounds` checks rows only, chunk columns
  wrap, and nothing clamps a column to the map. Entities hold unwrapped
  positions: the character is moved back a whole width when it crosses the
  seam and `GameKeepInWorld` keeps every body and particle within half a
  turn of it; `GameState.wrapShift` tells presentation to move the camera
  and its effects by the same amount, and `Renderer.travel` keeps the sky
  and backdrop continuous. Generation is periodic in x (noise lattices and
  biome regions are whole numbers round the world). The light solve copies a
  window that crosses the seam into planes of its own; the light texture
  repeats across. Top and bottom are still the ends of the world.
- Falling cells are processed bottom-to-top; horizontal traversal alternates.
- `updatedTick` prevents a moved cell from updating twice in one tick.
- Cell mutations wake only the affected chunk and crossed borders; generated
  distant dynamics remain asleep until streamed into play.
- The simulation schedule is a flag array plus a compact per-row list of active
  chunk columns, and the two must always agree; the single scheduler is private
  to `world_storage.c`, so wake a cell rather than reaching for the schedule.
  The set simulated by a tick is frozen at its start, so a wake raised during a
  tick schedules the next one.
- Fire and lava heating stay local. Passive lava cannot melt its rock lining.
  A capped source holds what it touches (`Cell.heatHeld`), so a lining rests
  exactly on the cap and its chunks sleep; never reintroduce the heat/cool
  sawtooth that kept every lava pocket awake for the whole session.
- A surface liquid cell takes a drop, slides only over other liquid in the
  direction it slid last, and gives up after `WORLD_LIQUID_WANDER_LIMIT`
  slides; a cell under pressure spreads as before, and only toward lower
  pressure. A settled pool and its shoreline must go to sleep — check with
  `activeChunkCount`, not by eye, because the motion this prevents is invisible.
- Liquid is one cell per unit and mass is conserved by construction: motion is
  `WorldMoveCell`, a swap, and nothing else creates or destroys a liquid cell.
  Pressure is a head kept in the liquid's `lifetime` (`world_fluid.h`),
  propagated from neighbours with a loss per hop — the loss is what lets a
  stale head die, never remove it — and a column whose bottom carries more head
  than its depth explains is lifted, taking from the foot of the surface that
  pushes. A moved liquid cell's head is cleared: carried along, a deep cell's
  head arriving in a shallow column reads as pressure there and every lift it
  causes carries another. Momentum is a bounded impulse queue in `World`, not a
  per-cell velocity; the cell must not grow for it.
- An empty cell has no temperature: the field is ignored and reads as
  ambient. That is what lets the cell array stay unwritten above the ground,
  so never initialise it, and never `memset` it on regeneration — take a fresh
  `calloc`. The ground is laid out in the bottom `WORLD_GROUND_ROWS` rows and
  described as fractions of that band; everything above is sky and free.
- The per-chunk water and lava counts gate the reaction scan. Only
  `WorldSetGeneratedCell`, `WorldSetCellRaw` and `WorldMoveCell` write a cell's
  material, and each moves the counts; a count that is ever too low is a
  reaction that never happens.
- Player collision uses substeps and must not tunnel; boost drilling must not
  leave the collider embedded. The substep count follows the displacement and is
  capped, so one frame's work is bounded however fast the player is going.
- The drill cuts the frame's whole path in one swept carve, before the collision
  substeps run. Cutting per substep re-carved almost the same ground sixty times
  a frame, and — worse — left the collider's leading edge against fresh material
  every step, so a boosting player bounced off the walls of their own tunnel.
  Clear the corridor first, then travel down it.
- A boosting player cuts through a terrain body exactly as they cut through the
  static world, via TerrainDamage. A detached slab must not be the one thing in
  the game that stops a drill boring through bedrock beside it.
- Drill heat is a fraction of each material's own phase threshold, never an
  absolute temperature: rock melts at 720 and dirt catches at 175, so one number
  either fails to tint the rock or sets the dirt alight.
- An ability with no row in the input binding table has no player control. That
  is how a mechanic stays in the engine — reachable by tests and by world
  reactions — without being something the player can fire. Explosion is exactly
  that, and must not regain a binding.
- Beams start at `PlayerBeamOrigin`, used by the gameplay ray and the drawn beam
  alike. A beam cast from the chest and drawn from the head reads as a bug the
  moment the player aims down.
- Nothing slows the player in a liquid, and nothing may start to: no flat
  multiplier, no drag however physical — both were tried and both were
  removed at the player's request. Flying through the world is the point of
  the game and no material punishes it. The liquid pays instead
  (`fluid_interaction.c`): a fast entry throws a crown, a fast low pass
  throws up a band of water several rows deep (and boils the surface at
  sonic speed), a dive at speed leaves a wake, and at drill speed the water
  in the corridor flashes to steam. When a reaction is asked to be bigger,
  move more cells — never add a drawn effect in its place. A body in liquid gets buoyancy from
  density alone (`terrain_fluid.c`), drag before buoyancy, and its splash is
  the water itself — never a drawn effect over it.
- A cave-in (`terrain_stability.c`) is asked only where the destruction log
  says something was cut, and it brings a roof down as slabs: a failed run is
  cracked off its supports so the detach check extracts the roof between the
  cracks as a body, and only the cracks and the ceiling row become rubble. A
  grain never anchors a component and a loose fragment that touches only at
  a corner goes with it; a body that cannot be split for want of a slot is
  marked `fracturePending` and split when one frees, never left as two rocks
  moving as one. Bodies are welded back only when every cell has a place,
  and water under a body is lifted to the surface, never destroyed.
- Re-entry (`atmosphere.c`) lives in the band between the space and cloud
  lines, read from `WorldGravityScaleAt` so the air starts where the pull
  does. Below `entrySpeed` — above the character's cruise — the air does
  nothing at all: flight without boost never burns. The character is heated
  there and never slowed — the air is a material like any other; a body is braked and its leading face heated
  through `TerrainDamageTemperAround`, and cools out of the band. The world
  is 4096 tall so that leaving and coming back are journeys: a corridor of a
  thousand cells, and five hundred of open space above it.
- A liquid cell under liquid stores no head: its head is read by walking up
  its column, bounded. Only a cell with rock over it stores one. Storing the
  chain sent a wave of head changes through an ocean after one blast and
  woke it for two hundred ticks; measure any fluid change on a real ocean
  (`docs/performance.md`, EF-WLD-011), not only on a tub.
- Thrust is decomposed along and across the direction of travel: forward is
  acceleration, backward is braking and is worth more than acceleration, across
  is steering and is what speed takes away. Steering authority falls to a
  fraction at top speed and never to zero — losing control entirely is not the
  price of going fast. Braking never carries the velocity past zero in one
  frame, which is what stops a hard brake reading as an instant reversal.
- World mutations, particles, and events use persistent/fixed-capacity storage;
  normal update and render loops must not allocate from the heap.
- Gameplay randomness is seeded and explicit. A seed plus a sequence of
  `GameInput` values must replay identically. Never call `GetRandomValue` from
  gameplay code — draw from the owning system's `Rng` (see `src/rng.h`).
  Presentation-only jitter may still use raylib's generator.
- Biome order, terrain noise and feature descriptors are derived from seed plus
  coordinates. Do not reintroduce one sequential generation stream whose draw
  order makes adding a cave move every later lake; biome boundaries must keep
  their blended, seam-free surface contract.
- Preserve the original Emberfall character and gameplay. References are not a
  license to copy another game's sprite, UI, assets, levels, or lore.

## Change discipline

Add a regression test before altering subtle existing behaviour. Measure hot
paths with `make bench`, and prefer workload counters over machine-specific time
assertions. Keep public headers small, ownership explicit, arrays contiguous,
and dependencies directed from app/presentation toward gameplay core. Do not
introduce a generic ECS, event bus, allocator, or raylib wrapper without a
measured project-specific need.

A code change is done when the relevant release/debug build, tests, sanitizer,
benchmark, and smoke-test pass; warnings remain zero; dead transitional code is
removed; and README/AGENTS/docs match the actual architecture. Commit each
coherent phase with an explanatory message.

## Current engineering audit

- The immutable baseline and confirmed hypotheses are recorded in
  `docs/performance.md`; do not rewrite the baseline after an optimization.
- `World.lastTickStats` exposes processed cells/chunks for non-flaky performance
  regression checks. `make bench` uses ten fixed scenarios on the production
  world size; timing assertions do not belong in tests.
- Baseline CPU allocation was 275.12 MiB before GPU state: 216 MiB cells,
  54 MiB persistent pixels, 5.06 MiB lighting, and minor metadata. The
  persistent pixel buffer is gone, `Cell` is packed to 12 bytes, and the sky
  above the ground band is never written, so the 16384x4096 map is 768 MiB
  virtual and about 162 MiB resident. The giant world texture is gone as well:
  `WorldRenderer` keeps a cache of 256x256 pages and only the visible ones are
  resident, so world size is no longer bounded by `GL_MAX_TEXTURE_SIZE`.
- The benchmark never activates generated lakes and calderas, so a "settled
  world costs nothing" reading from `make bench` says nothing about a long
  session. Sweep the activation window across the whole map and count what
  stays awake (see EF-PERF-001 in `docs/performance.md`) before believing that
  a change to the simulation lets the world sleep.
- Refactoring is deliberately phased. Do not combine game/input/events, world
  decomposition, Cell layout, active scheduling, and render paging into one
  rewrite. Keep every intermediate commit playable and measured.
- `GameState` now owns gameplay state and fixed-step orchestration. Gameplay
  receives `GameInput`; only `input.c` polls raylib controls. Transient feedback
  crosses the boundary through the fixed-capacity `GameEventBuffer`; add event
  types there instead of another one-frame presentation flag in `main.c`.
- `Renderer` owns presentation composition and `WorldRenderer` owns all GPU
  world state. Player, ability, and particle drawing live in dedicated renderer
  modules; simulation modules must not regain `Draw*` calls. `World` owns CPU
  cells/chunks/lighting only and remains valid in headless tests.
- `Renderer` owns full-resolution scene/emissive targets, two half-resolution
  bloom targets and the downsample/blur shaders. Five offscreen passes preserve
  the sharp scene and blur only explicit emission; missing shaders or bloom
  targets fall back to the sharp scene. Resources are reused in steady state
  and recreated only on resize. HUD remains a backbuffer overlay; gameplay must
  not gain render-target or shader dependencies.
- `SpaceRenderer` owns the space backdrop's textures (nebula, two star
  layers, a ringed giant), built once from the seed; it draws in screen space
  behind everything, faintly at night and fully as the view leaves the air
  (`EnvironmentRendererSpaceAmount`). Stars are never drawn in world
  coordinates again: scrolling one for one with the ground, they read as
  specks in front of the player.
- `EnvironmentRenderer` is renderer-owned presentation state. Its 51 bounded
  procedural descriptors (four ranges of continuous ridge lines are drawn
  from noise, not descriptors) and palette are derived from the world seed without
  consuming gameplay RNG; it draws into the existing scene/emissive passes and
  must never read or mutate `GameState`/`World`. Empty world pixels deliberately
  retain a depth-dependent translucent tint so the background can show through.
- Each resident world page has scene and emissive textures. One 8 KiB stack
  staging pair builds both from a dirty chunk; `MaterialInfo.emission` and hot
  solids enter bloom, ordinary bright terrain does not. Particle emission is
  explicit presentation metadata and must be reset whenever a pool slot is
  reused.
- The light solve skips open sky: the downward sweep starts at the first row
  with opacity, emission or the lamp, and the upward sweep stops once a row
  of ember in open air is below half a step of the 8-bit texture. Anything
  that starts to glow in the sky must reach the field through emission or
  the lamp, or the skip will not see it.
- A cell's tone is `Cell.shade`, six bits beside the two `heatHeld` uses, so
  `Cell` stays 12 bytes. It is given when a material is written and carried
  by every move, by extraction into a body's raster and back by a weld —
  colour by position made falling grains flicker and slabs change colour as
  they came loose. Palettes and patterns are table columns in `materials.c`
  (`dark`/`light`/`accent`/`pattern`); faces and liquid depth come from the
  neighbours the page builder already walks.
- Page pixels are unlit. `LightRenderer` uploads the world's coarse light
  field into a small texture and its shader lights pages and terrain bodies by
  world position, so a moving lamp or a turning day never rebuilds a chunk.
  Do not bake light back into pixels: that was 7.6 ms of every frame in
  flight. `WorldLightTint`/`WorldAirVeilAlpha` are the reference the shader
  mirrors; change both or neither. The world's sky channel is the fraction of
  full daylight; `daylight` is a uniform.
- The emissive plane occludes exactly where the scene plane does: a material
  that does not glow is opaque black there, air carries the same veil, and the
  character draws its silhouette. Anything drawn transparent in the emissive
  pass lets whatever glows behind it bloom through — that is how stars shone
  through the hero and through slabs thrown into space.
- `Renderer` also owns the fixed-capacity `PresentationFxSystem`: selected
  `GameEvent` values spawn short-lived world-space primitives, which update and
  draw in scene/emissive passes but can never read or mutate `World`. The pool
  is compact, bounded at 128 instances and replaces the lowest-priority effect
  nearest expiration on overflow; do not turn it into another particle engine
  or move it into `GameState`.
- Combat presentation maps explosion, beam contacts, force, boost and drill
  events into delayed staged FX without duplicating gameplay. Visual/audio
  variation owns separate presentation RNG state. `CameraFeedback` is a
  bounded 16-impulse presentation stack; it alone owns shake/rotation/zoom
  kicks and never writes player/world state.
- Abilities are a registry: `ABILITIES` in `abilities.c` holds what every power
  shares, one `apply` function holds what a power does, `input.c` owns the key
  bindings, and feedback leaves through `GameEvent` — knockback included, via
  `playerImpulse`. An ability never draws, never touches `Player`, and never
  plays a sound. See `docs/development/adding-an-ability.md`.
- Particles are one fixed pool with two roles kept apart by the type system:
  visual particles are stepped against a `const World *` and cannot write cells;
  only `PARTICLE_CONTACT_SETTLE` debris may, and only into an empty cell.
- `DynamicTerrainSystem` stores terrain that has been torn off the world as
  whole bodies, never as one entity per cell, under compile-time budgets that
  are enforced by refusing work rather than growing. It never receives a
  `World`. Bodies are addressed by generation handles, so a reference held
  across a free resolves to NULL instead of to whatever took the slot. See
  `docs/dynamic-terrain.md`.
- Every terrain-body budget refuses rather than evicts, and every refusal is
  counted. Nothing is ever thrown away to make room: choosing a victim would
  need a gameplay policy that does not exist, and an old body is not less
  valuable than a new one. The awake budget throttles motion, not existence — a
  body created past it is born asleep rather than refused.
- **Emberfall never scans the world looking for detached terrain.** Connectivity
  checks run only where a known destructive operation just removed structural
  material, inside a window whose side is a compile-time constant. Destructive
  effects report what they cut through `WorldRecordDestruction`; ordinary
  simulation reports nothing, and a detach check after every settled grain of
  sand is the full-world flood fill this design exists to avoid. Adding a new
  trigger means adding a bounded changed region, never a scan — and never from
  inside the thermal tick.
- Automatic detachment may act only on `WORLD_COMPONENT_DETACHED`. Missing a
  loose fragment costs nothing; tearing a piece out of the main landmass is
  unrecoverable. It extracts through the ordinary atomic path and never copies
  or clears cells itself, so a refusal leaves the world byte-for-byte unchanged.
- Abilities do not push terrain bodies directly: they describe a blast, and the
  fixed step delivers it after detachment and before integration, so a fragment
  a blast just cut free is thrown by that same blast. Queue drains on apply, so
  a blast lands exactly once however many fixed steps a frame runs.
- Large terrain bodies intentionally remain movable physical bodies. Their
  resistance to being moved comes from mass and inertia and from nothing else;
  never add a size check that refuses to push something for being big, and never
  a coefficient that cancels mass out of the result. The same applies to what
  the player can pick up: what makes a boulder uncarryable is that the hold's
  force divided by its mass falls under gravity, not a rule about size.
- The player is what gets corrected out of an overlap with a body, never the
  body. Pushing a slab aside to make room teleports terrain the player may be
  standing on. Collision against bodies tests the path the player took, not only
  where they ended up: at boost speed a frame covers ten cells, and a test of
  the final position lets them cross a thin slab without ever touching it.
- A power that both finds a target and changes it must decide what it reached
  before it changes anything. The laser asks the world and the bodies where the
  nearest blocker is, and only then burns it; burning first and checking after
  damages terrain the shot never arrived at, and nothing puts those cells back.
- A terrain body's raster can lose cells, and losing them moves its centre of
  mass. Every path that edits a raster must correct `position` and `velocity`
  for that shift, or the surviving cells are dragged across the world. Editing a
  raster is also the only thing that may trigger a connectivity recompute: a
  per-tick scan over every body's cells is the cost this design refuses.
- Cells inside a body do not fall, flow, burn or settle. A body is a rigid shape
  that can lose material; the moment its cells would need to move relative to
  each other, that is fracture, and fracture makes new bodies rather than a
  second sand simulation with its own frame of reference.
- A terrain body is destroyed only for leaving the world, never for being old,
  idle or off screen. Kill bounds are a world-safety region with its own margin;
  the camera has no say in what the simulation keeps. The player must be able to
  come back and find the rubble where they left it.
- Terrain-body collision reads the world through a `const World *` and never
  writes it; the world knows nothing about bodies, and no TerrainBody logic
  belongs in the cellular material simulation. Every cost is a compile-time
  constant — substeps, contacts, solver iterations — and the speed ceilings are
  chosen against the substep budget so tunnelling is impossible rather than
  unlikely. See `docs/dynamic-terrain.md`.
- Terrain bodies integrate on the fixed step, never on frame delta, and move as
  one transform: no integration step may walk a body's raster. The transform
  lives in `TerrainBodyLocalToWorld`/`TerrainBodyWorldToLocal` — read it there
  rather than re-deriving it, since rotation is about the centre of mass.
- Bodies collide with each other (`terrain_body_collision.c`) the way they
  collide with the world: raster against raster, one cell read per surface
  sample, pairs in slot order so a replay stays a replay. Contacts with the
  world and between bodies are solved in one projected Gauss-Seidel loop with
  warm starting (`terrain_contact.c`); solved separately, a pile creeps under a
  residual for ever and never sleeps. A sleeping body is a wall until something
  moving faster than `TERRAIN_PAIR_WAKE_SPEED` touches it, a body that wakes
  wakes what rests on it, and ground destroyed under a sleeper reaches it
  through the destruction log (`DynamicTerrainWakeInCells`) — never through a
  scan of sleeping bodies asking whether their floor is still there. Every
  substep shares one count set by the fastest body: two bodies can only be
  compared at the same moment.
- `TerrainBodyRenderer` is the only GPU owner for detached terrain. Its 32
  fixed cache slots key scene/emissive RGBA8 textures by the existing
  generation handle plus `rasterRevision`; unchanged bodies cost only a bounded
  slot scan and visible draw, while raster edits upload in place. It reads
  `DynamicTerrainSystem` through const pointers and must never write gameplay
  state. Point filtering and `centerOfMass` as the `DrawTexturePro` origin are
  non-negotiable transform invariants.
- Extraction (`terrain_extraction.h`) moves a proven-detached component out of
  the world atomically: everything that can fail runs before the first cell is
  cleared, so a failure leaves the world byte-for-byte unchanged and no body
  allocated. Only `WORLD_COMPONENT_DETACHED` may be extracted, and clearing
  goes through `WorldSetCell` so wake, dirty and light invalidation happen the
  ordinary way. Nothing calls it automatically yet.
- Connected-component queries (`world_components.h`) are bounded by a
  caller-supplied region and a caller-owned workspace, and must stay that way:
  the production world's terrain is one connected mass of fourteen million
  cells, so an unbounded fill is never acceptable. Only
  `WORLD_COMPONENT_DETACHED` may be acted on; every other status means leave it
  alone.
- The world module is `world.h` plus `materials.c`, `world_storage.c`,
  `world_simulation.c`, `world_thermal.c`, `world_generation.c`, `world_biomes.c`,
  `world_lighting.c`, `world_effects.c`, `world_render_data.c`,
  `world_components.c`, `world_fluid.c` and `world_structures.c` (ruins,
  dungeons, mines, crypts and sky islands, built on what `world_biomes.c`
  made through the `WorldGen*` helpers in `world_internal.h`).
  `world_internal.h`, `world_thermal.h`, `world_lighting.h` and `world_fluid.h`
  are private to those files. Hot accessors live in the internal headers as `static inline` on
  purpose: splitting responsibilities must not put a cross-module call in the
  per-cell loop. Add material properties as table columns in `materials.c`, not
  as a switch elsewhere — see `docs/development/adding-a-material.md`.
