/* Headless invariant tests for the simulation core.
 *
 * These never open a window and never touch the GPU: World owns only CPU state,
 * while WorldRenderer is not linked into this binary. That keeps the suite fast
 * enough to run on every build and lets it cover many more cases than the
 * windowed smoke test.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "materials.h"
#include "particles.h"
#include "player.h"
#include "abilities.h"
#include "world.h"
#include "dynamic_terrain.h"
#include "terrain_extraction.h"
#include "world_components.h"
#include "world_render_data.h"

static int testsRun = 0;
static int testsFailed = 0;
static const char *currentTest = "";

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            ++testsFailed;                                                     \
            fprintf(stderr, "FAIL %s: %s\n      ", currentTest, #condition);   \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            return;                                                            \
        }                                                                      \
    } while (0)

#define RUN(test)                                                              \
    do {                                                                       \
        int failedBefore = testsFailed;                                        \
        currentTest = #test;                                                   \
        ++testsRun;                                                            \
        test();                                                                \
        if (testsFailed == failedBefore) {                                     \
            printf("ok   %s\n", #test);                                        \
        }                                                                      \
    } while (0)

static int CountMaterial(const World *world, CellMaterial material)
{
    int count = 0;
    int y;

    for (y = 0; y < world->height; ++y) {
        int x;

        for (x = 0; x < world->width; ++x) {
            if (WorldGetCell(world, x, y) == material) {
                ++count;
            }
        }
    }
    return count;
}

static void FillRect(World *world, int x0, int y0, int x1, int y1,
                     CellMaterial material)
{
    int y;

    for (y = y0; y <= y1; ++y) {
        int x;

        for (x = x0; x <= x1; ++x) {
            WorldSetCell(world, x, y, material);
        }
    }
}

static void Tick(World *world, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        WorldUpdate(world);
    }
}

static bool HasGameEvent(const GameEventBuffer *events, GameEventType type)
{
    uint16_t index;

    for (index = 0u; index < events->count; ++index) {
        if (events->events[index].type == type) {
            return true;
        }
    }
    return false;
}

/* --- game boundary ----------------------------------------------------- */

typedef struct RenderProbe {
    int regions;
    uint64_t pixels;
} RenderProbe;

static bool CaptureRenderChunk(void *context, Rectangle bounds,
                               const Color *pixels,
                               const Color *emissivePixels)
{
    RenderProbe *probe = context;

    if (pixels == NULL || emissivePixels == NULL) {
        ++testsFailed;
        fprintf(stderr, "FAIL %s: render visitor received incomplete staging data\n",
                currentTest);
        return false;
    }
    ++probe->regions;
    probe->pixels += (uint64_t)bounds.width * (uint64_t)bounds.height;
    return true;
}

static void test_world_render_preparation_is_headless_and_incremental(void)
{
    World world;
    RenderProbe probe = {0};
    Rectangle wholeWorld = {0.0f, 0.0f, 64.0f, 64.0f};

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);

    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 4, "first preparation visited %d/4 chunks",
          probe.regions);
    CHECK(probe.pixels == 64u * 64u,
          "first preparation staged %llu/4096 pixels",
          (unsigned long long)probe.pixels);

    probe = (RenderProbe){0};
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 0,
          "settled renderer rebuilt %d unchanged chunks", probe.regions);

    WorldSetCell(&world, 10, 10, MATERIAL_EMPTY);
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 1,
          "one local edit rebuilt %d chunks instead of one", probe.regions);
    WorldUnload(&world);
}

/* A chunk the renderer cannot place must keep its dirty flag. Dropping it
   instead leaves stale pixels on screen until something unrelated happens to
   change that chunk again, which is exactly the kind of bug that only shows up
   on a machine with a smaller page cache than the one it was written on. */
static bool RefuseRenderChunk(void *context, Rectangle bounds, const Color *pixels,
                              const Color *emissivePixels)
{
    (void)bounds;
    (void)pixels;
    (void)emissivePixels;
    ++(*(int *)context);
    return false;
}

typedef struct EmissiveProbe {
    bool lavaEmits;
    bool fireEmits;
    bool sandStaysDark;
} EmissiveProbe;

static bool CaptureMaterialEmission(void *context, Rectangle bounds,
                                    const Color *pixels,
                                    const Color *emissivePixels)
{
    EmissiveProbe *probe = context;

    (void)pixels;
    if (bounds.x <= 4.0f && bounds.y <= 4.0f &&
        bounds.x + bounds.width > 6.0f && bounds.y + bounds.height > 4.0f) {
        int width = (int)bounds.width;
        int row = 4 - (int)bounds.y;
        int lavaColumn = 4 - (int)bounds.x;
        int fireColumn = 5 - (int)bounds.x;
        int sandColumn = 6 - (int)bounds.x;
        Color lava = emissivePixels[row * width + lavaColumn];
        Color fire = emissivePixels[row * width + fireColumn];
        Color sand = emissivePixels[row * width + sandColumn];

        probe->lavaEmits = lava.a == 255u &&
                           (lava.r != 0u || lava.g != 0u || lava.b != 0u);
        probe->fireEmits = fire.a == 255u &&
                           (fire.r != 0u || fire.g != 0u || fire.b != 0u);
        probe->sandStaysDark = sand.r == 0u && sand.g == 0u &&
                               sand.b == 0u && sand.a == 0u;
    }
    return true;
}

static void test_emissive_render_data_selects_emitters_not_bright_terrain(void)
{
    World world;
    EmissiveProbe probe = {0};
    Rectangle wholeWorld = {0.0f, 0.0f, 32.0f, 32.0f};

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 4, 4, MATERIAL_LAVA);
    WorldSetCell(&world, 5, 4, MATERIAL_FIRE);
    WorldSetCell(&world, 6, 4, MATERIAL_SAND);

    WorldPrepareVisible(&world, wholeWorld, CaptureMaterialEmission, &probe);
    CHECK(probe.lavaEmits, "lava produced no emissive render data");
    CHECK(probe.fireEmits, "fire produced no emissive render data");
    CHECK(probe.sandStaysDark,
          "bright sand leaked into the explicit emissive mask");
    WorldUnload(&world);
}

static void test_particle_emission_is_explicit_per_effect(void)
{
    ParticleSystem particles;
    int i;
    bool laserGlows = false;
    bool boostGlows = false;

    ParticlesInit(&particles, 0xE6BEu);
    ParticlesSpawnLaserSparks(&particles, (Vector2){10.0f, 10.0f},
                              (Vector2){1.0f, 0.0f});
    ParticlesSpawnBoostTrail(&particles, (Vector2){12.0f, 10.0f},
                             (Vector2){120.0f, 0.0f}, 2);
    for (i = 0; i < MAX_PARTICLES; ++i) {
        const Particle *particle = &particles.particles[i];

        if (!particle->active || particle->emission <= 0.0f) {
            continue;
        }
        if (particle->color.r == 255u && particle->color.g == 225u) {
            laserGlows = true;
        } else {
            boostGlows = true;
        }
    }
    CHECK(laserGlows, "laser sparks carry no emissive metadata");
    CHECK(boostGlows, "boost trail carries no emissive metadata");

    /* Reinitialisation catches stale pool fields; steam must remain a visual
       particle without accidentally inheriting glow from an overwritten slot. */
    ParticlesInit(&particles, 0xE6BEu);
    ParticlesSpawnSteam(&particles, (Vector2){10.0f, 10.0f});
    for (i = 0; i < MAX_PARTICLES; ++i) {
        CHECK(!particles.particles[i].active ||
                  particles.particles[i].emission == 0.0f,
              "steam particle %d leaked into emissive", i);
    }
}

static void test_a_refused_chunk_keeps_its_dirty_flag(void)
{
    World world;
    RenderProbe probe = {0};
    Rectangle wholeWorld = {0.0f, 0.0f, 64.0f, 64.0f};
    int refused = 0;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);

    WorldPrepareVisible(&world, wholeWorld, RefuseRenderChunk, &refused);
    CHECK(refused == 4, "first preparation offered %d/4 chunks", refused);

    /* Nothing was accepted, so the same four chunks must still be owed. */
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 4,
          "a refused chunk was forgotten: %d/4 offered again", probe.regions);

    probe = (RenderProbe){0};
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 0,
          "an accepted chunk was rebuilt again: %d chunks", probe.regions);
    WorldUnload(&world);
}

static void test_game_event_buffer_is_fixed_and_ordered(void)
{
    GameEventBuffer events = {0};
    int index;

    for (index = 0; index < MAX_GAME_EVENTS; ++index) {
        CHECK(GameEventsPush(&events, (GameEvent){
                  .type = GAME_EVENT_PLAYER_DRILL,
                  .count = index,
              }),
              "event %d was rejected before capacity", index);
    }
    CHECK(events.count == MAX_GAME_EVENTS, "buffer stored %u/%d events",
          events.count, MAX_GAME_EVENTS);
    CHECK(events.events[17].count == 17, "event order changed at index 17");
    CHECK(!GameEventsPush(&events, (GameEvent){.type = GAME_EVENT_EXPLOSION}),
          "event buffer grew past fixed capacity");
    CHECK(events.dropped == 1u, "overflow reported %u dropped events",
          events.dropped);

    GameEventsClear(&events);
    CHECK(events.count == 0u && events.dropped == 0u,
          "clear left count=%u dropped=%u", events.count, events.dropped);
}

static void test_game_update_publishes_transient_events(void)
{
    GameConfig config = GameDefaultConfig();
    GameState game;
    GameEventBuffer events = {0};
    GameInput input = {0};
    int cellX;
    int cellY;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0xE6BEu;
    CHECK(GameInit(&game, config), "game allocation failed");

    input.aimWorld = (Vector2){game.player.position.x + 20.0f,
                               game.player.position.y};
    input.ability[ABILITY_EXPLOSION] = true;
    GameUpdate(&game, &input, config.fixedStep, &events);
    CHECK(HasGameEvent(&events, GAME_EVENT_EXPLOSION),
          "explosion did not publish a game event");

    cellX = (int)game.player.position.x + 40;
    cellY = (int)game.player.position.y + 20;
    WorldSetCell(&game.world, cellX, cellY, MATERIAL_WATER);
    WorldSetCell(&game.world, cellX + 1, cellY, MATERIAL_LAVA);
    input = (GameInput){.aimWorld = game.player.position};
    GameUpdate(&game, &input, config.fixedStep, &events);
    CHECK(HasGameEvent(&events, GAME_EVENT_MATERIAL_REACTION),
          "water/lava reaction did not cross the GameEvents boundary");
    CHECK(!HasGameEvent(&events, GAME_EVENT_EXPLOSION),
          "one-frame explosion event leaked into the next update");
    GameUnload(&game);
}

/* Cells hold the low sixteen bits of the tick counter. The saving is 54 MiB,
   and the price is that the counter wraps; this checks that the wrap costs
   nothing anyone can see, and in particular that the truncated value never
   becomes zero, which is what would make every never-written cell in an awake
   chunk skip the same tick together. */
static void test_the_tick_counter_survives_wrapping_its_cell_stamp(void)
{
    World world;
    int before;
    int step;
    int lowest = 0;

    CHECK(WorldInit(&world, 64, 96), "world allocation failed");
    FillRect(&world, 20, 10, 40, 20, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);

    /* Jump to just before the wrap rather than ticking there: the behaviour
       under test is the arithmetic, not sixty-five thousand ticks of sand. */
    world.tick = 0xFFFAu;
    for (step = 0; step < 40; ++step) {
        WorldUpdate(&world);
        CHECK((uint16_t)world.tick != 0u,
              "the tick stamp reached zero, which unwritten cells already hold");
    }

    CHECK(world.tick > 0x10000u, "the test never reached the wrap: tick=%u",
          world.tick);
    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "sand was lost across the tick stamp wrap: %d of %d left",
          CountMaterial(&world, MATERIAL_SAND), before);

    for (step = 0; step < world.height; ++step) {
        int x;

        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, step) == MATERIAL_SAND && step > lowest) {
                lowest = step;
            }
        }
    }
    CHECK(lowest > 20, "sand stopped falling across the wrap; lowest row %d",
          lowest);
    WorldUnload(&world);
}

/* --- active chunk scheduler --------------------------------------------- */

/* The schedule exists in two representations — a flag per chunk and a compact
   per-row list — and every wake path has to keep them agreeing. A duplicate in
   a list would simulate a chunk twice in one tick; a flag with no list entry
   would drop it silently. */
static void CheckScheduleIsConsistent(const World *world, const char *when)
{
    int listed = 0;
    int flagged = 0;
    int chunkY;
    int chunkX;

    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int slot;
        int previous = -1;

        listed += (int)world->activeRowCount[chunkY];
        for (slot = 0; slot < (int)world->activeRowCount[chunkY]; ++slot) {
            int column = (int)world->activeRowColumns[(size_t)chunkY *
                                                          (size_t)world->chunkColumns +
                                                      (size_t)slot];

            CHECK(column >= 0 && column < world->chunkColumns,
                  "%s: scheduled column %d is out of range", when, column);
            CHECK(world->activeChunks[(size_t)chunkY * (size_t)world->chunkColumns +
                                      (size_t)column] != 0u,
                  "%s: chunk %d,%d is listed but not flagged", when, column, chunkY);
            /* WorldUpdate sorts each row before walking it, so after a tick a
               repeat would show up as a non-increasing pair. */
            CHECK(column != previous,
                  "%s: chunk %d,%d appears twice in the schedule", when, column,
                  chunkY);
            previous = column;
        }
    }
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            if (world->activeChunks[(size_t)chunkY * (size_t)world->chunkColumns +
                                    (size_t)chunkX] != 0u) {
                ++flagged;
            }
        }
    }
    CHECK(listed == flagged, "%s: %d chunks listed but %d flagged", when, listed,
          flagged);
    CHECK(world->activeChunkCount == flagged,
          "%s: reported %d active chunks but %d are flagged", when,
          world->activeChunkCount, flagged);
}

static void test_the_schedule_never_lists_a_chunk_twice(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 256, 160), "world allocation failed");
    /* Sand pouring across a chunk boundary and lava beside water: both wake
       neighbours constantly, from inside a tick and from outside one. */
    FillRect(&world, 20, 10, 44, 30, MATERIAL_SAND);
    FillRect(&world, 60, 100, 90, 110, MATERIAL_WATER);
    FillRect(&world, 91, 100, 120, 110, MATERIAL_LAVA);
    CheckScheduleIsConsistent(&world, "after seeding");

    for (step = 0; step < 60; ++step) {
        WorldUpdate(&world);
        CheckScheduleIsConsistent(&world, "during simulation");
        if (step == 30) {
            /* A mutation from outside a tick has to schedule the tick that is
               about to run, not the one that just finished. */
            WorldDrillCircle(&world, 200, 80, 5);
            CheckScheduleIsConsistent(&world, "after an out-of-tick mutation");
            CHECK(world.activeChunkCount > 0,
                  "drilling between ticks scheduled nothing");
        }
    }
    WorldUnload(&world);
}

