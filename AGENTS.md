# Emberfall agent guide

Emberfall is a C11 + raylib side-view sandbox built around a large cellular
world. raylib is the platform, input, rendering, and audio backend; the
long-term boundary is a headless-testable gameplay core below those systems.

Start with `README.md`, then `docs/README.md`. Detailed system documents contain
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
make bench           # deterministic 10-scenario benchmark
make asan
make ubsan
make profile
xvfb-run -a make run RUN_ARGS=--smoke-test
```

## Invariants

- The gameplay simulation advances at a fixed 60 Hz step.
- Falling cells are processed bottom-to-top; horizontal traversal alternates.
- `updatedTick` prevents a moved cell from updating twice in one tick.
- Cell mutations wake only the affected chunk and crossed borders; generated
  distant dynamics remain asleep until streamed into play.
- Fire and lava heating stay local. Passive lava cannot melt its rock lining.
- Player collision uses substeps and must not tunnel; boost drilling must not
  leave the collider embedded.
- World mutations, particles, and events use persistent/fixed-capacity storage;
  normal update and render loops must not allocate from the heap.
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
- Baseline CPU allocation is 275.12 MiB before GPU state: 216 MiB cells, 54 MiB
  persistent pixels, 5.06 MiB lighting, and minor metadata. The persistent
  pixel buffer and giant texture are confirmed migration targets.
- Refactoring is deliberately phased. Do not combine game/input/events, world
  decomposition, Cell layout, active scheduling, and render paging into one
  rewrite. Keep every intermediate commit playable and measured.
