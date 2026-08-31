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

`make test` builds and runs `tests/world_tests.c`, a headless suite over CPU-side
gameplay (`world`, `player`, `powers`, and `particles`). It never opens a window:
`WorldInit` allocates only CPU state and `WorldInitRenderer` — the one function
needing a GL context — is not called. Keep it that way, and add a test whenever a
simulation or power invariant is discovered or changed. Run it before handing
off; it is far cheaper and broader than the smoke test.

## Source layout

- `src/main.c`: window, fixed-step loop, camera, input, HUD, reset flow
- `src/world.c/.h`: materials, one-dimensional cell storage, generation,
  simulation, texture buffer, destruction helpers
- `src/player.c/.h`: sub-stepped gravity-free flight, boost drilling,
  circle-vs-cell collision, and state-based player rendering
- `src/powers.c/.h`: laser/cryo beams, explosion/force cooldowns, world effects
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
- A material carries a phase change in each direction, heating and cooling, and
  each one is a `MaterialPhase` with an explicit `enabled` flag. Encoding "no
  transition" as a target equal to the material itself is what let an unwritten
  field read back as "become EMPTY at 0C", so the cryo beam deleted every rock,
  dirt and sand cell it touched. Keep the zero value of a forgotten field inert.
- When adding a power or a material, work through how it meets every power and
  force that already exists, and add a test for each answer. Cold is not a
  solvent, a blast does not reach round a corner, ice melts in fire: the
  interactions are the design, and the ones nobody thought about are where the
  bugs are.
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
- Player rendering is a code-native pixel humanoid built in a body frame, not as
  a sprite: `up` runs hips-to-head, `side` across the shoulders, and the frame
  rotates from vertical toward the direction of travel as `leanAmount` rises, so
  the same joints hover upright with the knees drawn back and lay out flat with
  the arms forward at boost speed. Normal flight is hovering and caps its target
  lean at 0.12 even at maxSpeed; only an active boost opens the 0.82–1.0 range.
  Smooth the transition between those targets. Because `up` is hips-to-head, it
  must converge to **`travel`**, never `-travel`, at full lean; the latter makes
  the model fly feet-first and lean backwards. Interpolate the angle over its
  shortest arc, not the two vectors: upright and straight-down vectors cancel
  halfway and make the model snap. "Behind" for the legs comes from `-side` at
  rest and `-travel` at speed; taking the direction of travel when there is none
  tucks the feet upward.
- The model has **no outline**. A dark rim around every limb flattens it into a
  silhouette — a brick with a cape — and hides the shading that makes it a body.
  Contrast comes from a lit tone toward `up`, a shadow opposite, and distinctly
  darker far-side limbs. Keep one cell of neck, or the head merges into the
  shoulders; anchor the cape on the shoulder rather than beside it, or it reads
  as a separate ribbon; keep it near-vertical at rest, or the torso covers all
  but a ragged diagonal edge. Impact brightens the fills in pale gold — an orange
  flash is the colour of the cape and the two merge into one blob.
- Hand poses come from `PlayerPose`, set by the caller because only it knows
  which power is firing: aim-tracking in free flight, one arm straight for the
  laser, both palms out for cryo, a punch for the blast. Held powers refresh a
  short pose every frame; a one-shot asks for its own length and is not cut short
  by a shorter request. Animation is driven by `animationTime`; never use
  wall-clock time. The collider stays a simple circle and does not follow the
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
- `Q` delivers one heavy force blast, not a held stream: a sustained gust reads
  as weak whatever its numbers, because nothing in it is a moment of impact.
  Its gameplay tuning lives in `PowerSystem`: length 84, spread cosine 0.78,
  dynamic-cell reach 54, and player recoil 132. The cone visual reads the same
  length and angle instead of duplicating them. Dynamic cells are thrown a long
  way on a linear falloff — squaring it leaves everything past the first few
  cells barely moving. Solids do not move, but the exposed face the blow lands on
  is scoured to ash with up to a 16% central chance, so the hit leaves a mark;
  only cells actually exposed along the blast axis are marked, or the inside of a
  hill hollows out. The blast is occluded by terrain: without that it shoves
  material on the far side of a wall and scours the wall's back face. The player
  takes recoil, because a blow that moves the world but not the person delivering
  it reads as a button.
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
- Laser, explosion, material-reaction, drill, impact, force, and cryo sounds are
  synthesized at startup; no external assets are required. Laser, drill, force,
  and cryo are held states driven by `GameAudioUpdate` through a `GameAudioState`
  struct, not stacked one-shots. The drill pitches by the material it is cutting,
  sampled before the cut — afterwards the cell is empty and there is nothing left
  to identify. One-shots that can retrigger every frame, impact and reaction,
  carry a short cooldown. Audio failure must remain non-fatal, and no wave memory
  may be allocated in the frame loop.

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