/* --- determinism --------------------------------------------------------- */

/* A cheap order-sensitive digest of everything the simulation owns. Comparing
   whole worlds cell by cell would work too, but a single number makes a failure
   message readable and keeps the two-world comparisons below short. */
static uint64_t WorldDigest(const World *world)
{
    uint64_t digest = 0xcbf29ce484222325ull;
    int y;

    for (y = 0; y < world->height; ++y) {
        int x;

        for (x = 0; x < world->width; ++x) {
            const Cell *cell = &world->cells[(size_t)y * (size_t)world->width +
                                             (size_t)x];

            digest = (digest ^ (uint64_t)cell->material) * 0x100000001b3ull;
            digest = (digest ^ (uint64_t)(int64_t)(cell->temperature * 16.0f)) *
                     0x100000001b3ull;
        }
    }
    return digest;
}

static void test_the_same_seed_always_generates_the_same_world(void)
{
    World first;
    World second;

    CHECK(WorldInit(&first, 256, 144), "world allocation failed");
    CHECK(WorldInit(&second, 256, 144), "world allocation failed");
    WorldGenerate(&first, 0x51EDu);
    WorldGenerate(&second, 0x51EDu);

    CHECK(first.seed == 0x51EDu && second.seed == 0x51EDu,
          "the world did not record the seed it was generated from");
    CHECK(WorldDigest(&first) == WorldDigest(&second),
          "the same seed produced two different worlds");
    WorldUnload(&first);
    WorldUnload(&second);
}

static void test_regenerating_one_world_from_a_seed_reproduces_it(void)
{
    World world;
    uint64_t first;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    WorldGenerate(&world, 0x51EDu);
    first = WorldDigest(&world);
    WorldGenerate(&world, 0xA11CEu);
    CHECK(WorldDigest(&world) != first, "two seeds produced the same world");
    WorldGenerate(&world, 0x51EDu);
    CHECK(WorldDigest(&world) == first,
          "regenerating from the original seed did not restore the world");
    WorldUnload(&world);
}

/* Generation must not be perturbed by anything drawn later, which is why
   terrain and gameplay effects use separate streams off the same seed. */
static void test_world_effects_cannot_shift_the_terrain_a_seed_produces(void)
{
    World world;
    uint64_t clean;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    WorldGenerate(&world, 0x51EDu);
    clean = WorldDigest(&world);

    WorldGenerate(&world, 0x51EDu);
    WorldDrillCircle(&world, 128, 100, 6);
    WorldDestroyCircle(&world, 60, 100, 8, 0.5f);
    WorldGenerate(&world, 0x51EDu);
    CHECK(WorldDigest(&world) == clean,
          "drawing from the effect stream changed the terrain of a seed");
    WorldUnload(&world);
}

/* The whole point of a seeded game: a bug report is a seed plus a list of
   inputs, and a regression test can replay one exactly. */
static void test_a_seeded_session_replays_identically(void)
{
    GameConfig config = GameDefaultConfig();
    GameState first;
    GameState second;
    GameEventBuffer events = {0};
    int step;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0x1234u;
    CHECK(GameInit(&first, config), "game allocation failed");
    CHECK(GameInit(&second, config), "game allocation failed");
    CHECK(first.worldSeed == second.worldSeed,
          "the same configured seed produced two different worlds");

    for (step = 0; step < 40; ++step) {
        GameInput input = {0};

        input.move = (Vector2){1.0f, step % 7 == 0 ? -1.0f : 0.0f};
        input.boostHeld = step > 5;
        input.ability[ABILITY_LASER] = step % 5 == 0;
        input.ability[ABILITY_EXPLOSION] = step == 12;
        input.ability[ABILITY_FORCE] = step == 20;
        input.ability[ABILITY_CRYO] = step % 11 == 0;
        input.aimWorld = (Vector2){first.player.position.x + 30.0f,
                                   first.player.position.y + 8.0f};
        GameUpdate(&first, &input, config.fixedStep, &events);
        input.aimWorld = (Vector2){second.player.position.x + 30.0f,
                                   second.player.position.y + 8.0f};
        GameUpdate(&second, &input, config.fixedStep, &events);
    }

    CHECK(first.player.position.x == second.player.position.x &&
              first.player.position.y == second.player.position.y,
          "replay diverged: player at %.4f,%.4f vs %.4f,%.4f",
          first.player.position.x, first.player.position.y,
          second.player.position.x, second.player.position.y);
    CHECK(WorldDigest(&first.world) == WorldDigest(&second.world),
          "replay diverged: the two worlds no longer match");
    GameUnload(&first);
    GameUnload(&second);
}

/* The role split is enforced by the type system — visual particles see a
   `const World *` — but the invariant is worth stating in a test too, because
   a future spawner could give a visual effect the SETTLE contact mode by
   mistake and quietly start writing cells. */
static void test_visual_particles_never_change_the_world(void)
{
    World world;
    ParticleSystem particles;
    uint64_t before;
    int step;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    FillRect(&world, 0, 60, 127, 95, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    ParticlesSpawnExplosion(&particles, (Vector2){64.0f, 40.0f});
    ParticlesSpawnLaserSparks(&particles, (Vector2){64.0f, 58.0f},
                              (Vector2){0.0f, 1.0f});
    ParticlesSpawnImpact(&particles, (Vector2){64.0f, 58.0f},
                         (Vector2){0.0f, -1.0f}, 120.0f);
    ParticlesSpawnBoostTrail(&particles, (Vector2){64.0f, 30.0f},
                             (Vector2){200.0f, 0.0f}, 2);
    ParticlesSpawnBoostBurst(&particles, (Vector2){64.0f, 30.0f},
                             (Vector2){200.0f, 0.0f}, 2);
    ParticlesSpawnForceBlast(&particles, (Vector2){64.0f, 40.0f},
                             (Vector2){0.0f, 1.0f});
    ParticlesSpawnSteam(&particles, (Vector2){64.0f, 50.0f});

    before = WorldDigest(&world);
    for (step = 0; step < 120; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }
    CHECK(WorldDigest(&world) == before,
          "visual particles changed the world they were only meant to read");
    WorldUnload(&world);
}

/* --- dynamic terrain bodies --------------------------------------------- */

/* One system for the whole section, initialised per test. It owns a 1.25 MiB
   raster arena, which belongs on the heap rather than on a test's stack — the
   same reason GameState holds it rather than passing it around. */
static DynamicTerrainSystem terrain;

/* Fills a solid rectangle of one material inside a body's raster. */
static void FillBody(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                     int x0, int y0, int x1, int y1, CellMaterial material,
                     float temperature)
{
    int y;

    for (y = y0; y <= y1; ++y) {
        int x;

        for (x = x0; x <= x1; ++x) {
            DynamicTerrainSetCell(system, handle, x, y, material, temperature);
        }
    }
}

static void test_a_fresh_manager_holds_no_bodies(void)
{
    const DynamicTerrainStats *stats;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    stats = DynamicTerrainStatistics(&terrain);
    CHECK(stats->activeBodies == 0, "a fresh manager reports %d bodies",
          stats->activeBodies);
    CHECK(stats->allocatedDynamicCells == 0,
          "a fresh manager reserves %d cells", stats->allocatedDynamicCells);
    CHECK(DynamicTerrainGet(&terrain, TerrainBodyInvalidHandle()) == NULL,
          "the invalid handle resolved to a body");
    DynamicTerrainUnload(&terrain);
}

/* `TerrainBodyHandle handle = {0};` is what callers write without thinking.
   It must not name body zero: generation zero is reserved as never-live
   precisely so that the lazy initialiser fails instead of silently working
   until the first slot is reused. */
static void test_a_zero_initialised_handle_names_nothing(void)
{
    TerrainBodyHandle zeroed = {0};
    TerrainBodyHandle real;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    real = DynamicTerrainAllocBody(&terrain, 4, 4);

    CHECK(real.index == 0u, "the test needs the first allocation to take slot 0");
    CHECK(real.generation != 0u, "a live handle carries generation zero");
    CHECK(DynamicTerrainGet(&terrain, zeroed) == NULL,
          "a zero-initialised handle resolved to body zero");

    DynamicTerrainSetCell(&terrain, zeroed, 1, 1, MATERIAL_ROCK, 20.0f);
    CHECK(DynamicTerrainGetConst(&terrain, real)->cellCount == 0,
          "a zero-initialised handle wrote into body zero");
    DynamicTerrainFreeBody(&terrain, zeroed);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 1,
          "a zero-initialised handle freed body zero");
    DynamicTerrainUnload(&terrain);
}

static void test_allocation_returns_a_usable_body(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 8, 4);
    body = DynamicTerrainGet(&terrain, handle);

    CHECK(body != NULL, "allocation returned an unusable handle");
    CHECK(body->active && body->awake, "a new body is not active and awake");
    CHECK(body->width == 8 && body->height == 4, "body raster is %dx%d",
          body->width, body->height);
    CHECK(body->cellCount == 0, "a new body already holds %d cells",
          body->cellCount);
    CHECK(DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 32,
          "reserving an 8x4 raster reported %d cells",
          DynamicTerrainStatistics(&terrain)->allocatedDynamicCells);
    DynamicTerrainUnload(&terrain);
}

/* Every budget is enforced by refusing work, never by growing. */
static void test_the_manager_refuses_work_past_its_budgets(void)
{
    TerrainBodyHandle handles[MAX_TERRAIN_BODIES];
    TerrainBodyHandle overflow;
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    /* A shape that does not fit a slot is refused before any slot is used. */
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain,
                                                    TERRAIN_BODY_MAX_SPAN + 1, 1)) == NULL,
          "a body wider than the span limit was accepted");
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain, 128, 128)) == NULL,
          "a raster larger than the per-body capacity was accepted");
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain, 0, 4)) == NULL,
          "a zero-width body was accepted");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "a refused allocation still consumed a slot");

    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        handles[index] = DynamicTerrainAllocBody(&terrain, 4, 4);
        CHECK(DynamicTerrainGet(&terrain, handles[index]) != NULL,
              "slot %d of %d could not be allocated", index,
              MAX_TERRAIN_BODIES);
    }
    overflow = DynamicTerrainAllocBody(&terrain, 4, 4);
    CHECK(DynamicTerrainGet(&terrain, overflow) == NULL,
          "the manager allocated past MAX_TERRAIN_BODIES");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == MAX_TERRAIN_BODIES,
          "a refused allocation changed the active count");
    DynamicTerrainUnload(&terrain);
}

static void test_a_freed_slot_is_reused(void)
{
    TerrainBodyHandle first;
    TerrainBodyHandle second;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    first = DynamicTerrainAllocBody(&terrain, 6, 6);
    CHECK(DynamicTerrainGet(&terrain, first) != NULL, "allocation failed");

    DynamicTerrainFreeBody(&terrain, first);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "freeing a body left it counted");
    CHECK(DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 0,
          "freeing a body left its raster reserved");

    second = DynamicTerrainAllocBody(&terrain, 6, 6);
    CHECK(DynamicTerrainGet(&terrain, second) != NULL,
          "the freed slot could not be reused");
    CHECK(second.index == first.index, "reuse picked slot %u instead of %u",
          second.index, first.index);
    DynamicTerrainUnload(&terrain);
}

/* The reason handles carry a generation at all: a reference held across a free
   must not silently address whatever body lands in the slot next. */
static void test_a_stale_handle_never_resolves_to_the_new_body(void)
{
    TerrainBodyHandle stale;
    TerrainBodyHandle fresh;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    stale = DynamicTerrainAllocBody(&terrain, 4, 4);
    DynamicTerrainSetCell(&terrain, stale, 0, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFreeBody(&terrain, stale);

    CHECK(DynamicTerrainGet(&terrain, stale) == NULL,
          "a handle to a freed body still resolves");

    fresh = DynamicTerrainAllocBody(&terrain, 4, 4);
    CHECK(fresh.index == stale.index, "the test needs the slot to be reused");
    CHECK(fresh.generation != stale.generation,
          "the reused slot kept generation %u", fresh.generation);
    CHECK(DynamicTerrainGet(&terrain, stale) == NULL,
          "the stale handle resolved to the body that replaced it");

    /* Writing through a stale handle must be a no-op, not a write into the
       new body's raster. */
    DynamicTerrainSetCell(&terrain, stale, 1, 1, MATERIAL_SAND, 20.0f);
    CHECK(DynamicTerrainCellAt(&terrain, fresh, 1, 1) == MATERIAL_EMPTY,
          "a stale handle wrote into the body that replaced it");
    CHECK(DynamicTerrainGet(&terrain, fresh)->cellCount == 0,
          "a stale write changed the new body's cell count");

    /* Double free is safe and does not disturb the live body. */
    DynamicTerrainFreeBody(&terrain, stale);
    CHECK(DynamicTerrainGet(&terrain, fresh) != NULL,
          "freeing a stale handle killed the live body");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 1,
          "freeing a stale handle changed the active count");
    DynamicTerrainUnload(&terrain);
}

static void test_reset_releases_every_body(void)
{
    TerrainBodyHandle handles[4];
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    for (index = 0; index < 4; ++index) {
        handles[index] = DynamicTerrainAllocBody(&terrain, 8, 8);
        CHECK(DynamicTerrainGet(&terrain, handles[index]) != NULL,
              "allocation %d failed", index);
    }

    DynamicTerrainReset(&terrain);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "reset left %d bodies alive",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    CHECK(DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 0,
          "reset left %d cells reserved",
          DynamicTerrainStatistics(&terrain)->allocatedDynamicCells);
    for (index = 0; index < 4; ++index) {
        CHECK(DynamicTerrainGet(&terrain, handles[index]) == NULL,
              "handle %d survived the reset", index);
    }
    /* Peaks are session figures and deliberately outlive a reset. */
    CHECK(DynamicTerrainStatistics(&terrain)->peakBodies == 4,
          "reset erased the peak body count");
    CHECK(DynamicTerrainStatistics(&terrain)->peakDynamicCells == 256,
          "reset erased the peak cell count");
    DynamicTerrainUnload(&terrain);
}

/* A body must preserve enough of each cell to be drawn, collided with and put
   back: which material it is, and how hot it was. */
