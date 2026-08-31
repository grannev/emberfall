# CODEX.md — project guide for Codex

## Project intent

Emberfall is a small, original 2D pixel-physics sandbox written in C11 with
raylib. Keep the prototype immediately playable: the player flies freely,
destroys terrain with a laser, triggers cursor explosions, and observes sand,
water, and lava reacting to the changed world.

Do not copy names, characters, assets, UI, lore, or levels from MOLTYN, Noita,
or other games. Their broad sandbox idea is only inspiration.

## Build and verification

Use the system raylib through pkg-config. Do not hardcode raylib include or
library paths.

```sh
make
make debug
make run
make clean
```

The compiler is GCC in C11 mode with `-Wall -Wextra -Wpedantic`. Release uses
`-O2`; debug uses `-g -O0`. Before handing off changes, build both release and
debug configurations. For non-interactive runtime checks, the executable also
accepts `--smoke-test`, writes `build/emberfall-smoke.png`, and closes after a
few frames. It returns non-zero unless material reaction, laser contact,
explosion, player collision bounce, boost drilling, contained fire propagation,
and chunk sleep were all observed. The smoke path must not steer the live player,
and must not depend on where generation happened to put terrain; drive extra
coverage from the dedicated probes instead. Use `make run RUN_ARGS=--smoke-test`;
it still needs a working display (for example Xvfb in headless environments).

`make test` builds and runs `tests/world_tests.c`, a headless suite over the
simulation core. It never opens a window: `WorldInit` allocates only CPU state
and `WorldInitRenderer` — the one function needing a GL context — is not called.
Keep it that way, and add a test whenever a simulation invariant is discovered or
changed. Run it before handing off; it is far cheaper and broader than the smoke
test.

## Source layout

- `src/main.c`: window, fixed-step loop, camera, input, HUD, reset flow
- `src/world.c/.h`: materials, one-dimensional cell storage, generation,
  simulation, texture buffer, destruction helpers
- `src/player.c/.h`: sub-stepped gravity-free flight, boost drilling,
  circle-vs-cell collision, and state-based player rendering
- `src/powers.c/.h`: continuous laser, explosion cooldown, world effects
- `src/particles.c/.h`: fixed-capacity particle pool drawn as whole cells,
  colliding with terrain and settling into it
- `src/audio.c/.h`: startup-only procedural wave synthesis and sound playback
- `docs/`: Russian developer documentation; `docs/README.md` is its index

Keep module APIs small and data-oriented. Prefer direct C structs and functions
over frameworks, generic containers, or unnecessary abstraction.

## World invariants

- The simulation grid is 1536×864 unless a deliberate design change requires
  otherwise. Generation must stay parameterised by `world->width`/`height`: no
  fixed-size buffers and no literal feature coordinates. Feature sizes stay
  absolute and their counts scale with area, so a larger world gets more terrain
  of the same scale rather than the same layout stretched out.
- Store all cells in one contiguous allocation using `y * width + x` indexing.
- Keep one persistent `Color` buffer and render the world with one Texture2D;
  never render cells with per-cell DrawRectangle calls. `WorldDraw` rebuilds only
  dirty chunks that fall inside the visible rectangle it is given, and uploads
  one full-width row band through `UpdateTextureRec`. Drawing must cost what the
  player can see, not what the world is doing: activity is spread over the whole
  map while the camera shows a small window of it. A chunk skipped for being off
  screen keeps its dirty flag and is rebuilt on the frame it scrolls into view.
  Incremental drawing must stay pixel-identical to a full rebuild of the same
  region.
- Pixel-dirty and light-dirty chunks are separate sets. A pixel rebuild may wait
  many frames for the chunk to come on screen; light must be refreshed once,
  everywhere it changed, because the solve is global. Sharing one flag makes the
  light refresh re-scan every off-screen chunk every frame.
- Light is solved on a grid `WORLD_LIGHT_SCALE` coarser than the cells, in two
  channels: sky reaching down from the surface, and ember from anything that
  burns. One intensity can darken but cannot colour, and a lava lake lighting its
  own cavern in grey is not lighting. Emission takes the maximum over a block,
  never the mean: one lava cell in a wall is a source. Keep the ambient floor
  high enough that sealed ground reads as dim solid material rather than a black
  hole, and give the player a light of their own — a bored tunnel with no source
  must stay playable.
- The solved light is quantised, and re-lit chunks are found by comparing it
  exactly. A tolerance drifts: a sample that moves less than it every frame is
  never rebuilt, and the texture wanders arbitrarily far from the light it should
  be showing. Verify with a harness that compares incremental drawing against a
  full rebuild of the same world.
