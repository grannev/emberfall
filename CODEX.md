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
few frames. Use `make run RUN_ARGS=--smoke-test`; it still needs a working
display (for example Xvfb in headless environments).

## Source layout

- `src/main.c`: window, fixed-step loop, camera, input, HUD, reset flow
- `src/world.c/.h`: materials, one-dimensional cell storage, generation,
  simulation, texture buffer, destruction helpers
- `src/player.c/.h`: smooth gravity-free WASD flight and player rendering
- `src/powers.c/.h`: continuous laser, explosion cooldown, world effects
- `src/particles.c/.h`: fixed-capacity particle pool

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
- `DIRT` and `ROCK` are static. `SAND`, `WATER`, and `LAVA` are dynamic. Lava is
  intentionally slower and burns dirt.

## Gameplay invariants

- The player is never affected by gravity and may pass through cells; sandbox
  interaction is more important than collision realism.
- Movement uses delta time and remains smooth at varying render rates.
- The camera follows the player and magnifies the world with crisp pixel edges.
- Holding LMB maintains a laser toward the cursor. Dirt and sand break quickly;
  rock takes sustained exposure and becomes lava.
- RMB explodes at the cursor with a visible cooldown. Some affected rock becomes
  lava.
- `R` fully regenerates gameplay state. `F1` toggles the debug HUD.
- The HUD reports FPS, player position, dynamic-cell count, and current power.

## Change discipline

Preserve user changes and avoid unrelated rewrites. Use `rg` for code search and
`apply_patch` for manual edits. Keep warnings at zero where practical. When a
simulation change is subtle, verify both the material result and the one-update-
per-tick invariant. Update README controls or architecture notes when behavior or
build commands change.