static void test_a_body_preserves_the_material_and_heat_it_was_given(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 5, 3);
    DynamicTerrainSetCell(&terrain, handle, 0, 0, MATERIAL_ROCK, 715.5f);
    DynamicTerrainSetCell(&terrain, handle, 4, 2, MATERIAL_ICE, -14.0f);
    DynamicTerrainSetCell(&terrain, handle, 2, 1, MATERIAL_DIRT, 20.0f);

    CHECK(DynamicTerrainCellAt(&terrain, handle, 0, 0) == MATERIAL_ROCK &&
              DynamicTerrainCellAt(&terrain, handle, 4, 2) == MATERIAL_ICE &&
              DynamicTerrainCellAt(&terrain, handle, 2, 1) == MATERIAL_DIRT,
          "the raster did not keep the materials it was given");
    CHECK(DynamicTerrainCellAt(&terrain, handle, 1, 1) == MATERIAL_EMPTY,
          "an untouched slot is not empty");
    /* Exact, not approximately: temperature is stored as the float World holds,
       so a cell sitting just under a phase threshold cannot cross it merely by
       being torn off and put back. */
    CHECK(DynamicTerrainTemperatureAt(&terrain, handle, 0, 0) == 715.5f,
          "rock came back at %.3f degrees instead of 715.5",
          (double)DynamicTerrainTemperatureAt(&terrain, handle, 0, 0));
    CHECK(DynamicTerrainTemperatureAt(&terrain, handle, 4, 2) == -14.0f,
          "ice came back at %.3f degrees instead of -14",
          (double)DynamicTerrainTemperatureAt(&terrain, handle, 4, 2));

    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->cellCount == 3, "three writes produced %d cells",
          body->cellCount);

    /* Clearing a cell gives the slot back. */
    DynamicTerrainSetCell(&terrain, handle, 2, 1, MATERIAL_EMPTY, 0.0f);
    CHECK(body->cellCount == 2, "clearing a cell left %d cells",
          body->cellCount);
    DynamicTerrainUnload(&terrain);
}

/* Reads and writes outside the raster must be refused rather than reaching a
   neighbouring body: bodies share one arena. */
static void test_out_of_range_cell_access_is_safe(void)
{
    TerrainBodyHandle first;
    TerrainBodyHandle second;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    first = DynamicTerrainAllocBody(&terrain, 4, 4);
    second = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, second, 0, 0, 3, 3, MATERIAL_SAND, 20.0f);

    DynamicTerrainSetCell(&terrain, first, 4, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(&terrain, first, 0, 4, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(&terrain, first, -1, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(&terrain, first, 999, 999, MATERIAL_ROCK, 20.0f);

    CHECK(DynamicTerrainGetConst(&terrain, first)->cellCount == 0,
          "an out-of-range write was counted");
    CHECK(DynamicTerrainGetConst(&terrain, second)->cellCount == 16,
          "an out-of-range write reached the next body");
    CHECK(DynamicTerrainCellAt(&terrain, first, 4, 0) == MATERIAL_EMPTY &&
              DynamicTerrainCellAt(&terrain, first, -1, -1) == MATERIAL_EMPTY,
          "an out-of-range read returned material");
    CHECK(DynamicTerrainTemperatureAt(&terrain, first, 99, 99) == 0.0f,
          "an out-of-range temperature read returned heat");
    DynamicTerrainUnload(&terrain);
}

static void test_finalize_reports_the_occupied_bounds(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A raster with deliberate slack around its contents. */
    handle = DynamicTerrainAllocBody(&terrain, 16, 12);
    FillBody(&terrain, handle, 3, 2, 9, 7, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);

    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->minimumX == 3 && body->maximumX == 9 && body->minimumY == 2 &&
              body->maximumY == 7,
          "bounds are %d..%d, %d..%d instead of 3..9, 2..7", body->minimumX,
          body->maximumX, body->minimumY, body->maximumY);
    CHECK(body->cellCount == 7 * 6, "finalize counted %d cells instead of 42",
          body->cellCount);

    /* An empty body reports an inverted range rather than a false extent. */
    FillBody(&terrain, handle, 3, 2, 9, 7, MATERIAL_EMPTY, 0.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    CHECK(body->cellCount == 0 && body->maximumX < body->minimumX,
          "an emptied body still claims an extent");
    CHECK(body->mass == 0.0f && body->inertia == 0.0f,
          "an emptied body kept mass %.3f and inertia %.3f", (double)body->mass,
          (double)body->inertia);
    DynamicTerrainUnload(&terrain);
}

static void test_mass_follows_material_density(void)
{
    TerrainBodyHandle rock;
    TerrainBodyHandle ice;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    rock = DynamicTerrainAllocBody(&terrain, 4, 4);
    ice = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, rock, 0, 0, 3, 3, MATERIAL_ROCK, 20.0f);
    FillBody(&terrain, ice, 0, 0, 3, 3, MATERIAL_ICE, -14.0f);
    DynamicTerrainFinalizeBody(&terrain, rock);
    DynamicTerrainFinalizeBody(&terrain, ice);

    CHECK(fabsf(DynamicTerrainGetConst(&terrain, rock)->mass -
                16.0f * MaterialAt(MATERIAL_ROCK)->density) < 0.001f,
          "sixteen rock cells weigh %.3f",
          (double)DynamicTerrainGetConst(&terrain, rock)->mass);
    /* The property that actually matters downstream: the same slab of ice must
       be lighter than the same slab of rock. */
    CHECK(DynamicTerrainGetConst(&terrain, ice)->mass <
              DynamicTerrainGetConst(&terrain, rock)->mass,
          "a slab of ice is not lighter than the same slab of rock");
    DynamicTerrainUnload(&terrain);
}

static void test_centre_of_mass_lands_where_the_shape_says(void)
{
    TerrainBodyHandle even;
    TerrainBodyHandle lopsided;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    /* A uniform 4x4 block centred on its own middle: cell centres run from 0.5
       to 3.5, so the mean is 2.0. */
    even = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, even, 0, 0, 3, 3, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, even);
    body = DynamicTerrainGetConst(&terrain, even);
    CHECK(fabsf(body->centerOfMass.x - 2.0f) < 0.001f &&
              fabsf(body->centerOfMass.y - 2.0f) < 0.001f,
          "a uniform block centres at %.3f,%.3f instead of 2,2",
          (double)body->centerOfMass.x, (double)body->centerOfMass.y);

    /* Half rock, half ice: the centre must sit toward the heavier half. */
    lopsided = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, lopsided, 0, 0, 1, 3, MATERIAL_ROCK, 20.0f);
    FillBody(&terrain, lopsided, 2, 0, 3, 3, MATERIAL_ICE, -14.0f);
    DynamicTerrainFinalizeBody(&terrain, lopsided);
    body = DynamicTerrainGetConst(&terrain, lopsided);
    CHECK(body->centerOfMass.x < 2.0f,
          "the centre of mass ignored density: x = %.3f",
          (double)body->centerOfMass.x);
    CHECK(fabsf(body->centerOfMass.y - 2.0f) < 0.001f,
          "a vertically uniform body centres at y = %.3f instead of 2",
          (double)body->centerOfMass.y);
    DynamicTerrainUnload(&terrain);
}

static void test_inertia_grows_with_how_spread_out_a_body_is(void)
{
    TerrainBodyHandle single;
    TerrainBodyHandle compact;
    TerrainBodyHandle spread;
    float compactInertia;
    float spreadInertia;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    /* A one-cell body must still resist rotation. Zero inertia would give it
       infinite angular acceleration the moment anything touched it, which is
       why each cell contributes its own moment as well as its distance. */
    single = DynamicTerrainAllocBody(&terrain, 1, 1);
    DynamicTerrainSetCell(&terrain, single, 0, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, single);
    CHECK(DynamicTerrainGetConst(&terrain, single)->inertia > 0.0f,
          "a single-cell body has zero moment of inertia");

    /* Same mass, different spread: sixteen cells packed 4x4 against sixteen
       cells strung out 16x1. */
    compact = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, compact, 0, 0, 3, 3, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, compact);
    spread = DynamicTerrainAllocBody(&terrain, 16, 1);
    FillBody(&terrain, spread, 0, 0, 15, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, spread);

    compactInertia = DynamicTerrainGetConst(&terrain, compact)->inertia;
    spreadInertia = DynamicTerrainGetConst(&terrain, spread)->inertia;
    CHECK(fabsf(DynamicTerrainGetConst(&terrain, compact)->mass -
                DynamicTerrainGetConst(&terrain, spread)->mass) < 0.001f,
          "the two shapes were meant to weigh the same");
    CHECK(spreadInertia > compactInertia * 2.0f,
          "a long bar (%.2f) does not resist rotation far more than a compact "
          "block of the same mass (%.2f)", (double)spreadInertia,
          (double)compactInertia);
    DynamicTerrainUnload(&terrain);
}

/* The subsystem never receives a World, which is the strongest form of this
   guarantee; the test states the intention so a future signature change that
   hands it one has to argue with something. */
static void test_the_body_manager_leaves_the_world_alone(void)
{
    World world;
    TerrainBodyHandle handle;
    uint64_t before;
    int activeBefore;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    WorldGenerate(&world, 0xB0D1E5u);
    Tick(&world, 4);
    before = WorldDigest(&world);
    activeBefore = world.activeChunkCount;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 12, 12);
    FillBody(&terrain, handle, 0, 0, 11, 11, MATERIAL_ROCK, 400.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainFreeBody(&terrain, handle);
    DynamicTerrainReset(&terrain);
    DynamicTerrainUnload(&terrain);

    CHECK(WorldDigest(&world) == before,
          "the body manager changed the world it never received");
    CHECK(world.activeChunkCount == activeBefore,
          "the body manager woke chunks");
    WorldUnload(&world);
}

/* GameState owns the subsystem, so its lifecycle has to survive a regeneration
   without leaking bodies from the world that no longer exists. */
static void test_regenerating_the_world_drops_its_detached_pieces(void)
{
    GameConfig config = GameDefaultConfig();
    GameState game;
    TerrainBodyHandle handle;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0xB0D1E5u;
    CHECK(GameInit(&game, config), "game allocation failed");
    CHECK(DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies == 0,
          "a new game already has detached terrain");

    handle = DynamicTerrainAllocBody(&game.dynamicTerrain, 8, 8);
    CHECK(DynamicTerrainGet(&game.dynamicTerrain, handle) != NULL,
          "allocation through GameState failed");

    GameRegenerate(&game);
    CHECK(DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies == 0,
          "regenerating the world kept %d bodies cut from the old one",
          DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies);
    CHECK(DynamicTerrainGet(&game.dynamicTerrain, handle) == NULL,
          "a handle survived the world it was cut from");
    GameUnload(&game);
}

/* --- connected solid components ----------------------------------------- */

/* One workspace for the whole section. It is about 34 KiB, which belongs in a
   long-lived owner rather than on the stack of every test — and that is exactly
   how the future dynamic terrain system will hold it. */
static WorldComponentWorkspace componentWorkspace;

static const char *ComponentStatusName(WorldComponentStatus status)
{
    switch (status) {
    case WORLD_COMPONENT_DETACHED: return "DETACHED";
    case WORLD_COMPONENT_ANCHORED: return "ANCHORED";
    case WORLD_COMPONENT_UNKNOWN: return "UNKNOWN";
    case WORLD_COMPONENT_TOO_LARGE: return "TOO_LARGE";
    default: return "INVALID";
    }
}

static WorldComponentResult FindComponent(const World *world, Rectangle region,
                                          int seedX, int seedY)
{
    return WorldFindComponent(world, &componentWorkspace, region, seedX, seedY,
                              WORLD_COMPONENT_MAX_CELLS);
}

/* Every fixture below uses a world large enough to hold three chunks in each
   direction, so a component can be placed across a chunk border on purpose. */
static Rectangle WholeWorld(const World *world)
{
    return (Rectangle){0.0f, 0.0f, (float)world->width, (float)world->height};
}

static void test_a_lone_island_is_reported_detached(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 40, 40, 44, 44, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 42, 42);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "a lone island reported %s", ComponentStatusName(found.status));
    CHECK(found.cellCount == 25, "island has %d cells, expected 25",
          found.cellCount);
    CHECK(found.minimumX == 40 && found.maximumX == 44 &&
              found.minimumY == 40 && found.maximumY == 44,
          "island bounds are %d..%d, %d..%d", found.minimumX, found.maximumX,
          found.minimumY, found.maximumY);
    WorldUnload(&world);
}

static void test_two_islands_are_separate_components(void)
{
    World world;
    WorldComponentResult first;
    WorldComponentResult second;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 10, 10, 14, 14, MATERIAL_ROCK);
    FillRect(&world, 60, 60, 64, 64, MATERIAL_ROCK);

    first = FindComponent(&world, WholeWorld(&world), 12, 12);
    second = FindComponent(&world, WholeWorld(&world), 62, 62);

    CHECK(first.status == WORLD_COMPONENT_DETACHED &&
              second.status == WORLD_COMPONENT_DETACHED,
          "islands reported %s and %s", ComponentStatusName(first.status),
          ComponentStatusName(second.status));
    CHECK(first.cellCount == 25 && second.cellCount == 25,
          "islands have %d and %d cells, expected 25 each", first.cellCount,
          second.cellCount);
    /* The point of the test: neither search wandered into the other island. */
    CHECK(first.maximumX == 14 && second.minimumX == 60,
          "the two islands were merged into one component");
    WorldUnload(&world);
}

/* Water is not solid, so it cannot hold two blocks of rock together. This pins
   the membership rule down: a component is made of solid cells, not of
   non-empty ones. */
static void test_a_liquid_gap_does_not_join_two_components(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 20, 40, 24, 44, MATERIAL_ROCK);
    FillRect(&world, 25, 40, 29, 44, MATERIAL_WATER);
    FillRect(&world, 30, 40, 34, 44, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 22, 42);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "the left block reported %s", ComponentStatusName(found.status));
    CHECK(found.cellCount == 25,
          "water joined the two blocks: %d cells instead of 25",
          found.cellCount);
    WorldUnload(&world);
}

/* The floor of these fixtures spans the whole world, so it touches the map
   border — which the simulation already treats as immovable rock. */
static void BuildIslandOnABridge(World *world)
{
    FillRect(world, 0, 80, world->width - 1, world->height - 1, MATERIAL_ROCK);
    FillRect(world, 30, 20, 40, 30, MATERIAL_ROCK);
    FillRect(world, 35, 31, 35, 79, MATERIAL_ROCK);
}