- The light solve is the one part of drawing not proportional to what changed, so
  it must not run when its inputs did not change, nor more than once per
  simulation tick. A moving light source is the exception.
- Use nearest-neighbor texture filtering.
- Do not allocate heap memory in the frame loop.
- Update falling materials from bottom to top.
- Alternate horizontal traversal to avoid a permanent liquid bias.
- A moved cell must not simulate more than once in the same fixed tick; preserve
  the `updatedTick` mechanism when adding or changing materials.
- Simulation uses persistent 32×32 active-chunk and next-active-chunk arrays.
  Movement, heat, destruction, material changes, and public cell writes must wake
  the affected cell in both buffers. A chunk stays awake because something
  actually happened in it, never merely because it contains a dynamic or hot
  cell: a settled sand pile and the interior of a lava lake are allowed to sleep,
  and whatever later disturbs them wakes the surrounding chunks on its way
  through. A cell only influences its immediate neighbours, so it wakes an
  adjacent chunk only when it sits against that chunk's border.
- Active-chunk buffers are allocated in `WorldInit`, swapped after a fixed tick,
  and freed in `WorldUnload`. Never allocate chunk state in the frame loop.
- Every non-empty cell carries temperature. Laser, lava, and fire add heat;
  thermal thresholds drive dirt→fire, water→steam, and rock→lava transitions.
  Never reintroduce a separate rock-damage counter.
- What a material *is* belongs in the `MATERIALS` table; only what it *does* per
  tick belongs in code. Adding a material means adding one table entry, and every
  entry needs a name — a gap is zero-filled and silently reads back as EMPTY.
- Passive heat from lava is capped below rock's melt threshold, and a saturated
  neighbour is skipped rather than reheated. Without that ceiling one pocket
  turns the entire map to lava, the same failure the fire budget prevents, and
  its ever-growing boundary never lets chunks sleep.
- `WorldDrillCircle` removes only solid cells, leaves a small fraction as ash,
  and merely warms everything it cannot cut. That wall temperature must stay
  below the water steam point and the dirt ignition point, so drilling can never
  start a fire or boil water on its own.
- Solid cells are tinted toward ember as their temperature approaches their own
  phase threshold, on a non-linear ramp so modest heat stays visible. Cells at
  ambient temperature must skip the tint entirely: it runs for every cell of
  every dirty chunk and cannot afford per-cell work that does not early-out.
- Keep passive heat from one fire cell below the dirt ignition budget over its
  full lifetime. Fire may burn a local cluster, but it must not consume an
  unlimited connected dirt layer; the smoke-test has a containment probe.
- `DIRT` and `ROCK` are static. Sand, water, lava, steam, smoke, fire, and ash
  participate in cell simulation. Steam/smoke/fire rise; ash falls.
- Water/lava contact turns water into a physical steam cell, solidifies one lava
  cell into rock, and appends a reaction to the fixed-capacity event buffer.
  Main consumes those events after every fixed tick for extra particles/audio;
  do not allocate events.

## Gameplay invariants

- The player is never affected by gravity. Dirt, rock, and sand are solid for a
  circular player collider; liquids, gases, fire, and ash are passable.
- WASD applies normalized thrust to persistent velocity. Linear drag and a speed
  cap keep flight controllable without removing inertia.
- Shift plus directional input activates boost acceleration and its higher speed
  cap. Boost survives the loss of directional input for a short grace window so
  releasing WASD inside a tunnel does not drop the drill into a wall. Above the
  drill threshold, every movement substep clears a circle ahead of the collider
  through `WorldDrillCircle`; the out-of-bounds world boundary remains
  indestructible. Below that threshold the drill still bites where the player is
  pressed into material, cutting around the collider instead of ahead of it:
  otherwise a boost begun from rest against a wall can never start, because the
  collision zeroes the blocked velocity before it can reach the threshold, and a
  cut placed ahead lands inside the hole already made while the rim that blocks
  survives. Freeing the collider must widen until it actually succeeds — the
  drill measures an integer radius from a floored centre while the collider is a
  float circle measured to the nearest cell edge, so one cut of the same nominal
  radius can leave a blocking cell standing. Each cut cell costs speed, but the resistance is floored just
  above the drill threshold so a boost can never stall inside solid terrain.
  Boost has no energy resource or cooldown.
- Movement is split into steps no longer than 0.5 world cells to prevent
  tunneling. Solid impacts reflect the blocked velocity component with limited
  restitution and publish impact position/normal/strength for particles and
  camera shake. Resolve embedding again after simulation because sand can enter
  the player between movement updates. A player who is boosting cuts free of
  that embedding and keeps their momentum; relocating them would zero the very
  velocity that was about to clear it, which made a sand body impassable even
  though the drill removes sand. Relocation stays the fallback for every other
  case.
