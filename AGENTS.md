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
- Player collision uses substeps and must not tunnel; boost drilling must not
  leave the collider embedded.
- World mutations, particles, and events use persistent/fixed-capacity storage;
  normal update and render loops must not allocate from the heap.
- Gameplay randomness is seeded and explicit. A seed plus a sequence of
  `GameInput` values must replay identically. Never call `GetRandomValue` from
  gameplay code — draw from the owning system's `Rng` (see `src/rng.h`).
  Presentation-only jitter may still use raylib's generator.
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
  persistent pixel buffer is gone and `Cell` is packed to 12 bytes, so the
  current estimate is 167.22 MiB. The giant world texture is gone as well:
  `WorldRenderer` keeps a cache of 256x256 pages and only the visible ones are
  resident, so world size is no longer bounded by `GL_MAX_TEXTURE_SIZE`.
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
- Each resident world page has scene and emissive textures. One 8 KiB stack
  staging pair builds both from a dirty chunk; `MaterialInfo.emission` and hot
  solids enter bloom, ordinary bright terrain does not. Particle emission is
  explicit presentation metadata and must be reset whenever a pool slot is
  reused.
- Abilities are a registry: `ABILITIES` in `abilities.c` holds what every power
  shares, one `apply` function holds what a power does, `input.c` owns the key
  bindings, and feedback leaves through `GameEvent` — knockback included, via
  `playerImpulse`. An ability never draws, never touches `Player`, and never
  plays a sound. See `docs/development/adding-an-ability.md`.
- Particles are one fixed pool with two roles kept apart by the type system:
  visual particles are stepped against a `const World *` and cannot write cells;
  only `PARTICLE_CONTACT_SETTLE` debris may, and only into an empty cell.
- The world module is `world.h` plus `materials.c`, `world_storage.c`,
  `world_simulation.c`, `world_thermal.c`, `world_generation.c`,
  `world_lighting.c`, `world_effects.c` and `world_render_data.c`.
  `world_internal.h`, `world_thermal.h` and `world_lighting.h` are private to
  those files. Hot accessors live in the internal headers as `static inline` on
  purpose: splitting responsibilities must not put a cross-module call in the
  per-cell loop. Add material properties as table columns in `materials.c`, not
  as a switch elsewhere — see `docs/development/adding-a-material.md`.