static void test_an_intact_bridge_to_the_ground_prevents_detachment(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    BuildIslandOnABridge(&world);

    found = FindComponent(&world, WholeWorld(&world), 35, 25);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "an island still bridged to the ground reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

static void test_cutting_the_bridge_detaches_the_island(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    BuildIslandOnABridge(&world);
    FillRect(&world, 35, 50, 35, 52, MATERIAL_EMPTY);

    found = FindComponent(&world, WholeWorld(&world), 35, 25);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "an island cut free reported %s", ComponentStatusName(found.status));
    /* 11x11 island plus the nineteen cells of bridge still hanging from it. */
    CHECK(found.cellCount == 121 + 19, "the freed piece has %d cells, expected 140",
          found.cellCount);
    CHECK(found.minimumY == 20 && found.maximumY == 49,
          "the freed piece spans rows %d..%d, expected 20..49", found.minimumY,
          found.maximumY);

    /* The stump below the cut is still part of the ground. */
    found = FindComponent(&world, WholeWorld(&world), 35, 60);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "the stump left standing on the floor reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

/* Connectivity is four-neighbour, and that is a decision rather than an
   accident, so it is pinned here in both directions. Two blocks meeting at a
   corner are two components... */
static void test_a_corner_contact_does_not_join_two_components(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 40, 40, 44, 44, MATERIAL_ROCK);
    FillRect(&world, 45, 45, 49, 49, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 42, 42);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "the upper block reported %s", ComponentStatusName(found.status));
    CHECK(found.cellCount == 25,
          "a corner contact merged two blocks: %d cells instead of 25",
          found.cellCount);
    WorldUnload(&world);
}

/* ...and the price of that, stated openly: a piece joined to the ground by a
   single diagonal staircase reads as free. Such a join is one cell thick and
   would not hold anything up, but a caller that wants a minimum thickness
   before tearing terrain off has to impose it itself. Changing connectivity
   later should mean deliberately editing this test, not discovering it. */
static void test_a_diagonal_only_join_to_the_ground_reads_as_detached(void)
{
    World world;
    WorldComponentResult found;
    int step;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 0, 80, world.width - 1, world.height - 1, MATERIAL_ROCK);
    FillRect(&world, 30, 20, 40, 30, MATERIAL_ROCK);
    /* A staircase from the island's bottom-right corner down to the floor.
       Every cell touches the next only at a corner, the first touches the
       island only at a corner, and the hole punched under the last one leaves
       it touching the floor only at a corner too. */
    for (step = 0; step < 49; ++step) {
        WorldSetCell(&world, 41 + step, 31 + step, MATERIAL_ROCK);
    }
    WorldSetCell(&world, 89, 80, MATERIAL_EMPTY);

    found = FindComponent(&world, WholeWorld(&world), 35, 25);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "a diagonally joined island reported %s",
          ComponentStatusName(found.status));
    CHECK(found.cellCount == 121,
          "the staircase was counted as part of the island: %d cells",
          found.cellCount);
    WorldUnload(&world);
}

/* Nothing in the detector is chunk-aware, and this test exists to keep it that
   way: a future chunk-local optimisation must not quietly cut components at a
   chunk border. */
static void test_a_component_crossing_a_chunk_boundary_stays_whole(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    CHECK(28 / WORLD_CHUNK_SIZE != 36 / WORLD_CHUNK_SIZE,
          "the fixture no longer straddles a chunk border");
    FillRect(&world, 28, 28, 36, 36, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 28, 28);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "a component across a chunk border reported %s",
          ComponentStatusName(found.status));
    CHECK(found.cellCount == 81,
          "a component across a chunk border has %d cells, expected 81",
          found.cellCount);
    CHECK(found.minimumX == 28 && found.maximumX == 36 &&
              found.minimumY == 28 && found.maximumY == 36,
          "bounds %d..%d, %d..%d were cut at a chunk border", found.minimumX,
          found.maximumX, found.minimumY, found.maximumY);
    WorldUnload(&world);
}

/* Reaching the edge of the query region is not by itself a reason to give up:
   the detector peeks one cell past it to learn whether the component actually
   continues. It gives up only when something solid is really there. */
static void test_a_component_continuing_past_the_region_is_unknown(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 10, 40, 60, 40, MATERIAL_ROCK);

    found = FindComponent(&world, (Rectangle){20.0f, 30.0f, 21.0f, 21.0f}, 30, 40);
    CHECK(found.status == WORLD_COMPONENT_UNKNOWN,
          "a bar running out of the region reported %s",
          ComponentStatusName(found.status));
    /* A failed search reports nothing rather than however much it explored: a
       partial component is not a smaller component, and handing one back
       invites a caller to act on it. */
    CHECK(found.cellCount == 0 && found.minimumX == 0 && found.maximumX == 0,
          "an unknown result carried %d cells and bounds %d..%d",
          found.cellCount, found.minimumX, found.maximumX);
    WorldUnload(&world);
}

static void test_a_component_that_only_touches_the_region_edge_is_detached(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 22, 60, 26, 64, MATERIAL_ROCK);

    /* A region exactly the size of the island: every cell of it sits on the
       region edge, and every neighbour outside is empty. */
    found = FindComponent(&world, (Rectangle){22.0f, 60.0f, 5.0f, 5.0f}, 24, 62);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "an island filling its region reported %s",
          ComponentStatusName(found.status));
    CHECK(found.cellCount == 25, "island has %d cells, expected 25",
          found.cellCount);
    WorldUnload(&world);
}

static void test_a_component_touching_the_world_edge_is_anchored(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 0, 40, 4, 44, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 2, 42);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "a component against the map border reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

static void test_a_component_larger_than_the_budget_is_too_large(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 10, 10, 29, 29, MATERIAL_ROCK);

    found = WorldFindComponent(&world, &componentWorkspace, WholeWorld(&world),
                               20, 20, 50);
    CHECK(found.status == WORLD_COMPONENT_TOO_LARGE,
          "a 400-cell block under a 50-cell budget reported %s",
          ComponentStatusName(found.status));

    /* The same block fits comfortably in the full workspace. */
    found = FindComponent(&world, WholeWorld(&world), 20, 20);
    CHECK(found.status == WORLD_COMPONENT_DETACHED && found.cellCount == 400,
          "the same block reported %s with %d cells",
          ComponentStatusName(found.status), found.cellCount);
    WorldUnload(&world);
}

/* A region wider than the visited bitmap can index is refused outright.
   Clipping it instead would silently answer a question the caller did not
   ask. */
static void test_an_oversized_or_malformed_query_is_refused(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    FillRect(&world, 40, 40, 44, 44, MATERIAL_ROCK);

    found = FindComponent(&world, (Rectangle){0.0f, 0.0f, 200.0f, 100.0f}, 42, 42);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "a %d-wide region reported %s instead of being refused",
          WORLD_COMPONENT_MAX_SPAN + 1, ComponentStatusName(found.status));

    /* The span is judged on what the caller asked for, before the world clips
       it. Otherwise the same oversized region would be refused in open ground
       and accepted at the border, purely because the map trimmed it. */
    found = FindComponent(&world, (Rectangle){-160.0f, 0.0f, 200.0f, 100.0f},
                          42, 42);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "an oversized region was accepted at the map border, reporting %s",
          ComponentStatusName(found.status));

    /* A legal region that merely hangs over the edge is still fine. */
    FillRect(&world, 0, 40, 4, 44, MATERIAL_ROCK);
    found = FindComponent(&world, (Rectangle){-40.0f, 20.0f, 100.0f, 60.0f}, 2, 42);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "a legal region overhanging the border reported %s",
          ComponentStatusName(found.status));

    found = FindComponent(&world, (Rectangle){20.0f, 20.0f, 64.0f, 64.0f}, 10, 10);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "a seed outside the region reported %s",
          ComponentStatusName(found.status));

    found = FindComponent(&world, (Rectangle){20.0f, 20.0f, 64.0f, 64.0f}, 60, 60);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "a seed on an empty cell reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

/* The detector is a query, not a step of the simulation. It must leave no trace
   at all — not a material, not a temperature, and not a woken chunk. */
static void test_the_detector_never_changes_the_world(void)
{
    World world;
    uint64_t before;
    int activeBefore;
    uint32_t tickBefore;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    WorldGenerate(&world, 0xC0FFEEu);
    FillRect(&world, 60, 40, 70, 50, MATERIAL_ROCK);
    FillRect(&world, 100, 40, 160, 40, MATERIAL_ROCK);
    Tick(&world, 4);

    before = WorldDigest(&world);
    activeBefore = world.activeChunkCount;
    tickBefore = world.tick;

    /* One query of each outcome the detector can produce. */
    (void)FindComponent(&world, (Rectangle){40.0f, 20.0f, 64.0f, 64.0f}, 65, 45);
    (void)FindComponent(&world, (Rectangle){100.0f, 20.0f, 32.0f, 64.0f}, 110, 40);
    (void)FindComponent(&world, (Rectangle){0.0f, 100.0f, 100.0f, 44.0f}, 10, 130);
    (void)WorldFindComponent(&world, &componentWorkspace,
                             (Rectangle){40.0f, 20.0f, 64.0f, 64.0f}, 65, 45, 4);
    (void)FindComponent(&world, (Rectangle){0.0f, 0.0f, 200.0f, 100.0f}, 65, 45);

    CHECK(WorldDigest(&world) == before,
          "the detector changed materials or temperatures");
    CHECK(world.activeChunkCount == activeBefore,
          "the detector woke chunks: %d active, expected %d",
          world.activeChunkCount, activeBefore);
    CHECK(world.tick == tickBefore, "the detector advanced the tick counter");
    WorldUnload(&world);
}

/* --- world to body extraction ------------------------------------------- */

/* Extraction is the first thing that joins the detector to the body store, and
   its one hard promise is atomicity: either it completes, or the world is
   exactly as it was. Most of these tests are therefore failure tests that
   compare a digest across the attempt. */

static TerrainExtractResult ExtractAt(World *world, DynamicTerrainSystem *system,
                                      Rectangle region, int seedX, int seedY)
{
    WorldComponentResult component =
        WorldFindComponent(world, &componentWorkspace, region, seedX, seedY,
                           WORLD_COMPONENT_MAX_CELLS);

    return TerrainExtractComponent(world, system, &componentWorkspace, component);
}

/* An island of two materials at two temperatures, so a test can tell whether
   the body kept what it was given rather than merely the right number of
   cells. */
static void BuildExtractableIsland(World *world)
{
    FillRect(world, 40, 30, 47, 35, MATERIAL_ROCK);
    FillRect(world, 44, 30, 47, 32, MATERIAL_ICE);
    WorldSetTemperature(world, 41, 34, 640.0f);
    WorldSetTemperature(world, 45, 31, -14.0f);
}

static void test_extraction_moves_an_island_out_of_the_world(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;
    int rockBefore;
    int iceBefore;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    BuildExtractableIsland(&world);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);
    iceBefore = CountMaterial(&world, MATERIAL_ICE);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);
    CHECK(body != NULL, "extraction returned an unusable handle");

    /* The static world no longer holds any of it. */
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 0 &&
              CountMaterial(&world, MATERIAL_ICE) == 0,
          "extraction left %d rock and %d ice cells behind",
          CountMaterial(&world, MATERIAL_ROCK),
          CountMaterial(&world, MATERIAL_ICE));

    /* And the body holds all of it: nothing was lost in transit. */
    CHECK(body->cellCount == rockBefore + iceBefore,
          "body holds %d cells, the island had %d", body->cellCount,
          rockBefore + iceBefore);
    CHECK(extracted.cellCount == body->cellCount,
          "the result claims %d cells but the body has %d", extracted.cellCount,
          body->cellCount);
    CHECK(DynamicTerrainStatistics(&terrain)->extractionsSucceeded == 1 &&
              DynamicTerrainStatistics(&terrain)->extractionsFailed == 0,
          "counters read %d succeeded / %d failed",
          DynamicTerrainStatistics(&terrain)->extractionsSucceeded,
          DynamicTerrainStatistics(&terrain)->extractionsFailed);
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_extraction_conserves_materials_and_heat(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;
    int rock = 0;
    int ice = 0;
    int localX;
    int localY;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    BuildExtractableIsland(&world);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);

    for (localY = 0; localY < body->height; ++localY) {
        for (localX = 0; localX < body->width; ++localX) {
            CellMaterial material =
                DynamicTerrainCellAt(&terrain, extracted.body, localX, localY);

            if (material == MATERIAL_ROCK) ++rock;
            if (material == MATERIAL_ICE) ++ice;
        }
    }
    /* The island is 8x6 rock with a 4x3 corner replaced by ice. */
    CHECK(rock == 8 * 6 - 4 * 3 && ice == 4 * 3,
          "body holds %d rock and %d ice, expected %d and %d", rock, ice,
          8 * 6 - 4 * 3, 4 * 3);

    /* Temperature travels as the float the world held, exactly. */
    CHECK(DynamicTerrainTemperatureAt(&terrain, extracted.body, 1, 4) == 640.0f,
          "the hot cell arrived at %.3f instead of 640",
          (double)DynamicTerrainTemperatureAt(&terrain, extracted.body, 1, 4));
    CHECK(DynamicTerrainTemperatureAt(&terrain, extracted.body, 5, 1) == -14.0f,
          "the frozen cell arrived at %.3f instead of -14",
          (double)DynamicTerrainTemperatureAt(&terrain, extracted.body, 5, 1));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Local (0,0) is the component's bounding-box corner and the body's origin is
   its centre of mass, so the documented transform has to round-trip. */
static void test_extraction_places_the_body_where_the_island_was(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A uniform 8x6 block, whose centre of mass is its middle. */
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);

    CHECK(body->sourceX == 40 && body->sourceY == 30,
          "the body came from %d,%d instead of 40,30", body->sourceX,
          body->sourceY);
    CHECK(body->width == 8 && body->height == 6, "raster is %dx%d, expected 8x6",
          body->width, body->height);
    CHECK(body->minimumX == 0 && body->maximumX == 7 && body->minimumY == 0 &&
              body->maximumY == 5,
          "local bounds are %d..%d, %d..%d", body->minimumX, body->maximumX,
          body->minimumY, body->maximumY);
    CHECK(fabsf(body->centerOfMass.x - 4.0f) < 0.001f &&
              fabsf(body->centerOfMass.y - 3.0f) < 0.001f,
          "local centre of mass is %.3f,%.3f instead of 4,3",
          (double)body->centerOfMass.x, (double)body->centerOfMass.y);
    /* The world position of that centre: 40 + 4, 30 + 3. */
    CHECK(fabsf(body->position.x - 44.0f) < 0.001f &&
              fabsf(body->position.y - 33.0f) < 0.001f,
          "the body sits at %.3f,%.3f instead of 44,33", (double)body->position.x,
          (double)body->position.y);
    CHECK(body->angle == 0.0f && body->velocity.x == 0.0f &&
              body->velocity.y == 0.0f && body->angularVelocity == 0.0f,
          "a freshly extracted body is already moving");
    CHECK(body->mass > 0.0f && body->inertia > 0.0f,
          "mass %.3f and inertia %.3f were not finalised", (double)body->mass,
          (double)body->inertia);
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Clearing goes through the ordinary world write path, so the chunks that held
   the island must be scheduled and marked for a pixel rebuild — otherwise the
   island would still be on screen after being torn out. */