- The player has no health, damage, death, or respawn systems. Lava, fire, and
  self-explosions do not punish the player; this is intentionally a low-friction
  sandbox.
- Movement uses delta time and remains smooth at varying render rates.
- Player rendering is a compact, code-native pixel humanoid made from whole-cell
  rectangles. Preserve its dark helmet/suit, cyan accent, separate limbs, and
  orange stepped cape. Head/arm aim follows the cursor independently, while
  velocity determines the cape side and vertical bend. The cape root and tail
  must keep overlapping at every wave offset so the cape never splits into a
  detached block, and body parts drawn over the cape must show a lit fill rather
  than only their dark outline. Impact feedback brightens the fills and keeps the
  rim dark; never recolour the outline itself, which floods the model and erases
  the silhouette. Preserve separate idle, thrust, boost, drill, and impact
  feedback driven by `animationTime`; do not use wall-clock time for player
  animation. The collider stays a simple circle and does not follow the
  visible silhouette.
- Visual references may guide scale, contrast, or broad genre language, but do
  not reproduce another game's character sprite, exact silhouette, palette, or
  animation one-for-one. Emberfall must keep an original character design.
- The camera leads the player along their velocity and widens its view as they
  go faster, both clamped and smoothed, and magnifies the world with crisp pixel
  edges. A camera locked to the player's exact position shows too little of what
  a boost is about to hit. Drive the widening from measured speed, not from the
  boost key, so knockback reads the same as thrust and a stalled boost does not
  zoom out.
- Holding LMB traces a contact laser toward the cursor direction. It passes
  through air and liquids, stops at the nearest dirt/sand/rock cell, applies one
  local brush, and reports the hit point for glow and sparks. Rock takes
  sustained exposure and becomes lava.
- RMB explodes at the cursor with a visible cooldown. The inner radius removes
  terrain and turns some rock into lava; the outer shockwave pushes dynamic
  cells from outer bands inward and emits one event for player knockback,
  expanding-ring feedback, and camera shake.
- Particles collide with solid terrain rather than flying through it, and their
  behaviour on contact is a `ParticleContact` on the particle: gases and glow
  pass, sparks and shards bounce, drill debris settles. A particle born inside
  material must escape it before terrain can stop it. Settling writes only empty
  cells and only a fraction of debris settles, so effects can never overwrite
  terrain, bury the player, or refill a tunnel faster than the drill cuts it.
- Holding `Q` sweeps a force cone that pushes dynamic cells and destroys none.
  It is the one power that shapes the world instead of removing from it, so the
  cell count it touches must be conserved; solids do not move. Apply it on a
  fixed cadence, never per frame — whole cells move, so a per-frame gust would
  be stronger at a higher frame rate.
- Holding `E` fires the cryo beam, the thermal inverse of the laser: water
  freezes to `ICE`, lava settles back to `ROCK`, fire is snuffed. Its rate on
  lava must clearly beat lava's own relaxation toward 900C, or the beam finds an
  equilibrium above the freezing threshold and cools nothing. `ICE` is solid and
  does not drift back to ambient, which is what makes it the only way the player
  can add material to the world; any heat still melts it. A slowly drifting
  material cannot work here at all — a cell changing less than the sleep
  threshold per tick never wakes its own chunk.
- Powers have no energy, ammunition, or overheat resource. The laser is always
  available. Explosion keeps only its short input cooldown and physical
  feedback.
- `R` fully regenerates gameplay state. `F1` toggles the debug HUD.
- The HUD reports FPS, player position, dynamic-cell count, and current power.
- What the laser does to a material is a rate in the `MATERIALS` table, and the
  beam stops at anything `WorldMaterialIsSolid` reports. Never reintroduce a
  switch on material identity there: the first version of `ICE` was solid,
  stopped no beam, and could not be melted, purely because the laser still named
  three materials by hand.
- Laser, explosion, material-reaction, and drill sounds are synthesized at
  startup; no external assets are required. Laser and drill are held states
  driven by `GameAudioUpdate`, not stacked one-shots. Audio failure must remain
  non-fatal, and no wave memory may be allocated in the frame loop.

## Change discipline

Preserve user changes and avoid unrelated rewrites. Use `rg` for code search and
`apply_patch` for manual edits. Keep warnings at zero where practical. When a
simulation change is subtle, verify both the material result and the one-update-
per-tick invariant. Update README controls or architecture notes when behavior or
build commands change.

Keep `docs/` synchronized with code. Changes to module ownership, public APIs,
material behavior/thresholds, player collision, power behavior, frame ordering,
build commands, or smoke-test coverage must update the corresponding document
in the same commit. Do not treat the root README as the complete developer
reference.
