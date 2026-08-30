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
explosion, player collision, contained fire propagation, and chunk sleep were
all observed. Use `make run RUN_ARGS=--smoke-test`; it still needs a working
display (for example Xvfb in headless environments).

## Source layout

- `src/main.c`: window, fixed-step loop, camera, input, HUD, reset flow
- `src/world.c/.h`: materials, one-dimensional cell storage, generation,
  simulation, texture buffer, destruction helpers
- `src/player.c/.h`: sub-stepped gravity-free flight, circle-vs-cell collision,
  and player rendering
- `src/powers.c/.h`: continuous laser, explosion cooldown, world effects
- `src/particles.c/.h`: fixed-capacity particle pool
- `src/audio.c/.h`: startup-only procedural wave synthesis and sound playback
- `docs/`: Russian developer documentation; `docs/README.md` is its index

Keep module APIs small and data-oriented. Prefer direct C structs and functions
over frameworks, generic containers, or unnecessary abstraction.

## World invariants

- The simulation grid is 512×288 unless a deliberate design change requires
  otherwise.
- Store all cells in one contiguous allocation using `y * width + x` indexing.
- Keep one persistent `Color` buffer and render the world with one Texture2D
  updated by `UpdateTexture`; never render cells with per-cell DrawRectangle
  calls.
- Use nearest-neighbor texture filtering.
- Do not allocate heap memory in the frame loop.
- Update falling materials from bottom to top.
- Alternate horizontal traversal to avoid a permanent liquid bias.
- A moved cell must not simulate more than once in the same fixed tick; preserve
  the `updatedTick` mechanism when adding or changing materials.
- Simulation uses persistent 32×32 active-chunk and next-active-chunk arrays.
  Movement, heat, destruction, material changes, and public cell writes must
  wake the affected cell and neighboring chunks in both buffers. Dynamic or hot
  cells keep their area awake; static ambient chunks are allowed to sleep.
- Active-chunk buffers are allocated in `WorldInit`, swapped after a fixed tick,
  and freed in `WorldUnload`. Never allocate chunk state in the frame loop.
- Every non-empty cell carries temperature. Laser, lava, and fire add heat;
  thermal thresholds drive dirt→fire, water→steam, and rock→lava transitions.
  Never reintroduce a separate rock-damage counter.
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
- Movement is split into steps no longer than 0.5 world cells to prevent
  tunneling. Solid impacts reflect the blocked velocity component with limited
  restitution and publish impact position/normal/strength for particles and
  camera shake. Resolve embedding again after simulation because sand can enter
  the player between movement updates.
- The player has no health, damage, death, or respawn systems. Lava, fire, and
  self-explosions do not punish the player; this is intentionally a low-friction
  sandbox.
- Movement uses delta time and remains smooth at varying render rates.
- Player rendering is a compact, code-native pixel humanoid made from whole-cell
  rectangles. Preserve its dark helmet/suit, cyan accent, separate limbs, and
  orange stepped cape. Mirror it toward the cursor instead of freely rotating
  it; velocity shifts the cape and thrust animates the legs. The collider stays
  a simple circle and does not follow the visible silhouette.
- Visual references may guide scale, contrast, or broad genre language, but do
  not reproduce another game's character sprite, exact silhouette, palette, or
  animation one-for-one. Emberfall must keep an original character design.
- The camera follows the player and magnifies the world with crisp pixel edges.
- Holding LMB traces a contact laser toward the cursor direction. It passes
  through air and liquids, stops at the nearest dirt/sand/rock cell, applies one
  local brush, and reports the hit point for glow and sparks. Rock takes
  sustained exposure and becomes lava.
- RMB explodes at the cursor with a visible cooldown. The inner radius removes
  terrain and turns some rock into lava; the outer shockwave pushes dynamic
  cells from outer bands inward and emits one event for player knockback,
  expanding-ring feedback, and camera shake.
- Powers have no energy, ammunition, or overheat resource. The laser is always
  available. Explosion keeps only its short input cooldown and physical
  feedback.
- `R` fully regenerates gameplay state. `F1` toggles the debug HUD.
- The HUD reports FPS, player position, dynamic-cell count, and current power.
- Laser, explosion, and material-reaction sounds are synthesized at startup; no
  external assets are required. Audio failure must remain non-fatal, and no wave
  memory may be allocated in the frame loop.

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