static void test_extraction_wakes_and_dirties_the_chunks_it_emptied(void)
{
    World world;
    TerrainExtractResult extracted;
    size_t chunkIndex;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);

    /* Settle first, then clear the flags, so what the extraction sets is the
       only thing left to see. */
    Tick(&world, 3);
    chunkIndex = (size_t)(30 / WORLD_CHUNK_SIZE) * (size_t)world.chunkColumns +
                 (size_t)(40 / WORLD_CHUNK_SIZE);
    memset(world.dirtyChunks, 0,
           (size_t)world.chunkColumns * (size_t)world.chunkRows);
    memset(world.lightDirtyChunks, 0,
           (size_t)world.chunkColumns * (size_t)world.chunkRows);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    CHECK(world.dirtyChunks[chunkIndex] != 0u,
          "the emptied chunk was not marked for a pixel rebuild");
    CHECK(world.lightDirtyChunks[chunkIndex] != 0u,
          "the emptied chunk did not have its light inputs invalidated");
    CHECK(world.activeChunks[chunkIndex] != 0u,
          "the emptied chunk was not scheduled for simulation");
    CHECK(world.activeChunkCount > 0, "extraction scheduled nothing at all");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_extraction_crosses_a_chunk_boundary(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    CHECK(28 / WORLD_CHUNK_SIZE != 40 / WORLD_CHUNK_SIZE,
          "the fixture no longer straddles a chunk border");
    FillRect(&world, 28, 28, 40, 40, MATERIAL_DIRT);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 30, 30);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);
    CHECK(body->cellCount == 13 * 13,
          "a body across a chunk border holds %d of 169 cells", body->cellCount);
    CHECK(CountMaterial(&world, MATERIAL_DIRT) == 0,
          "%d dirt cells survived on the far side of the chunk border",
          CountMaterial(&world, MATERIAL_DIRT));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Every refusal must leave the world byte-for-byte as it was, and must not
   strand a half-filled body in the store. */
static void CheckExtractionRefused(World *world, DynamicTerrainSystem *system,
                                   TerrainExtractResult extracted,
                                   TerrainExtractStatus expected,
                                   uint64_t digestBefore, int bodiesBefore,
                                   const char *what)
{
    CHECK(extracted.status == expected, "%s reported %s instead of %s", what,
          TerrainExtractStatusName(extracted.status),
          TerrainExtractStatusName(expected));
    CHECK(WorldDigest(world) == digestBefore, "%s changed the world", what);
    CHECK(DynamicTerrainGet(system, extracted.body) == NULL,
          "%s handed back a live body", what);
    CHECK(DynamicTerrainStatistics(system)->activeBodies == bodiesBefore,
          "%s left %d bodies allocated instead of %d", what,
          DynamicTerrainStatistics(system)->activeBodies, bodiesBefore);
}

static void test_an_anchored_component_is_never_extracted(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A floor spanning the map touches the border, so it is anchored. */
    FillRect(&world, 0, 80, 127, 95, MATERIAL_ROCK);
    before = WorldDigest(&world);

    CheckExtractionRefused(&world, &terrain,
                           ExtractAt(&world, &terrain, WholeWorld(&world), 60, 88),
                           TERRAIN_EXTRACT_NOT_DETACHED, before, 0,
                           "extracting anchored ground");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_an_unknown_component_is_never_extracted(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A bar that runs out of the query region: the detector cannot prove it
       free, so extraction must not guess. */
    FillRect(&world, 10, 40, 100, 40, MATERIAL_ROCK);
    before = WorldDigest(&world);

    CheckExtractionRefused(
        &world, &terrain,
        ExtractAt(&world, &terrain, (Rectangle){20.0f, 30.0f, 21.0f, 21.0f}, 30, 40),
        TERRAIN_EXTRACT_NOT_DETACHED, before, 0,
        "extracting an unknown component");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_a_component_the_detector_refused_is_never_extracted(void)
{
    World world;
    WorldComponentResult component;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 30, 30, 60, 60, MATERIAL_ROCK);
    before = WorldDigest(&world);

    /* A budget too small for the block: the detector reports TOO_LARGE, and
       that is not a licence to tear out whatever it managed to walk. */
    component = WorldFindComponent(&world, &componentWorkspace,
                                   WholeWorld(&world), 40, 40, 64);
    CHECK(component.status == WORLD_COMPONENT_TOO_LARGE,
          "the fixture no longer produces TOO_LARGE");
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_NOT_DETACHED, before, 0,
        "extracting a truncated component");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_extraction_without_a_free_body_slot_changes_nothing(void)
{
    World world;
    uint64_t before;
    int index;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    before = WorldDigest(&world);

    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        CHECK(DynamicTerrainGet(&terrain,
                                DynamicTerrainAllocBody(&terrain, 2, 2)) != NULL,
              "could not fill slot %d", index);
    }

    CheckExtractionRefused(&world, &terrain,
                           ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34),
                           TERRAIN_EXTRACT_NO_BODY_SLOT, before,
                           MAX_TERRAIN_BODIES, "extracting with a full store");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 8 * 6,
          "a refused extraction removed rock from the world");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* A ring: few enough cells to pass the detector, but a bounding box larger than
   any body's raster. The store must say so rather than tear out what fits. */
static void test_a_component_too_wide_for_a_body_changes_nothing(void)
{
    World world;
    WorldComponentResult component;
    Rectangle region = {30.0f, 20.0f, 128.0f, 90.0f};
    uint64_t before;

    CHECK(WorldInit(&world, 200, 140), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 30, 20, 157, 20, MATERIAL_ROCK);
    FillRect(&world, 30, 109, 157, 109, MATERIAL_ROCK);
    FillRect(&world, 30, 20, 30, 109, MATERIAL_ROCK);
    FillRect(&world, 157, 20, 157, 109, MATERIAL_ROCK);
    before = WorldDigest(&world);

    component = WorldFindComponent(&world, &componentWorkspace, region, 60, 20,
                                   WORLD_COMPONENT_MAX_CELLS);
    CHECK(component.status == WORLD_COMPONENT_DETACHED,
          "the ring fixture reported %s", ComponentStatusName(component.status));
    CHECK((component.maximumX - component.minimumX + 1) *
                  (component.maximumY - component.minimumY + 1) >
              TERRAIN_BODY_RASTER_CAPACITY,
          "the ring no longer exceeds a body raster");
    CHECK(component.cellCount < MAX_TERRAIN_BODY_CELLS,
          "the ring was meant to fail on its bounding box, not its cell count");

    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_CELL_CAPACITY, before, 0, "extracting an oversized ring");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* A fabricated result is the one input the detector cannot vouch for, so the
   preflight has to stand on its own. */
static void test_a_malformed_component_changes_nothing(void)
{
    World world;
    WorldComponentResult component;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    before = WorldDigest(&world);

    component = (WorldComponentResult){WORLD_COMPONENT_DETACHED, 0, 0, 0, 0, 0};
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting an empty component");

    component = (WorldComponentResult){WORLD_COMPONENT_DETACHED, 4, 40, 30, 39, 35};
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting inverted bounds");

    component = (WorldComponentResult){WORLD_COMPONENT_DETACHED, 4, 40, 30, 200, 35};
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting bounds outside the world");

    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(NULL, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting from no world");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* The detector runs at one moment and extraction at another. If the world moved
   on in between, the recorded cells are stale and copying them would put a hole
   inside a body of rock. */
static void test_a_component_the_world_has_moved_past_changes_nothing(void)
{
    World world;
    WorldComponentResult component;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);

    component = WorldFindComponent(&world, &componentWorkspace,
                                   WholeWorld(&world), 41, 34,
                                   WORLD_COMPONENT_MAX_CELLS);
    CHECK(component.status == WORLD_COMPONENT_DETACHED, "the fixture reported %s",
          ComponentStatusName(component.status));

    /* Something else eats one of its cells between the search and the commit. */
    WorldSetCell(&world, 43, 32, MATERIAL_EMPTY);
    before = WorldDigest(&world);

    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_WORLD_CHANGED, before, 0,
        "extracting a component the world moved past");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 8 * 6 - 1,
          "the stale extraction removed rock anyway: %d cells left",
          CountMaterial(&world, MATERIAL_ROCK));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Extraction, then a reset: the bodies go with the world they were cut from,
   and the store is reusable afterwards. */
static void test_reset_after_extraction_returns_the_store_to_empty(void)
{
    World world;
    TerrainExtractResult extracted;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));

    DynamicTerrainReset(&terrain);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "reset left %d bodies after an extraction",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    CHECK(DynamicTerrainGet(&terrain, extracted.body) == NULL,
          "an extracted body's handle survived the reset");
    /* Outcome counters are session figures and outlive a reset. */
    CHECK(DynamicTerrainStatistics(&terrain)->extractionsSucceeded == 1,
          "reset erased the extraction counter");

    /* And the store still works. */
    FillRect(&world, 60, 30, 67, 35, MATERIAL_ROCK);
    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 61, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK,
          "extraction after a reset reported %s",
          TerrainExtractStatusName(extracted.status));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* --- abilities ----------------------------------------------------------- */

static void test_ability_table_passes_its_own_validation(void)
{
    /* GameInit runs this too. It catches the mistake that matters: a one-shot
       power with no cooldown, which fires on every frame the button is held
       and so quietly becomes the held power it was not meant to be. */
    CHECK(AbilitiesValidate(), "the ability table failed validation");
}

static void test_a_one_shot_ability_respects_its_cooldown(void)
{
    World world;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events = {0};
    bool requested[ABILITY_COUNT] = {false};
    float cooldown = AbilityDefinitionAt(ABILITY_FORCE)->cooldown;
    int fired = 0;
    int step;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    AbilitiesInit(&abilities, 0xE6BEu);
    ParticlesInit(&particles, 0xE6BEu);
    requested[ABILITY_FORCE] = true;

    /* Half the cooldown's worth of frames, all of them asking to fire. */
    for (step = 0; step < 30; ++step) {
        GameEventsClear(&events);
        AbilitiesUpdate(&abilities, &world, &particles, &events,
                        (Vector2){20.0f, 35.0f}, (Vector2){150.0f, 35.0f},
                        cooldown * 0.5f, requested);
        if (AbilityStateAt(&abilities, ABILITY_FORCE)->triggered) {
            ++fired;
        }
    }
    CHECK(fired == 15, "force fired %d times in 30 frames of half-cooldown "
                       "steps instead of 15", fired);
    WorldUnload(&world);
}

static void test_a_held_ability_reports_one_start_per_hold(void)
{
    World world;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events = {0};
    bool requested[ABILITY_COUNT] = {false};
    int starts = 0;
    int activeFrames = 0;
    int step;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    FillRect(&world, 60, 30, 70, 40, MATERIAL_ROCK);
    AbilitiesInit(&abilities, 0xE6BEu);
    ParticlesInit(&particles, 0xE6BEu);

    for (step = 0; step < 12; ++step) {
        /* Held for the first six frames, released, then held again. */
        requested[ABILITY_LASER] = step < 6 || step >= 9;
        GameEventsClear(&events);
        AbilitiesUpdate(&abilities, &world, &particles, &events,
                        (Vector2){20.0f, 35.0f}, (Vector2){150.0f, 35.0f},
                        1.0f / 60.0f, requested);
        if (AbilityStateAt(&abilities, ABILITY_LASER)->triggered) {
            ++starts;
        }
        if (AbilityStateAt(&abilities, ABILITY_LASER)->active) {
            ++activeFrames;
        }
    }
    CHECK(activeFrames == 9, "a held beam ran on %d/9 requested frames",
          activeFrames);
    CHECK(starts == 2, "a beam held twice reported %d starts instead of 2",
          starts);
    WorldUnload(&world);
}

/* Every ability's knockback reaches the player through the event buffer, which
   is what lets the player module stay ignorant of which powers exist. */
static void test_ability_knockback_reaches_the_player_through_events(void)
{
    GameConfig config = GameDefaultConfig();
    GameState game;
    GameEventBuffer events = {0};
    GameInput input = {0};
    Vector2 before;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0xE6BEu;
    CHECK(GameInit(&game, config), "game allocation failed");

    before = game.player.velocity;
    input.aimWorld = (Vector2){game.player.position.x + 40.0f,
                               game.player.position.y};
    input.ability[ABILITY_FORCE] = true;
    GameUpdate(&game, &input, config.fixedStep, &events);

    CHECK(HasGameEvent(&events, GAME_EVENT_FORCE),
          "the force ability published no event");
    CHECK(game.player.velocity.x < before.x - 50.0f,
          "force recoil did not push the player back: %.1f -> %.1f",
          before.x, game.player.velocity.x);
    GameUnload(&game);
}

/* --- material table ----------------------------------------------------- */

static void test_every_material_has_table_data(void)
{
    int material;

    for (material = 0; material < MATERIAL_COUNT; ++material) {
        const char *name = WorldMaterialName((CellMaterial)material);

        CHECK(name != NULL, "material %d has no name", material);
        CHECK(name[0] != '\0', "material %d has an empty name", material);
        /* A gap in the table is zero-filled, which reads back as the EMPTY
           entry: that is exactly the silent failure the table has to catch. */
        CHECK(material == MATERIAL_EMPTY || strcmp(name, "EMPTY") != 0,
              "material %d fell back to the EMPTY entry", material);
    }
}

static void test_material_table_passes_its_own_validation(void)
{
    /* WorldInit runs this too, so a table that fails here fails the whole game
       loudly rather than producing cells that misbehave in one rare path. */
    CHECK(MaterialsValidate(), "the material table failed validation");
}

/* The cryo rate used to be a switch statement that named four materials and
   gave everything else a default. Moving it into the table is only safe if the
   numbers that made cryo work are still the numbers the beam reads. */
