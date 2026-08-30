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
few frames. It returns non-zero unless a water/lava reaction, laser contact, and
explosion event were all observed. Use `make run RUN_ARGS=--smoke-test`; it
still needs a working display (for example Xvfb in headless environments).

## Source layout

- `src/main.c`: window, fixed-step loop, camera, input, HUD, reset flow
- `src/world.c/.h`: materials, one-dimensional cell storage, generation,
  simulation, texture buffer, destruction helpers
- `src/player.c/.h`: sub-stepped gravity-free flight, circle-vs-cell collision,
  hazards, health/respawn, and player rendering
- `src/powers.c/.h`: continuous laser, explosion cooldown, world effects
- `src/particles.c/.h`: fixed-capacity particle pool
- `src/audio.c/.h`: startup-only procedural wave synthesis and sound playback

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
- Every non-empty cell carries temperature. Laser, lava, and fire add heat;
  thermal thresholds drive dirt→fire, water→steam, and rock→lava transitions.
  Never reintroduce a separate rock-damage counter.
- `DIRT` and `ROCK` are static. Sand, water, lava, steam, smoke, fire, and ash
  participate in cell simulation. Steam/smoke/fire rise; ash falls.
- Water/lava contact turns water into a physical steam cell, solidifies one lava
  cell into rock, and appends a reaction to the fixed-capacity event buffer.
  Main consumes those events after every fixed tick for extra particles/audio;
  do not allocate events.

## Gameplay invariants

- The player is never affected by gravity. Dirt, rock, and sand are solid for a
  circular player collider; liquids, gases, fire, and ash are passable.
- Movement is split into steps no longer than 0.75 world cells to prevent
  tunneling. Resolve embedding again after simulation because sand can enter the
  player between movement updates.
- Lava, fire, and nearby self-explosions deal damage with brief invulnerability.
  Death keeps the changed world and respawns the player after a short delay.
- Movement uses delta time and remains smooth at varying render rates.
- The camera follows the player and magnifies the world with crisp pixel edges.
- Holding LMB traces a contact laser toward the cursor direction. It passes
  through air and liquids, stops at the nearest dirt/sand/rock cell, applies one
  local brush, and reports the hit point for glow and sparks. Rock takes
  sustained exposure and becomes lava.
- RMB explodes at the cursor with a visible cooldown. The inner radius removes
  terrain and turns some rock into lava; the outer shockwave pushes dynamic
  cells from outer bands inward and emits one event for player knockback,
  expanding-ring feedback, and camera shake.
- Powers own a 0–100 energy resource. Laser use drains energy and accumulates
  heat; overheating locks it until heat falls below the recovery threshold.
  Explosions require both cooldown and enough energy. Keep these values visible
  in the always-on status HUD.
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