static void test_every_material_carries_its_own_cryo_rate(void)
{
    int material;

    for (material = 0; material < MATERIAL_COUNT; ++material) {
        if (material == MATERIAL_EMPTY) {
            continue;
        }
        CHECK(MaterialAt((CellMaterial)material)->chillRate > 0.0f,
              "material %s cannot be chilled at all",
              WorldMaterialName((CellMaterial)material));
    }
    /* Lava relaxes back toward 900C at 8% of the gap per tick, which is about
       22 degrees a tick at its freezing point. A beam that does not clearly
       beat that finds an equilibrium above the threshold and never freezes. */
    CHECK(MaterialAt(MATERIAL_LAVA)->chillRate >
              MaterialAt(MATERIAL_WATER)->chillRate * 10.0f,
          "lava must be chilled far harder than water");
}

static void test_solidity_matches_what_blocks_the_player(void)
{
    CHECK(WorldMaterialIsSolid(MATERIAL_DIRT), "dirt must be solid");
    CHECK(WorldMaterialIsSolid(MATERIAL_ROCK), "rock must be solid");
    CHECK(WorldMaterialIsSolid(MATERIAL_SAND), "sand must be solid");
    CHECK(!WorldMaterialIsSolid(MATERIAL_EMPTY), "empty must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_WATER), "water must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_LAVA), "lava must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_STEAM), "steam must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_SMOKE), "smoke must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_FIRE), "fire must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_ASH), "ash must be passable");
}

/* --- movement ---------------------------------------------------------- */

static void test_sand_falls_to_the_floor_and_is_conserved(void)
{
    World world;
    int before;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 30, 4, 33, 11, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);
    Tick(&world, 300);

    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "sand count changed from %d to %d", before,
          CountMaterial(&world, MATERIAL_SAND));
    CHECK(CountMaterial(&world, MATERIAL_SAND) == 32, "expected 32 sand cells");
    CHECK(WorldGetCell(&world, 31, 47) == MATERIAL_SAND,
          "sand did not reach the floor");
    CHECK(WorldGetCell(&world, 31, 4) == MATERIAL_EMPTY,
          "sand did not leave its starting cell");
    WorldUnload(&world);
}

static void test_sand_falls_at_most_one_cell_per_tick(void)
{
    World world;
    int tick;

    CHECK(WorldInit(&world, 32, 64), "world allocation failed");
    WorldSetCell(&world, 16, 2, MATERIAL_SAND);

    /* A cell that has already moved this tick must not be simulated again, so
       a free-falling grain advances exactly one row per tick. */
    for (tick = 1; tick <= 20; ++tick) {
        WorldUpdate(&world);
        CHECK(WorldGetCell(&world, 16, 2 + tick) == MATERIAL_SAND,
              "after %d ticks the grain was not at row %d", tick, 2 + tick);
    }
    WorldUnload(&world);
}

static void test_water_spreads_sideways_and_is_conserved(void)
{
    World world;
    int before;
    int minimumX = 1 << 30;
    int maximumX = -1;
    int highest = 1 << 30;
    int x;
    int y;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 0, 47, 63, 47, MATERIAL_ROCK);
    FillRect(&world, 30, 30, 33, 40, MATERIAL_WATER);
    before = CountMaterial(&world, MATERIAL_WATER);
    Tick(&world, 400);

    CHECK(CountMaterial(&world, MATERIAL_WATER) == before,
          "water count changed from %d to %d", before,
          CountMaterial(&world, MATERIAL_WATER));

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) != MATERIAL_WATER) {
                continue;
            }
            if (x < minimumX) minimumX = x;
            if (x > maximumX) maximumX = x;
            if (y < highest) highest = y;
        }
    }

    /* The column was four cells wide and eleven tall; once it lands it must
       slump into a much wider, much shallower mound. */
    CHECK(maximumX - minimumX >= 20,
          "water spanned only x=[%d..%d]", minimumX, maximumX);
    CHECK(highest >= 40, "water piled up to row %d instead of slumping", highest);
    WorldUnload(&world);
}

/* --- thermal ----------------------------------------------------------- */

static void test_rock_becomes_lava_above_its_threshold(void)
{
    World world;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 16, 16, MATERIAL_ROCK);
    WorldSetTemperature(&world, 16, 16, 760.0f);
    WorldUpdate(&world);

    CHECK(WorldGetCell(&world, 16, 16) == MATERIAL_LAVA,
          "rock above 720C did not melt");
    WorldUnload(&world);
}

static void test_water_becomes_steam_above_its_threshold(void)
{
    World world;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 16, 16, MATERIAL_WATER);
    WorldSetTemperature(&world, 16, 16, 120.0f);
    WorldUpdate(&world);

    CHECK(WorldGetCell(&world, 16, 16) == MATERIAL_STEAM,
          "water above 108C did not boil");
    WorldUnload(&world);
}

static void test_water_and_lava_react_into_steam_and_rock(void)
{
    World world;
    bool reactionSeen = false;
    int tick;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    FillRect(&world, 0, 31, 31, 31, MATERIAL_ROCK);
    WorldSetCell(&world, 16, 20, MATERIAL_WATER);
    WorldSetCell(&world, 17, 20, MATERIAL_LAVA);

    for (tick = 0; tick < 60 && !reactionSeen; ++tick) {
        WorldUpdate(&world);
        reactionSeen = world.reactionCount > 0;
    }

    CHECK(reactionSeen, "no reaction event was published");
    CHECK(CountMaterial(&world, MATERIAL_STEAM) > 0, "no steam was produced");
    WorldUnload(&world);
}

static void test_one_fire_cell_cannot_consume_a_whole_dirt_field(void)
{
    /* The containment budget is the invariant that keeps fire from eating an
       unlimited connected dirt layer. Check it from several ignition points. */
    static const int ignitionX[] = {24, 10, 39, 24};
    static const int ignitionY[] = {16, 9, 23, 9};
    int probe;

    for (probe = 0; probe < 4; ++probe) {
        World world;
        int dirtBefore;
        int dirtAfter;

        CHECK(WorldInit(&world, 48, 32), "world allocation failed");
        FillRect(&world, 8, 8, 40, 24, MATERIAL_DIRT);
        dirtBefore = CountMaterial(&world, MATERIAL_DIRT);
        WorldSetCell(&world, ignitionX[probe], ignitionY[probe], MATERIAL_FIRE);
        Tick(&world, 400);
        dirtAfter = CountMaterial(&world, MATERIAL_DIRT);

        CHECK(dirtAfter > dirtBefore / 2,
              "ignition %d burned %d of %d dirt cells", probe,
              dirtBefore - dirtAfter, dirtBefore);
        CHECK(CountMaterial(&world, MATERIAL_FIRE) == 0,
              "ignition %d still burning after 400 ticks", probe);
        WorldUnload(&world);
    }
}

static void test_a_lava_pocket_cannot_consume_its_rock_lining(void)
{
    World world;
    int lavaBefore;
    int rockBefore;

    CHECK(WorldInit(&world, 128, 128), "world allocation failed");
    FillRect(&world, 0, 0, 127, 127, MATERIAL_ROCK);
    FillRect(&world, 40, 50, 79, 69, MATERIAL_LAVA);
    lavaBefore = CountMaterial(&world, MATERIAL_LAVA);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);

    /* Lava heats what it touches, but rock must asymptote below its melt point.
       Without that budget a single pocket turns the whole map to lava, exactly
       as an unbudgeted fire would burn every connected dirt cell. */
    Tick(&world, 3000);

    CHECK(CountMaterial(&world, MATERIAL_LAVA) == lavaBefore,
          "lava grew from %d to %d cells", lavaBefore,
          CountMaterial(&world, MATERIAL_LAVA));
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == rockBefore,
          "lava melted %d rock cells",
          rockBefore - CountMaterial(&world, MATERIAL_ROCK));
    WorldUnload(&world);
}

static void test_lava_still_ignites_dirt_it_touches(void)
{
    World world;
    bool burned = false;
    int tick;

    /* The melt budget must not make lava thermally inert: dirt ignites far
       below rock's threshold and still has to catch. */
    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_DIRT);
    FillRect(&world, 28, 28, 35, 35, MATERIAL_LAVA);

    for (tick = 0; tick < 600 && !burned; ++tick) {
        WorldUpdate(&world);
        burned = CountMaterial(&world, MATERIAL_FIRE) > 0 ||
                 CountMaterial(&world, MATERIAL_DIRT) < 64 * 64 - 64;
    }
    CHECK(burned, "lava never ignited the dirt around it");
    WorldUnload(&world);
}

static void test_settled_cells_sleep_but_wake_when_disturbed(void)
{
    World world;
    int settledChunks;

    CHECK(WorldInit(&world, 128, 128), "world allocation failed");
    FillRect(&world, 0, 100, 127, 127, MATERIAL_ROCK);
    FillRect(&world, 40, 90, 60, 99, MATERIAL_SAND);
    Tick(&world, 400);
    settledChunks = world.activeChunkCount;

    /* A pile that cannot move is not work: it must let its chunks sleep. */
    CHECK(settledChunks == 0, "settled sand kept %d chunks awake", settledChunks);

    /* ...but pulling the floor out from under it has to wake it again. */
    FillRect(&world, 40, 100, 60, 127, MATERIAL_EMPTY);
    WorldUpdate(&world);
    CHECK(world.activeChunkCount > 0, "removing the floor woke nothing");
    Tick(&world, 200);
    CHECK(WorldGetCell(&world, 50, 127) == MATERIAL_SAND,
          "sand did not fall into the space that opened below it");
    WorldUnload(&world);
}

/* --- drilling ---------------------------------------------------------- */

static void test_drill_removes_solids_and_leaves_liquids(void)
{
    World world;
    int waterBefore;
    int lavaBefore;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_ROCK);
    FillRect(&world, 28, 28, 35, 35, MATERIAL_WATER);
    FillRect(&world, 28, 36, 35, 40, MATERIAL_LAVA);
    waterBefore = CountMaterial(&world, MATERIAL_WATER);
    lavaBefore = CountMaterial(&world, MATERIAL_LAVA);

    CHECK(WorldDrillCircle(&world, 32, 32, 6) > 0, "drill removed nothing");
    CHECK(CountMaterial(&world, MATERIAL_WATER) == waterBefore,
          "drill destroyed %d water cells",
          waterBefore - CountMaterial(&world, MATERIAL_WATER));
    CHECK(CountMaterial(&world, MATERIAL_LAVA) == lavaBefore,
          "drill destroyed %d lava cells",
          lavaBefore - CountMaterial(&world, MATERIAL_LAVA));
    WorldUnload(&world);
}

static void test_drill_returns_the_number_of_cells_it_removed(void)
{
    World world;
    int solidBefore;
    int removed;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_ROCK);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK);
    removed = WorldDrillCircle(&world, 32, 32, 5);

    /* A fraction of the cut becomes ash instead of empty, so the removed count
       must match rock lost, not the empty cells created. */
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == solidBefore - removed,
          "removed %d but rock fell by %d", removed,
          solidBefore - CountMaterial(&world, MATERIAL_ROCK));
    CHECK(WorldDrillCircle(&world, 32, 32, 5) == 0,
          "drilling the same hole twice removed cells again");
    WorldUnload(&world);
}

static void test_drill_cannot_breach_the_world_boundary(void)
{
    World world;
    int x;
    int y;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    FillRect(&world, 0, 0, 31, 31, MATERIAL_ROCK);
    /* Centred outside the grid, and straddling every edge. */
    WorldDrillCircle(&world, -8, 16, 6);
    WorldDrillCircle(&world, 40, 16, 6);
    WorldDrillCircle(&world, 16, -8, 6);
    WorldDrillCircle(&world, 16, 40, 6);
    WorldDrillCircle(&world, 0, 0, 4);

    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; ++x) {
            CHECK(WorldGetCell(&world, x, y) != MATERIAL_EMPTY ||
                      (x < 6 && y < 6),
                  "cell %d,%d was cleared from outside the grid", x, y);
        }
    }
    WorldUnload(&world);
}

static void test_drill_heat_cannot_ignite_dirt_or_boil_water(void)
{
    World world;
    int pass;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_DIRT);
    FillRect(&world, 40, 20, 47, 44, MATERIAL_WATER);

    /* Repeated passes must not accumulate heat past the phase thresholds. */
    for (pass = 0; pass < 40; ++pass) {
        WorldDrillCircle(&world, 20 + pass % 8, 32, 5);
        WorldUpdate(&world);
    }
    Tick(&world, 60);

    CHECK(CountMaterial(&world, MATERIAL_FIRE) == 0,
          "drilling ignited %d dirt cells", CountMaterial(&world, MATERIAL_FIRE));
    CHECK(CountMaterial(&world, MATERIAL_STEAM) == 0,
          "drilling boiled %d water cells", CountMaterial(&world, MATERIAL_STEAM));
    WorldUnload(&world);
}

/* --- chunk scheduling -------------------------------------------------- */

static void test_a_settled_world_lets_chunks_sleep(void)
{
    World world;
    int totalChunks;

    CHECK(WorldInit(&world, 256, 256), "world allocation failed");
    FillRect(&world, 0, 200, 255, 255, MATERIAL_ROCK);
    Tick(&world, 120);
    totalChunks = world.chunkColumns * world.chunkRows;

    CHECK(world.activeChunkCount < totalChunks,
          "all %d chunks stayed awake in a static world", totalChunks);
    CHECK(WorldCountDynamicCells(&world) == 0,
          "static rock counted as %d dynamic cells",
          WorldCountDynamicCells(&world));
    WorldUnload(&world);
}

static void test_activity_wakes_only_a_local_neighbourhood(void)
{
    World world;

    CHECK(WorldInit(&world, 256, 256), "world allocation failed");
    Tick(&world, 8);
    CHECK(world.activeChunkCount == 0, "empty world kept %d chunks awake",
          world.activeChunkCount);

    WorldSetCell(&world, 128, 128, MATERIAL_SAND);
    WorldUpdate(&world);
    CHECK(world.activeChunkCount > 0, "a falling grain woke no chunk");
    CHECK(world.activeChunkCount <= 9,
          "one grain woke %d chunks", world.activeChunkCount);
    WorldUnload(&world);
}

static void test_tick_stats_report_structural_work(void)
{
    World world;

    CHECK(WorldInit(&world, 70, 50), "world allocation failed");
    WorldUpdate(&world);
    CHECK(world.lastTickStats.processedChunks == 0u,
          "empty world reported %u processed chunks",
          world.lastTickStats.processedChunks);
    CHECK(world.lastTickStats.processedCells == 0u,
          "empty world reported %llu processed cells",
          (unsigned long long)world.lastTickStats.processedCells);

    /* This cell is inside one full 32x32 chunk, away from every border. The
       counter describes cells visited by the scheduler, not cells that moved. */
    WorldSetCell(&world, 40, 10, MATERIAL_SAND);
    WorldUpdate(&world);
    CHECK(world.lastTickStats.processedChunks == 1u,
          "one local mutation processed %u chunks",
          world.lastTickStats.processedChunks);
    CHECK(world.lastTickStats.processedCells == 1024u,
          "one full chunk reported %llu processed cells",
          (unsigned long long)world.lastTickStats.processedCells);
    WorldUnload(&world);
}

static void test_generation_streams_only_requested_dynamic_regions(void)
{
    World world;
    int totalChunks;
    int firstRegionChunks;
    int chunkY;

    CHECK(WorldInit(&world, 512, 288), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);
    totalChunks = world.chunkColumns * world.chunkRows;

    /* Generated terrain has not interacted yet, so a huge map begins asleep.
       Streaming a region wakes only the chunks in it that actually contain
       sand, fluids, gases or heat. Actual public writes use a separate wake path. */
    CHECK(world.activeChunkCount == 0,
          "generation woke %d distant chunks before a region was requested",
          world.activeChunkCount);
    WorldActivateRegion(&world,
                        (Rectangle){0.0f, 0.0f, (float)world.width * 0.5f,
                                    (float)world.height});
    CHECK(world.activeChunkCount > 0,
          "streaming the first region woke no sand or fluids");
    firstRegionChunks = world.activeChunkCount;
    for (chunkY = 0; chunkY < world.chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = world.chunkColumns / 2; chunkX < world.chunkColumns;
             ++chunkX) {
            size_t index = (size_t)chunkY * (size_t)world.chunkColumns +
                           (size_t)chunkX;

            CHECK(world.activeChunks[index] == 0u,
                  "streaming the left half woke distant chunk %d,%d",
                  chunkX, chunkY);
        }
    }
    {
        int farX = world.width - 2;
        int farY = 2;
        size_t farIndex = (size_t)(farY / WORLD_CHUNK_SIZE) *
                              (size_t)world.chunkColumns +
                          (size_t)(farX / WORLD_CHUNK_SIZE);

        WorldSetCell(&world, farX, farY, MATERIAL_SAND);
        CHECK(world.activeChunks[farIndex] != 0u,
              "a real mutation outside the streamed region stayed asleep");
    }
    WorldActivateRegion(&world,
                        (Rectangle){0.0f, 0.0f, (float)world.width,
                                    (float)world.height});
    CHECK(world.activeChunkCount > firstRegionChunks,
          "streaming the second half found no additional dynamic chunks");
    CHECK(world.activeChunkCount < totalChunks / 2,
          "streaming woke %d of %d chunks instead of only dynamic regions",
          world.activeChunkCount, totalChunks);
    WorldUnload(&world);
}

/* --- player against the world ------------------------------------------ */

static void test_boosting_player_tunnels_through_rock(void)
{
    World world;
    Player player;
    int step;
    int drilled = 0;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 60, 40, 200, 90, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    for (step = 0; step < 120; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        drilled += player.drilledCells;
    }

    CHECK(drilled > 0, "the player never cut a cell");
    CHECK(player.position.x > 200.0f,
          "the player stalled at x=%.1f inside the rock", player.position.x);
    WorldUnload(&world);
}

static void test_boost_from_rest_bores_into_a_wall(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 128, 200), "world allocation failed");
    /* The player starts resting on solid ground, the way a spawn does, and
       boosts straight down into it. */
    FillRect(&world, 0, 40, 127, 199, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){64.0f, 32.0f});

    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){0.0f, 1.0f}, true, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);
    }

    /* Below the drill threshold the collision zeroes the blocked velocity every
       frame, so without a contact drill the speed can never climb to the
       threshold and the boost stalls on the surface forever. */
    CHECK(player.position.y > 120.0f,
          "the player never broke the surface, stopping at y=%.1f",
          player.position.y);
    WorldUnload(&world);
}

static void test_boosting_player_tunnels_through_sand(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 60, 40, 200, 90, MATERIAL_SAND);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    /* Sand is dynamic, so this has to run a whole frame the way main.c does:
       the drill cuts, the world settles into the fresh tunnel, and only then is
       embedding resolved. Driving PlayerUpdate alone would never see it. */
    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);
    }

    CHECK(player.position.x > 200.0f,
          "the player stalled at x=%.1f inside the sand", player.position.x);
    WorldUnload(&world);
}

static int CountActiveParticles(const ParticleSystem *particles)
{
    int i;
    int count = 0;

    for (i = 0; i < MAX_PARTICLES; ++i) {
        if (particles->particles[i].active) {
            ++count;
        }
    }
    return count;
}

static void test_cryo_does_not_destroy_solid_terrain(void)
{
    World world;
    int step;
    int solidBefore;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 0, 30, 95, 40, MATERIAL_ROCK);
    FillRect(&world, 0, 41, 95, 48, MATERIAL_DIRT);
    FillRect(&world, 0, 49, 95, 56, MATERIAL_SAND);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK) +
                  CountMaterial(&world, MATERIAL_DIRT) +
                  CountMaterial(&world, MATERIAL_SAND);

    /* Cold is not a solvent. Rock, dirt and sand have no cooling transition, so
       chilling them far below ambient must do nothing but make them cold.
       Encoding "no transition" as a zero-filled field once made every one of
       them evaporate at 0C, and the cryo beam bored through the map. */
    for (step = 0; step < 240; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 5.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }

    CHECK(CountMaterial(&world, MATERIAL_ROCK) +
                  CountMaterial(&world, MATERIAL_DIRT) +
                  CountMaterial(&world, MATERIAL_SAND) ==
              solidBefore,
          "the cryo beam destroyed terrain: %d solid cells became %d", solidBefore,
          CountMaterial(&world, MATERIAL_ROCK) + CountMaterial(&world, MATERIAL_DIRT) +
              CountMaterial(&world, MATERIAL_SAND));
    CHECK(WorldGetTemperature(&world, 48, 31) < 0.0f,
          "the beam did not even chill the rock face it stopped against (%.1f C)",
          WorldGetTemperature(&world, 48, 31));
    WorldUnload(&world);
}

static int CountFilled(const World *world)
{
    int x;
    int y;
    int filled = 0;

    for (y = 0; y < world->height; ++y) {
        for (x = 0; x < world->width; ++x) {
            if (WorldGetCell(world, x, y) != MATERIAL_EMPTY) {
                ++filled;
            }
        }
    }
    return filled;
}

static void test_no_material_evaporates_when_merely_cooled(void)
{
    CellMaterial material;

    /* The whole-table version of the test above, and the one that catches the
       next material added without a deliberate cooling entry. Chilling may turn
       a material into a different one — water into ice, lava into rock, fire
       into smoke — but it must never make matter disappear. A few ticks only,
       so the lifetimes of fire, smoke and steam cannot confuse the count. */
    for (material = 0; material < MATERIAL_COUNT; ++material) {
        World world;
        int x;
        int y;
        int before;

        if (material == MATERIAL_EMPTY) {
            continue;
        }
        CHECK(WorldInit(&world, 32, 40), "world allocation failed");
        for (y = 10; y < 20; ++y) {
            for (x = 10; x < 20; ++x) {
                WorldSetCell(&world, x, y, material);
                WorldSetTemperature(&world, x, y, -260.0f);
            }
        }
        before = CountFilled(&world);
        Tick(&world, 4);
        CHECK(CountFilled(&world) == before,
              "%s lost cells when chilled: %d filled became %d",
              WorldMaterialName(material), before, CountFilled(&world));
        WorldUnload(&world);
    }
}

static void test_cryo_snuffs_fire_into_smoke(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 24, 24, 40, 30, MATERIAL_FIRE);

    for (step = 0; step < 60; ++step) {
        WorldApplyChill(&world, (Vector2){32.0f, 5.0f}, (Vector2){32.0f, 44.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_SMOKE) > 0,
          "chilled fire left no smoke behind");
    WorldUnload(&world);
}

static void test_cryo_freezes_water_into_standing_ice(void)
{
    World world;
    int step;
    int ice;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 20, 40, 76, 50, MATERIAL_WATER);

    for (step = 0; step < 60; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 20.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    ice = CountMaterial(&world, MATERIAL_ICE);
    CHECK(ice > 0, "the cryo beam froze nothing");
    CHECK(WorldMaterialIsSolid(MATERIAL_ICE), "ice must be solid to build with");

    /* Ice does not drift back on its own, so it is something the player can
       actually build with. */
    Tick(&world, 600);
    CHECK(CountMaterial(&world, MATERIAL_ICE) == ice,
          "undisturbed ice changed on its own: %d cells became %d", ice,
          CountMaterial(&world, MATERIAL_ICE));
    WorldUnload(&world);
}

static void test_heat_melts_ice_back_into_water(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 30, 40, 60, 46, MATERIAL_ICE);

    /* Stable is not permanent: anything warm has to undo it, or the player
       could seal the world shut with something nothing can remove. */
    for (step = 0; step < 60; ++step) {
        WorldApplyLaser(&world, (Vector2){45.0f, 20.0f}, (Vector2){45.0f, 60.0f},
                        2.25f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_WATER) > 0,
          "the laser never melted any ice");
    WorldUnload(&world);
}

static void test_cryo_settles_lava_back_into_rock(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 30, 40, 66, 50, MATERIAL_LAVA);

    /* Lava relaxes back toward 900C every tick, so this only passes if the beam
       out-cools that relaxation rather than merely dipping the temperature. */
    for (step = 0; step < 120; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 20.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_ROCK) > 0,
          "sustained cryo never turned any lava into rock");
    WorldUnload(&world);
}

static void test_one_force_blast_throws_loose_material_far(void)
{
    World world;
    int before;
    int rightmost = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 160, 64), "world allocation failed");
    FillRect(&world, 30, 30, 40, 40, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);

    /* A single blow, not a stream: the whole point is that one press moves the
       world a long way. */
    WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                         62.0f, 0.82f, 34);

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND && x > rightmost) {
                rightmost = x;
            }
        }
    }

    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "the blast destroyed loose material: %d sand cells became %d", before,
          CountMaterial(&world, MATERIAL_SAND));
    CHECK(rightmost > 55, "one blast barely moved anything; rightmost sand at x=%d",
          rightmost);
    WorldUnload(&world);
}

static void test_configured_force_power_hits_far_and_hard(void)
{
    World world;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events = {0};
    bool requested[ABILITY_COUNT] = {false};
    const AbilityState *force;
    const GameEvent *forceEvent = NULL;
    int rightmost = 0;
    int activeParticles = 0;
    int before;
    uint16_t index;
    int x;
    int y;
    int i;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    FillRect(&world, 30, 30, 40, 40, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);
    AbilitiesInit(&abilities, 0xE6BEu);
    ParticlesInit(&particles, 0xE6BEu);

    requested[ABILITY_FORCE] = true;
    AbilitiesUpdate(&abilities, &world, &particles, &events,
                    (Vector2){20.0f, 35.0f}, (Vector2){150.0f, 35.0f},
                    1.0f / 60.0f, requested);
    force = AbilityStateAt(&abilities, ABILITY_FORCE);

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND && x > rightmost) {
                rightmost = x;
            }
        }
    }
    for (i = 0; i < MAX_PARTICLES; ++i) {
        if (particles.particles[i].active) {
            ++activeParticles;
        }
    }
    for (index = 0u; index < events.count; ++index) {
        if (events.events[index].type == GAME_EVENT_FORCE) {
            forceEvent = &events.events[index];
        }
    }

    CHECK(force->triggered, "Q press did not fire the force ability");
    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "configured force destroyed loose cells");
    CHECK(rightmost > 76,
          "configured Q still reads as a weak shove; rightmost sand at x=%d",
          rightmost);
    CHECK(activeParticles >= 48,
          "configured Q spawned only %d burst particles", activeParticles);
    CHECK(forceEvent != NULL, "the force ability published no game event");
    /* The recoil now travels as the event's player impulse, so the player
       module never needs to know the force blast exists. */
    CHECK(forceEvent->playerImpulse.x <= -120.0f,
          "configured recoil is too small or points the wrong way: %.1f",
          forceEvent->playerImpulse.x);
    WorldUnload(&world);
}

static void test_force_blast_marks_solid_terrain_without_boring_through_it(void)
{
    World world;
    int before;
    int step;
    int remaining;
    int interior = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 30, 20, 60, 50, MATERIAL_ROCK);
    before = CountMaterial(&world, MATERIAL_ROCK);

    for (step = 0; step < 20; ++step) {
        WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                             62.0f, 0.82f, 34);
        WorldUpdate(&world);
    }

    remaining = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(remaining < before, "the blast left no mark on the rock face at all");
    /* A dent, not a tunnel: the wall must still be a wall after twenty blows. */
    CHECK(remaining > before * 4 / 5,
          "the blast bored through solid terrain: %d rock cells became %d", before,
          remaining);

    /* Only the exposed face is scoured; the inside of the block is untouched. */
    for (y = 25; y <= 45; ++y) {
        for (x = 40; x <= 58; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_ROCK) {
                ++interior;
            }
        }
    }
    CHECK(interior == 21 * 19, "the blast hollowed out rock behind the surface");
    WorldUnload(&world);
}

static void test_force_blast_does_not_reach_behind_a_wall(void)
{
    World world;
    int step;
    int x;
    int y;
    int moved = 0;

    CHECK(WorldInit(&world, 160, 64), "world allocation failed");
    /* A wall across the cone, with loose sand sheltering behind it. */
    FillRect(&world, 50, 0, 54, 63, MATERIAL_ROCK);
    FillRect(&world, 70, 30, 80, 40, MATERIAL_SAND);

    for (step = 0; step < 10; ++step) {
        WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                             62.0f, 0.82f, 34);
    }

    /* No settling ticks: any sand outside its original block can only have been
       thrown, and the wall should have absorbed the whole blow. */
    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND &&
                (x < 70 || x > 80 || y < 30 || y > 40)) {
                ++moved;
            }
        }
    }
    CHECK(moved == 0, "%d sheltered cells were thrown through solid rock", moved);
    WorldUnload(&world);
}

static void test_bouncing_particles_do_not_pass_through_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int inside = 0;
    int i;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 70, 0, 127, 63, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    /* Impact sparks fired straight at a rock wall from open air. */
    ParticlesSpawnImpact(&particles, (Vector2){50.0f, 32.0f},
                         (Vector2){1.0f, 0.0f}, 90.0f);
    CHECK(CountActiveParticles(&particles) > 0, "no particles were spawned");

    for (step = 0; step < 60; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
        for (i = 0; i < MAX_PARTICLES; ++i) {
            const Particle *particle = &particles.particles[i];

            if (particle->active && particle->position.x >= 71.0f) {
                ++inside;
            }
        }
    }

    CHECK(inside == 0, "%d particle samples ended up inside the rock", inside);
    WorldUnload(&world);
}

static void test_drill_debris_settles_as_ash_without_overwriting_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int rockBefore;
    int rockAfter;
    int ash;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 0, 40, 127, 63, MATERIAL_ROCK);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    for (step = 0; step < 40; ++step) {
        ParticlesSpawnDrillDebris(&particles, (Vector2){64.0f, 30.0f},
                                  (Vector2){0.0f, -120.0f}, 30);
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }
    for (step = 0; step < 120; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }

    ash = CountMaterial(&world, MATERIAL_ASH);
    rockAfter = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(ash > 0, "debris never settled into the world");
    CHECK(rockAfter == rockBefore,
          "settling debris overwrote terrain: %d rock cells became %d",
          rockBefore, rockAfter);
    WorldUnload(&world);
}

static void test_passing_particles_ignore_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int i;
    bool crossed = false;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 0, 20, 127, 24, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    /* Steam is a gas effect: a rock ceiling must not stop it, or reaction plumes
       would pile up against the lid of a pocket instead of drifting. */
    ParticlesSpawnSteam(&particles, (Vector2){64.0f, 40.0f});
    for (step = 0; step < 120 && !crossed; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
        for (i = 0; i < MAX_PARTICLES; ++i) {
            const Particle *particle = &particles.particles[i];

            if (particle->active && particle->position.y < 20.0f) {
                crossed = true;
            }
        }
    }

    CHECK(crossed, "steam never rose past the rock ceiling");
    WorldUnload(&world);
}

static void test_drill_resistance_never_stalls_the_boost(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 40, 20, 255, 110, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    for (step = 0; step < 200; ++step) {
        float speed;

        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        speed = sqrtf(player.velocity.x * player.velocity.x +
                      player.velocity.y * player.velocity.y);
        /* Once inside solid terrain the drill must keep the player above its
           own threshold, otherwise a boost would bury itself. */
        if (player.position.x > 60.0f && player.position.x < 240.0f) {
            CHECK(speed >= player.drillSpeed,
                  "speed fell to %.1f at x=%.1f, below the %.1f drill threshold",
                  speed, player.position.x, player.drillSpeed);
        }
    }
    WorldUnload(&world);
}

static void test_normal_flight_keeps_a_hover_pose(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){48.0f, 64.0f});

    for (frame = 0; frame < 180; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, false,
                     1.0f / 120.0f);
    }

    CHECK(!player.boosting, "normal-flight test unexpectedly enabled boost");
    CHECK(player.velocity.x > player.maxSpeed * 0.95f,
          "normal flight never reached cruise speed: %.1f", player.velocity.x);
    CHECK(fabsf(player.leanAmount - 0.12f) < 0.02f,
          "normal flight stopped reading as a slight hover lean: %.3f",
          player.leanAmount);
    WorldUnload(&world);
}

static void test_boost_flight_reaches_a_head_first_pose(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){48.0f, 64.0f});

    for (frame = 0; frame < 180; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
    }

    CHECK(player.boosting, "boost-pose test never enabled boost");
    CHECK(player.leanAmount > 0.9f,
          "boost never reached its head-first pose: %.3f", player.leanAmount);
    WorldUnload(&world);
}

static void test_long_boost_climbs_three_stages_into_supersonic(void)
{
    World world;
    Player player;
    int expectedStage = 1;
    int transitions = 0;
    int stageThreeFrame = -1;
    int sonicFrame = -1;
    int frame;

    CHECK(WorldInit(&world, 4096, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){128.0f, 64.0f});

    for (frame = 0; frame < 960 && sonicFrame < 0; ++frame) {
        float before = sqrtf(player.velocity.x * player.velocity.x +
                             player.velocity.y * player.velocity.y);
        float after;

        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
        after = sqrtf(player.velocity.x * player.velocity.x +
                      player.velocity.y * player.velocity.y);
        if (player.boostStageChanged != PLAYER_BOOST_NONE) {
            CHECK((int)player.boostStageChanged == expectedStage,
                  "boost skipped or reordered a stage: expected %d, got %d",
                  expectedStage, (int)player.boostStageChanged);
            if (player.boostStageChanged == PLAYER_BOOST_STAGE_TWO) {
                CHECK(after - before > 70.0f,
                      "stage II had no real kick: %.1f -> %.1f", before, after);
            } else if (player.boostStageChanged == PLAYER_BOOST_STAGE_THREE) {
                CHECK(after - before > 150.0f,
                      "stage III had no real kick: %.1f -> %.1f", before, after);
                stageThreeFrame = frame;
            }
            ++expectedStage;
            ++transitions;
        }
        if (player.boostStage == PLAYER_BOOST_STAGE_THREE &&
            after >= player.sonicSpeed) {
            sonicFrame = frame;
        }
    }

    CHECK(transitions == 3, "long boost emitted %d stage kicks instead of 3",
          transitions);
    CHECK(player.boostStage == PLAYER_BOOST_STAGE_THREE,
          "long boost stopped at stage %d", (int)player.boostStage);
    CHECK(sonicFrame >= 0, "stage III never crossed sonic speed %.1f",
          player.sonicSpeed);
    CHECK(sonicFrame - stageThreeFrame <= 30,
          "stage III needed %.2fs to reach sonic speed instead of one kick",
          (float)(sonicFrame - stageThreeFrame) / 120.0f);

    PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, false, 1.0f / 120.0f);
    CHECK(player.boostStage == PLAYER_BOOST_NONE,
          "releasing Shift left boost stage %d armed", (int)player.boostStage);
    WorldUnload(&world);
}

static void test_stalled_boost_cannot_charge_a_later_stage(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    PlayerInit(&player, (Vector2){124.0f, 48.0f});

    /* The outside of the simulation is indestructible rock. Holding thrust into
       it exercises a long Shift press with neither sustained speed nor aligned
       travel; elapsed key time alone must never unlock Stage II. */
    for (frame = 0; frame < 600; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
    }

    CHECK(player.boostStage == PLAYER_BOOST_STAGE_ONE,
          "stalled boost charged stage %d at the world boundary",
          (int)player.boostStage);
    CHECK(player.boostStageTime < 0.01f,
          "stalled boost retained %.3fs of false stage progress",
          player.boostStageTime);
    WorldUnload(&world);
}

static void test_player_never_ends_a_frame_inside_solid_terrain(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 0, 70, 255, 127, MATERIAL_ROCK);
    FillRect(&world, 90, 20, 130, 70, MATERIAL_DIRT);
    PlayerInit(&player, (Vector2){20.0f, 40.0f});

    for (step = 0; step < 400; ++step) {
        /* Sweep the input around so the player grinds along walls and corners
           rather than following one clean line. */
        float angle = (float)step * 0.11f;
        Vector2 input = {cosf(angle), sinf(angle)};
        int radius;
        int checkedX;
        int checkedY;
        bool embedded = false;

        PlayerUpdate(&player, &world, input, (step % 3) != 0, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);

        radius = (int)player.radius;
        for (checkedY = -radius; checkedY <= radius && !embedded; ++checkedY) {
            for (checkedX = -radius; checkedX <= radius; ++checkedX) {
                int cellX = (int)player.position.x + checkedX;
                int cellY = (int)player.position.y + checkedY;

                if (checkedX * checkedX + checkedY * checkedY > radius * radius) {
                    continue;
                }
                if (WorldMaterialIsSolid(WorldGetCell(&world, cellX, cellY))) {
                    embedded = true;
                    break;
                }
            }
        }
        CHECK(!embedded, "player embedded in solid terrain at %.1f,%.1f on step %d",
              player.position.x, player.position.y, step);
    }
    WorldUnload(&world);
}

int main(void)
{
    RUN(test_world_render_preparation_is_headless_and_incremental);
    RUN(test_a_refused_chunk_keeps_its_dirty_flag);
    RUN(test_emissive_render_data_selects_emitters_not_bright_terrain);
    RUN(test_particle_emission_is_explicit_per_effect);
    RUN(test_game_event_buffer_is_fixed_and_ordered);
    RUN(test_game_update_publishes_transient_events);
    RUN(test_the_same_seed_always_generates_the_same_world);
    RUN(test_regenerating_one_world_from_a_seed_reproduces_it);
    RUN(test_world_effects_cannot_shift_the_terrain_a_seed_produces);
    RUN(test_a_seeded_session_replays_identically);
    RUN(test_the_tick_counter_survives_wrapping_its_cell_stamp);
    RUN(test_the_schedule_never_lists_a_chunk_twice);
    RUN(test_visual_particles_never_change_the_world);
    RUN(test_a_fresh_manager_holds_no_bodies);
    RUN(test_a_zero_initialised_handle_names_nothing);
    RUN(test_allocation_returns_a_usable_body);
    RUN(test_the_manager_refuses_work_past_its_budgets);
    RUN(test_a_freed_slot_is_reused);
    RUN(test_a_stale_handle_never_resolves_to_the_new_body);
    RUN(test_reset_releases_every_body);
    RUN(test_a_body_preserves_the_material_and_heat_it_was_given);
    RUN(test_out_of_range_cell_access_is_safe);
    RUN(test_finalize_reports_the_occupied_bounds);
    RUN(test_mass_follows_material_density);
    RUN(test_centre_of_mass_lands_where_the_shape_says);
    RUN(test_inertia_grows_with_how_spread_out_a_body_is);
    RUN(test_the_body_manager_leaves_the_world_alone);
    RUN(test_regenerating_the_world_drops_its_detached_pieces);
    RUN(test_a_lone_island_is_reported_detached);
    RUN(test_two_islands_are_separate_components);
    RUN(test_a_liquid_gap_does_not_join_two_components);
    RUN(test_an_intact_bridge_to_the_ground_prevents_detachment);
    RUN(test_cutting_the_bridge_detaches_the_island);
    RUN(test_a_corner_contact_does_not_join_two_components);
    RUN(test_a_diagonal_only_join_to_the_ground_reads_as_detached);
    RUN(test_a_component_crossing_a_chunk_boundary_stays_whole);
    RUN(test_a_component_continuing_past_the_region_is_unknown);
    RUN(test_a_component_that_only_touches_the_region_edge_is_detached);
    RUN(test_a_component_touching_the_world_edge_is_anchored);
    RUN(test_a_component_larger_than_the_budget_is_too_large);
    RUN(test_an_oversized_or_malformed_query_is_refused);
    RUN(test_the_detector_never_changes_the_world);
    RUN(test_extraction_moves_an_island_out_of_the_world);
    RUN(test_extraction_conserves_materials_and_heat);
    RUN(test_extraction_places_the_body_where_the_island_was);
    RUN(test_extraction_wakes_and_dirties_the_chunks_it_emptied);
    RUN(test_extraction_crosses_a_chunk_boundary);
    RUN(test_an_anchored_component_is_never_extracted);
    RUN(test_an_unknown_component_is_never_extracted);
    RUN(test_a_component_the_detector_refused_is_never_extracted);
    RUN(test_extraction_without_a_free_body_slot_changes_nothing);
    RUN(test_a_component_too_wide_for_a_body_changes_nothing);
    RUN(test_a_malformed_component_changes_nothing);
    RUN(test_a_component_the_world_has_moved_past_changes_nothing);
    RUN(test_reset_after_extraction_returns_the_store_to_empty);
    RUN(test_ability_table_passes_its_own_validation);
    RUN(test_a_one_shot_ability_respects_its_cooldown);
    RUN(test_a_held_ability_reports_one_start_per_hold);
    RUN(test_ability_knockback_reaches_the_player_through_events);
    RUN(test_every_material_has_table_data);
    RUN(test_material_table_passes_its_own_validation);
    RUN(test_every_material_carries_its_own_cryo_rate);
    RUN(test_solidity_matches_what_blocks_the_player);
    RUN(test_sand_falls_to_the_floor_and_is_conserved);
    RUN(test_sand_falls_at_most_one_cell_per_tick);
    RUN(test_water_spreads_sideways_and_is_conserved);
    RUN(test_rock_becomes_lava_above_its_threshold);
    RUN(test_water_becomes_steam_above_its_threshold);
    RUN(test_water_and_lava_react_into_steam_and_rock);
    RUN(test_one_fire_cell_cannot_consume_a_whole_dirt_field);
    RUN(test_a_lava_pocket_cannot_consume_its_rock_lining);
    RUN(test_lava_still_ignites_dirt_it_touches);
    RUN(test_settled_cells_sleep_but_wake_when_disturbed);
    RUN(test_drill_removes_solids_and_leaves_liquids);
    RUN(test_drill_returns_the_number_of_cells_it_removed);
    RUN(test_drill_cannot_breach_the_world_boundary);
    RUN(test_drill_heat_cannot_ignite_dirt_or_boil_water);
    RUN(test_a_settled_world_lets_chunks_sleep);
    RUN(test_activity_wakes_only_a_local_neighbourhood);
    RUN(test_tick_stats_report_structural_work);
    RUN(test_generation_streams_only_requested_dynamic_regions);
    RUN(test_boosting_player_tunnels_through_rock);
    RUN(test_boost_from_rest_bores_into_a_wall);
    RUN(test_boosting_player_tunnels_through_sand);
    RUN(test_drill_resistance_never_stalls_the_boost);
    RUN(test_normal_flight_keeps_a_hover_pose);
    RUN(test_boost_flight_reaches_a_head_first_pose);
    RUN(test_long_boost_climbs_three_stages_into_supersonic);
    RUN(test_stalled_boost_cannot_charge_a_later_stage);
    RUN(test_cryo_does_not_destroy_solid_terrain);
    RUN(test_no_material_evaporates_when_merely_cooled);
    RUN(test_cryo_snuffs_fire_into_smoke);
    RUN(test_cryo_freezes_water_into_standing_ice);
    RUN(test_heat_melts_ice_back_into_water);
    RUN(test_cryo_settles_lava_back_into_rock);
    RUN(test_one_force_blast_throws_loose_material_far);
    RUN(test_configured_force_power_hits_far_and_hard);
    RUN(test_force_blast_marks_solid_terrain_without_boring_through_it);
    RUN(test_force_blast_does_not_reach_behind_a_wall);
    RUN(test_bouncing_particles_do_not_pass_through_terrain);
    RUN(test_drill_debris_settles_as_ash_without_overwriting_terrain);
    RUN(test_passing_particles_ignore_terrain);
    RUN(test_player_never_ends_a_frame_inside_solid_terrain);

    printf("\n%d tests, %d failed\n", testsRun, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
