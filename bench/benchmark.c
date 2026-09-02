#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <raylib.h>

#include "player.h"
#include "world.h"
#include "abilities.h"
#include "game.h"
#include "terrain_detach.h"
#include "terrain_impulse.h"
#include "terrain_physics.h"
#include "world_lighting.h"

#define BENCH_WORLD_WIDTH 16384
/* Tracks the production map. A benchmark measuring a world the game no longer
   ships is a benchmark of nothing: the height decides how much sky the light
   field has to fill and how deep the ground the simulation walks is. */
#define BENCH_WORLD_HEIGHT 1440
#define BENCH_DEFAULT_TICKS 180
#define BENCH_SEED 0x00e6be11u

typedef struct BenchContext {
    World *world;
    Player player;
    int centerX;
    int ticks;
} BenchContext;

typedef void (*ScenarioSetup)(BenchContext *context);
typedef void (*ScenarioStep)(BenchContext *context, int tick);

typedef struct Scenario {
    const char *name;
    ScenarioSetup setup;
    ScenarioStep step;
} Scenario;

static double NowSeconds(void)
{
    struct timespec time;

    (void)clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static int CompareDouble(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    return (a > b) - (a < b);
}

static double Percentile(const double *samples, int count, double percentile)
{
    double ordered[BENCH_DEFAULT_TICKS * 2];
    int index;

    if (count <= 0 || count > (int)(sizeof(ordered) / sizeof(ordered[0]))) {
        return 0.0;
    }
    memcpy(ordered, samples, (size_t)count * sizeof(*samples));
    qsort(ordered, (size_t)count, sizeof(*ordered), CompareDouble);
    index = (int)ceil(percentile * (double)count) - 1;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return ordered[index];
}

static size_t CurrentRssBytes(void)
{
    FILE *status = fopen("/proc/self/statm", "r");
    unsigned long virtualPages = 0ul;
    long residentPages = 0;
    long pageSize = sysconf(_SC_PAGESIZE);

    if (status == NULL || pageSize <= 0) {
        if (status != NULL) fclose(status);
        return 0u;
    }
    if (fscanf(status, "%lu %ld", &virtualPages, &residentPages) != 2) {
        residentPages = 0;
    }
    (void)virtualPages;
    fclose(status);
    return residentPages > 0 ? (size_t)residentPages * (size_t)pageSize : 0u;
}

static size_t PeakRssBytes(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0u;
    }
    /* Linux reports ru_maxrss in KiB. Emberfall currently targets Linux. */
    return (size_t)usage.ru_maxrss * 1024u;
}

static size_t CountSetFlags(const uint8_t *flags, size_t count)
{
    size_t set = 0u;
    size_t index;

    for (index = 0u; index < count; ++index) {
        set += flags[index] != 0u ? 1u : 0u;
    }
    return set;
}

static void ResetWorkFlags(World *world)
{
    size_t chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;

    memset(world->activeChunks, 0, chunkCount * sizeof(*world->activeChunks));
    memset(world->nextActiveChunks, 0,
           chunkCount * sizeof(*world->nextActiveChunks));
    memset(world->dirtyChunks, 0, chunkCount * sizeof(*world->dirtyChunks));
    memset(world->lightDirtyChunks, 0,
           chunkCount * sizeof(*world->lightDirtyChunks));
    world->activeChunkCount = 0;
}

static void FillRectangle(World *world, int firstX, int firstY, int lastX,
                          int lastY, CellMaterial material)
{
    int y;

    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
            WorldSetCell(world, x, y, material);
        }
    }
}

static void PrepareScenario(BenchContext *context)
{
    WorldGenerate(context->world, BENCH_SEED);
    ResetWorkFlags(context->world);
}

static void PrepareArena(BenchContext *context)
{
    PrepareScenario(context);
    FillRectangle(context->world, context->centerX - 220, 190,
                  context->centerX + 720, 570, MATERIAL_EMPTY);
    FillRectangle(context->world, context->centerX - 220, 571,
                  context->centerX + 720, 578, MATERIAL_ROCK);
}

static void SetupSettled(BenchContext *context)
{
    PrepareScenario(context);
}

static void SetupSand(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX - 100, 215,
                  context->centerX + 100, 330, MATERIAL_SAND);
}

static void SetupWater(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX - 140, 220,
                  context->centerX + 140, 390, MATERIAL_WATER);
}

static void SetupFireLava(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX - 150, 350,
                  context->centerX + 180, 530, MATERIAL_DIRT);
    FillRectangle(context->world, context->centerX - 95, 285,
                  context->centerX + 95, 345, MATERIAL_LAVA);
    FillRectangle(context->world, context->centerX + 110, 320,
                  context->centerX + 140, 349, MATERIAL_FIRE);
}

static void SetupExplosion(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX - 100, 260,
                  context->centerX + 100, 500, MATERIAL_ROCK);
}

static void StepExplosion(BenchContext *context, int tick)
{
    if (tick == 0 || tick == context->ticks / 2) {
        WorldDestroyCircle(context->world, context->centerX, 360, 72, 0.38f);
        WorldApplyShockwave(context->world, context->centerX, 360, 72, 118);
    }
}

static void SetupDestruction(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX - 180, 220,
                  context->centerX + 600, 540, MATERIAL_DIRT);
}

static void StepDestruction(BenchContext *context, int tick)
{
    if (tick < 24) {
        int x = context->centerX - 150 + (tick % 8) * 92;
        int y = 250 + (tick / 8) * 96;

        WorldDestroyCircle(context->world, x, y, 38, 0.12f);
    }
}

static void SetupDrilling(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX, 270,
                  context->centerX + 700, 330, MATERIAL_ROCK);
    PlayerInit(&context->player,
               (Vector2){(float)context->centerX - 35.0f, 300.0f});
}

static void StepDrilling(BenchContext *context, int tick)
{
    (void)tick;
    PlayerUpdate(&context->player, context->world, (Vector2){1.0f, 0.0f}, true,
                 1.0f / 60.0f);
}

static void SetupForce(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX + 10, 255,
                  context->centerX + 125, 345, MATERIAL_SAND);
    FillRectangle(context->world, context->centerX + 30, 346,
                  context->centerX + 125, 430, MATERIAL_WATER);
}

static void StepForce(BenchContext *context, int tick)
{
    if (tick % 30 == 0) {
        WorldApplyForceBlast(context->world,
                             (Vector2){(float)context->centerX, 300.0f},
                             (Vector2){1.0f, 0.0f}, 84.0f, 0.78f, 54);
    }
}

static void SetupCryo(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX + 15, 260,
                  context->centerX + 180, 390, MATERIAL_WATER);
    FillRectangle(context->world, context->centerX + 181, 260,
                  context->centerX + 290, 390, MATERIAL_LAVA);
}

static void StepCryo(BenchContext *context, int tick)
{
    float verticalOffset = (float)(tick % 80) - 40.0f;

    (void)WorldApplyChill(context->world,
                          (Vector2){(float)context->centerX, 325.0f + verticalOffset},
                          (Vector2){(float)context->centerX + 320.0f,
                                    325.0f + verticalOffset},
                          2.6f, 1.0f / 60.0f);
}

static void SetupMixed(BenchContext *context)
{
    PrepareArena(context);
    FillRectangle(context->world, context->centerX - 170, 210,
                  context->centerX - 30, 340, MATERIAL_SAND);
    FillRectangle(context->world, context->centerX - 20, 220,
                  context->centerX + 150, 390, MATERIAL_WATER);
    FillRectangle(context->world, context->centerX + 151, 260,
                  context->centerX + 260, 430, MATERIAL_LAVA);
    FillRectangle(context->world, context->centerX + 270, 300,
                  context->centerX + 430, 500, MATERIAL_DIRT);
    FillRectangle(context->world, context->centerX + 360, 270,
                  context->centerX + 390, 299, MATERIAL_FIRE);
}

static void StepMixed(BenchContext *context, int tick)
{
    if (tick % 45 == 0) {
        int x = context->centerX + 300 + (tick / 45) * 24;

        WorldDestroyCircle(context->world, x, 380, 24, 0.2f);
        WorldApplyShockwave(context->world, x, 380, 24, 52);
    }
    if (tick % 30 == 0) {
        WorldApplyForceBlast(context->world,
                             (Vector2){(float)context->centerX - 190.0f, 300.0f},
                             (Vector2){1.0f, 0.0f}, 84.0f, 0.78f, 54);
    }
    (void)WorldApplyLaser(context->world,
                          (Vector2){(float)context->centerX + 250.0f, 360.0f},
                          (Vector2){(float)context->centerX + 470.0f, 420.0f},
                          2.25f, 1.0f / 60.0f);
}

static void RunScenario(BenchContext *context, const Scenario *scenario,
                        double *samples)
{
    uint64_t processedCells = 0u;
    uint64_t processedChunks = 0u;
    uint64_t dirtyRegions = 0u;
    double total = 0.0;
    size_t chunkCount = (size_t)context->world->chunkColumns *
                        (size_t)context->world->chunkRows;
    int tick;

    scenario->setup(context);
    for (tick = 0; tick < context->ticks; ++tick) {
        double start;

        /* Model a renderer that consumes every pending dirty region once per
           frame. The actual visible-page upload count is measured later by the
           renderer benchmark; this is the producer-side workload. */
        memset(context->world->dirtyChunks, 0,
               chunkCount * sizeof(*context->world->dirtyChunks));
        start = NowSeconds();
        if (scenario->step != NULL) {
            scenario->step(context, tick);
        }
        WorldUpdate(context->world);
        samples[tick] = (NowSeconds() - start) * 1000.0;
        total += samples[tick];
        processedCells += context->world->lastTickStats.processedCells;
        processedChunks += context->world->lastTickStats.processedChunks;
        dirtyRegions += CountSetFlags(context->world->dirtyChunks, chunkCount);
    }

    printf("%-18s avg=%7.3f ms  p50=%7.3f  p95=%7.3f  p99=%7.3f  "
           "active=%4d sleep=%5zu  cells/tick=%9" PRIu64
           " chunks/tick=%4" PRIu64 " dirty/tick=%4" PRIu64 "\n",
           scenario->name, total / (double)context->ticks,
           Percentile(samples, context->ticks, 0.50),
           Percentile(samples, context->ticks, 0.95),
           Percentile(samples, context->ticks, 0.99),
           context->world->activeChunkCount,
           chunkCount - (size_t)context->world->activeChunkCount,
           processedCells / (uint64_t)context->ticks,
           processedChunks / (uint64_t)context->ticks,
           dirtyRegions / (uint64_t)context->ticks);
}

/* Lighting is measured on its own because it is the one part of drawing that
   is not proportional to what changed, and because it runs per frame rather
   than per tick. No GL context is needed: WorldUpdateLighting works entirely on
   CPU fields. */
static void RunLightingBenchmark(BenchContext *context, double *samples)
{
    World *world = context->world;
    /* A 640x360 view, the widest the camera reaches at full boost. */
    Rectangle lightView = {(float)context->centerX - 320.0f, 100.0f, 640.0f, 360.0f};
    double still;
    double moving;
    double disturbed;
    double total;
    int frame;

    PrepareScenario(context);
    /* Warm: the first solve always runs, because nothing has been solved yet. */
    WorldSetPointLight(world, (Vector2){(float)context->centerX, 200.0f}, 60.0f,
                       0.8f);
    WorldUpdateLighting(world, lightView);

    total = 0.0;
    for (frame = 0; frame < context->ticks; ++frame) {
        double start = NowSeconds();

        WorldUpdateLighting(world, lightView);
        total += NowSeconds() - start;
    }
    still = total * 1000.0 / (double)context->ticks;

    /* A light moving at boost speed: about eight cells per frame, so it leaves
       its light cell — and forces a solve — on every single frame. */
    total = 0.0;
    for (frame = 0; frame < context->ticks; ++frame) {
        double start;

        WorldSetPointLight(world,
                           (Vector2){(float)context->centerX + (float)frame * 8.0f,
                                     200.0f},
                           60.0f, 0.8f);
        lightView.x = (float)context->centerX + (float)frame * 8.0f - 320.0f;
        start = NowSeconds();
        WorldUpdateLighting(world, lightView);
        samples[frame] = (NowSeconds() - start) * 1000.0;
        total += samples[frame];
    }
    /* samples[] is already in milliseconds here. */
    moving = total / (double)context->ticks;

    total = 0.0;
    for (frame = 0; frame < context->ticks; ++frame) {
        double start;

        WorldDrillCircle(world, context->centerX + frame, 300, 6);
        WorldUpdate(world);
        start = NowSeconds();
        WorldUpdateLighting(world, lightView);
        total += NowSeconds() - start;
    }
    disturbed = total * 1000.0 / (double)context->ticks;

    printf("lighting           still=%7.3f ms  moving_light=%7.3f ms  "
           "p95=%7.3f  digging=%7.3f ms  light_cells=%d\n",
           still, moving, Percentile(samples, context->ticks, 0.95), disturbed,
           world->lightColumns * world->lightRows);
}

/* Dynamic terrain has its own cost curve — bodies, not cells — so it is
   measured on its own rather than folded into a cellular scenario. Bodies are
   dropped onto the generated surface so they collide against real terrain
   rather than a flat test floor.

   The scenarios exist to answer the two questions the budgets are set from:
   what does a full world of moving rubble cost, and how much of that cost does
   sleeping take away. The sleeping rows carry exactly the same bodies as the
   awake ones — same count, same size, same place — so the difference between
   the two rows is the sleep check and nothing else. */
typedef struct {
    const char *name;
    int bodyCount;
    int bodyWidth;
    int bodyHeight;
    bool asleep;   /* settle every body before the first tick */
    int awakeBudget; /* 0 keeps the shipped default */
} TerrainBenchScenario;

static void RunDynamicTerrainBenchmark(BenchContext *context, double *samples,
                                       TerrainBenchScenario scenario)
{
    DynamicTerrainSystem terrain;
    World *world = context->world;
    double total = 0.0;
    int index;
    int frame;

    if (!DynamicTerrainInit(&terrain)) {
        printf("dynamic terrain      allocation failed\n");
        return;
    }
    if (scenario.awakeBudget > 0) {
        terrain.config.maxAwakeBodies = scenario.awakeBudget;
    }
    PrepareScenario(context);

    for (index = 0; index < scenario.bodyCount; ++index) {
        TerrainBodyHandle handle = DynamicTerrainAllocBody(&terrain,
                                                           scenario.bodyWidth,
                                                           scenario.bodyHeight);
        TerrainBody *body;
        int y;

        for (y = 0; y < scenario.bodyHeight; ++y) {
            int x;

            for (x = 0; x < scenario.bodyWidth; ++x) {
                DynamicTerrainSetCell(&terrain, handle, x, y, MATERIAL_ROCK, 20.0f);
            }
        }
        DynamicTerrainFinalizeBody(&terrain, handle);
        body = DynamicTerrainGet(&terrain, handle);
        if (body == NULL) {
            break;
        }
        body->position = (Vector2){(float)(context->centerX - 200 + index * 26),
                                   120.0f};
        if (scenario.asleep) {
            /* Settled in place, holding a slot and its cells but asking for no
               work. This is the state most rubble spends its life in. The quiet
               spell is fed in ordinary frame steps because the module refuses a
               step larger than a frame; the body has no velocity, so nothing
               resets the timer and the loop always terminates well inside its
               bound. */
            int quiet;

            for (quiet = 0; quiet < 64 && body->awake; ++quiet) {
                DynamicTerrainSettleBody(&terrain, body, 1.0f / 60.0f);
            }
        } else {
            body->velocity = (Vector2){12.0f, 0.0f};
            body->angularVelocity = 0.6f;
        }
    }

    for (frame = 0; frame < context->ticks; ++frame) {
        double start = NowSeconds();

        TerrainPhysicsUpdate(&terrain, world, 1.0f / 60.0f);
        samples[frame] = (NowSeconds() - start) * 1000.0;
        total += samples[frame];
    }

    printf("terrain %-18s avg=%7.3f ms  p50=%7.3f  p95=%7.3f  "
           "awake=%3d sleeping=%3d cells=%6d  contacts/tick=%5d "
           "substeps/tick=%4d max_contacts=%3d\n",
           scenario.name, total / (double)context->ticks,
           Percentile(samples, context->ticks, 0.50),
           Percentile(samples, context->ticks, 0.95),
           terrain.stats.awakeBodies, terrain.stats.sleepingBodies,
           terrain.stats.dynamicCellsUsed,
           terrain.stats.collisionContacts, terrain.stats.collisionSubsteps,
           terrain.stats.maxContactsObserved);
    DynamicTerrainUnload(&terrain);
}

/* Automatic detachment is event-driven, and the first row below is the claim
   that matters: a tick with no destruction in it must cost nothing measurable,
   because otherwise the feature would be a tax on every frame of a game that is
   mostly not blowing anything up.

   Only TerrainDetachProcess is timed. Building the scene and firing the blast
   happen outside the clock: they are the benchmark's setup, not the cost under
   test. */
typedef struct {
    const char *name;
    bool destroy;      /* fire a blast into the scene each frame */
    int fragments;     /* block-on-pillar assemblies to build */
    bool anchored;     /* build a large structure that cannot come loose */
    int blastY;
    int blastRadius;
} DetachBenchScenario;

#define DETACH_BENCH_GROUND 400
/* Pillars close enough together that one blast severs all of them, blocks
   narrow enough to stay separate components, and blocks high enough that the
   blast cuts the supports without touching what it is meant to set free. */
#define DETACH_BENCH_PILLAR_STEP 12
#define DETACH_BENCH_BLOCK_WIDTH 8
#define DETACH_BENCH_BLOCK_HEIGHT 12
#define DETACH_BENCH_BLOCK_BOTTOM (DETACH_BENCH_GROUND - 50)

static void BuildDetachScene(BenchContext *context,
                             const DetachBenchScenario *scenario)
{
    int centre = context->centerX;
    int index;

    FillRectangle(context->world, centre - 260, DETACH_BENCH_GROUND - 220,
                  centre + 260, DETACH_BENCH_GROUND + 40, MATERIAL_EMPTY);
    FillRectangle(context->world, centre - 260, DETACH_BENCH_GROUND,
                  centre + 260, DETACH_BENCH_GROUND + 40, MATERIAL_ROCK);

    if (scenario->anchored) {
        /* A slab welded to the ground on both sides: the worst case for a
           detector, because every seed on it explores until it gives up. */
        FillRectangle(context->world, centre - 60, DETACH_BENCH_GROUND - 60,
                      centre + 60, DETACH_BENCH_GROUND - 1, MATERIAL_ROCK);
    }
    for (index = 0; index < scenario->fragments; ++index) {
        int pillarX = centre - (scenario->fragments - 1) *
                                   DETACH_BENCH_PILLAR_STEP / 2 +
                      index * DETACH_BENCH_PILLAR_STEP;
        int blockBottom = DETACH_BENCH_BLOCK_BOTTOM;

        FillRectangle(context->world, pillarX, blockBottom + 1,
                      pillarX, DETACH_BENCH_GROUND - 1, MATERIAL_ROCK);
        FillRectangle(context->world, pillarX - DETACH_BENCH_BLOCK_WIDTH / 2,
                      blockBottom - DETACH_BENCH_BLOCK_HEIGHT + 1,
                      pillarX + DETACH_BENCH_BLOCK_WIDTH / 2, blockBottom,
                      MATERIAL_ROCK);
    }
}

static void RunDetachBenchmark(BenchContext *context, double *samples,
                               DetachBenchScenario scenario)
{
    DynamicTerrainSystem terrain;
    TerrainDetachSystem detach;
    World *world = context->world;
    double total = 0.0;
    int frame;

    if (!DynamicTerrainInit(&terrain)) {
        printf("detach               allocation failed\n");
        return;
    }
    TerrainDetachInit(&detach);
    PrepareScenario(context);

    for (frame = 0; frame < context->ticks; ++frame) {
        double start;
        int slot;

        if (scenario.destroy) {
            BuildDetachScene(context, &scenario);
            WorldDestroyCircle(world, context->centerX, scenario.blastY,
                               scenario.blastRadius, 0.0f);
        }

        start = NowSeconds();
        (void)TerrainDetachProcess(&detach, world, &terrain, NULL);
        samples[frame] = (NowSeconds() - start) * 1000.0;
        total += samples[frame];

        /* Bodies are freed outside the clock so every frame starts from the
           same budget and the row measures detection, not accumulation. */
        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            if (terrain.bodies[slot].active) {
                DynamicTerrainFreeBody(&terrain, (TerrainBodyHandle){
                    (uint16_t)slot, terrain.bodies[slot].generation});
            }
        }
    }

    printf("detach %-20s avg=%7.4f ms  p50=%7.4f  p95=%7.4f  "
           "regions=%5d checks=%6d explored=%8d detached=%5d cells=%7d  "
           "anchored=%5d unknown=%5d small=%5d large=%5d\n",
           scenario.name, total / (double)context->ticks,
           Percentile(samples, context->ticks, 0.50),
           Percentile(samples, context->ticks, 0.95),
           detach.stats.regionsProcessed, detach.stats.detachChecks,
           detach.stats.detachCellsExplored, detach.stats.autoDetachSucceeded,
           detach.stats.autoDetachCells,
           detach.stats.autoDetachRejectedAnchored,
           detach.stats.autoDetachRejectedUnknown,
           detach.stats.autoDetachRejectedTooSmall,
           detach.stats.autoDetachRejectedTooLarge);
    DynamicTerrainUnload(&terrain);
}

/* Blast delivery. The first row is the claim that matters: a tick in which no
   power fired must cost nothing, because most ticks are that tick. The others
   bound the cost when one does — a flat pass over a hard budget of 32 bodies,
   plus, for the cone, one coarse line-of-sight march per body it reaches. */
typedef struct {
    const char *name;
    bool fire;
    TerrainBlastShape shape;
} ImpulseBenchScenario;

static void RunImpulseBenchmark(BenchContext *context, double *samples,
                                ImpulseBenchScenario scenario)
{
    DynamicTerrainSystem terrain;
    TerrainImpulseSystem impulses;
    World *world = context->world;
    Vector2 origin;
    double total = 0.0;
    int index;
    int frame;

    if (!DynamicTerrainInit(&terrain)) {
        printf("impulse              allocation failed\n");
        return;
    }
    TerrainImpulseInit(&impulses);
    PrepareScenario(context);
    origin = (Vector2){(float)context->centerX, (float)DETACH_BENCH_GROUND};

    /* Open ground around the blast. The generated world is solid here, and a
       cone blast fired from inside rock is occluded before it starts — a
       correct answer, and a row that measures nothing. */
    FillRectangle(world, context->centerX - 140, DETACH_BENCH_GROUND - 120,
                  context->centerX + 140, DETACH_BENCH_GROUND, MATERIAL_EMPTY);

    /* A full manager, spread across the blast's reach so some bodies are hit
       hard, some faintly and some not at all. */
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        TerrainBodyHandle handle = DynamicTerrainAllocBody(&terrain, 12, 12);
        TerrainBody *body;
        int y;

        for (y = 0; y < 12; ++y) {
            int x;

            for (x = 0; x < 12; ++x) {
                DynamicTerrainSetCell(&terrain, handle, x, y, MATERIAL_ROCK, 20.0f);
            }
        }
        DynamicTerrainFinalizeBody(&terrain, handle);
        body = DynamicTerrainGet(&terrain, handle);
        if (body == NULL) {
            break;
        }
        body->position = (Vector2){origin.x - 60.0f + (float)index * 4.0f,
                                   origin.y - 40.0f};
    }

    for (frame = 0; frame < context->ticks; ++frame) {
        double start;

        if (scenario.fire) {
            TerrainBlast blast;

            blast.shape = scenario.shape;
            blast.origin = origin;
            blast.direction = (Vector2){0.0f, -1.0f};
            blast.radius = scenario.shape == TERRAIN_BLAST_CONE
                               ? ABILITY_FORCE_LENGTH
                               : ABILITY_EXPLOSION_SHOCK_RADIUS;
            blast.spreadCosine = ABILITY_FORCE_SPREAD_COSINE;
            blast.momentum = ABILITY_EXPLOSION_BODY_IMPULSE;
            (void)TerrainImpulseQueueBlast(&impulses, blast);
        }

        start = NowSeconds();
        (void)TerrainImpulseApply(&impulses, &terrain, NULL, world);
        samples[frame] = (NowSeconds() - start) * 1000.0;
        total += samples[frame];

        /* Velocities are cleared outside the clock so every frame starts from
           the same state and the row measures delivery, not accumulation. */
        for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
            terrain.bodies[index].velocity = (Vector2){0.0f, 0.0f};
            terrain.bodies[index].angularVelocity = 0.0f;
        }
    }

    printf("impulse %-19s avg=%7.4f ms  p50=%7.4f  p95=%7.4f  "
           "blasts=%5d applied=%7d explosion=%7d force=%7d occluded=%6d "
           "asleep=%5d\n",
           scenario.name, total / (double)context->ticks,
           Percentile(samples, context->ticks, 0.50),
           Percentile(samples, context->ticks, 0.95),
           impulses.stats.blastsApplied,
           impulses.stats.bodyImpulseApplications,
           impulses.stats.bodiesAffectedByExplosion,
           impulses.stats.bodiesAffectedByForce,
           impulses.stats.bodiesOccluded, impulses.stats.bodiesLeftSleeping);
    DynamicTerrainUnload(&terrain);
}

/* Sustained destructive traversal, driven through the real GameUpdate so the
   number means what a player would feel rather than what one subsystem costs in
   isolation. Open cave, then dense rock, then a slab on a support that comes
   loose and has to be flown through.

   This is the scenario the FPS complaint is about, and the one any claim of
   having made it faster has to be measured against. */
#define TRAVERSAL_ALTITUDE 150
#define TRAVERSAL_LENGTH 3600

static void RunTraversalBenchmark(double *samples, int ticks)
{
    GameState game;
    GameConfig config = GameDefaultConfig();
    GameEventBuffer events;
    GameInput input;
    double total = 0.0;
    double worst = 0.0;
    uint64_t drilled = 0;
    uint64_t dirty = 0;
    uint64_t cells = 0;
    int centre;
    int tick;
    int worstTick = 0;
    int worstChunks = 0;
    int worstCells = 0;
    float minimumSpeed = 1.0e9f;
    int x;
    int y;

    config.seed = 0x7a7e5u;
    if (!GameInit(&game, config)) {
        printf("traversal            allocation failed\n");
        return;
    }
    centre = game.world.width / 2;

    /* Open sky to build speed in, then a wall of rock to spend it on. */
    for (y = TRAVERSAL_ALTITUDE - 90; y <= TRAVERSAL_ALTITUDE + 90; ++y) {
        for (x = centre - 80; x <= centre + 600; ++x) {
            WorldSetCell(&game.world, x, y, MATERIAL_EMPTY);
        }
    }
    for (y = TRAVERSAL_ALTITUDE - 90; y <= TRAVERSAL_ALTITUDE + 90; ++y) {
        for (x = centre + 600; x <= centre + TRAVERSAL_LENGTH; ++x) {
            WorldSetCell(&game.world, x, y, MATERIAL_ROCK);
        }
    }
    /* A slab on a thin support, right in the flight path, so detachment and a
       live terrain body are part of what is being measured. */
    /* Small enough to pass the automatic-detach size policy: a slab too big to
       be worth a body would simply be refused, and the scenario would measure
       nothing about detachment at all. */
    for (y = TRAVERSAL_ALTITUDE - 40; y <= TRAVERSAL_ALTITUDE - 29; ++y) {
        for (x = centre + 320; x <= centre + 360; ++x) {
            WorldSetCell(&game.world, x, y, MATERIAL_ROCK);
        }
    }
    for (y = TRAVERSAL_ALTITUDE - 28; y <= TRAVERSAL_ALTITUDE + 90; ++y) {
        WorldSetCell(&game.world, centre + 340, y, MATERIAL_ROCK);
    }

    /* A slab placed straight in the flight path as a body, not left to chance.
       The scenario has to measure drilling through detached terrain, and
       engineering a detachment that happens to end up in front of the player is
       a lot of fixture for a fact the extraction API can state directly. */
    {
        TerrainBodyHandle slab = DynamicTerrainAllocBody(&game.dynamicTerrain,
                                                         26, 34);
        int cx;
        int cy;

        for (cy = 0; cy < 34; ++cy) {
            for (cx = 0; cx < 26; ++cx) {
                DynamicTerrainSetCell(&game.dynamicTerrain, slab, cx, cy,
                                      MATERIAL_ROCK, 20.0f);
            }
        }
        DynamicTerrainFinalizeBody(&game.dynamicTerrain, slab);
        DynamicTerrainGet(&game.dynamicTerrain, slab)->position =
            (Vector2){(float)(centre + 480), (float)TRAVERSAL_ALTITUDE};
        /* Settled, or gravity carries it out of the flight path long before the
           player gets there and the scenario quietly stops measuring the thing
           it was built to measure. */
        for (cy = 0; cy < 64 &&
                     DynamicTerrainGetConst(&game.dynamicTerrain, slab)->awake;
             ++cy) {
            DynamicTerrainSettleBody(&game.dynamicTerrain,
                                     DynamicTerrainGet(&game.dynamicTerrain,
                                                       slab),
                                     1.0f / 60.0f);
        }
    }

    game.player.position = (Vector2){(float)(centre - 60),
                                     (float)TRAVERSAL_ALTITUDE};
    game.player.velocity = (Vector2){0.0f, 0.0f};
    memset(&input, 0, sizeof(input));
    input.move = (Vector2){1.0f, 0.0f};
    input.boostHeld = true;

    for (tick = 0; tick < ticks; ++tick) {
        double start;

        input.aimWorld = (Vector2){game.player.position.x + 60.0f,
                                   game.player.position.y};
        start = NowSeconds();
        GameUpdate(&game, &input, 1.0f / 60.0f, &events);
        samples[tick] = (NowSeconds() - start) * 1000.0;
        total += samples[tick];
        /* The first few ticks are the scene settling after it was built, not
           anything a player would ever experience. Measuring the spike there
           would hide the one that matters. */
        if (tick >= 5 && samples[tick] > worst) {
            worst = samples[tick];
            worstTick = tick;
            worstChunks = (int)game.world.lastTickStats.processedChunks;
            worstCells = (int)game.world.lastTickStats.processedCells;
        }
        if (tick >= 60) {
            /* Once the rock has been reached. The speed at the final tick says
               only where the player happened to be standing; the lowest speed
               after entering the ground says what the ground cost them. */
            float now = sqrtf(game.player.velocity.x * game.player.velocity.x +
                              game.player.velocity.y * game.player.velocity.y);

            if (now < minimumSpeed) {
                minimumSpeed = now;
            }
        }
        drilled += (uint64_t)game.player.drilledCells;
        dirty += game.world.lastTickStats.processedChunks;
        cells += game.world.lastTickStats.processedCells;
    }

    printf("traversal            avg=%7.3f ms  p50=%7.3f  p95=%7.3f  "
           "worst=%7.3f  speed=%5.0f floor=%5.0f travel=%6.0f drilled=%7llu cells/tick=%8llu "
           "chunks/tick=%5llu  detach=%3d carved=%4d split=%3d bodies=%2d "
           "bodyDrill=%5d fx=%3u  worst@%3d(%5d chunks %7d cells)\n",
           total / (double)ticks, Percentile(samples, ticks, 0.50),
           Percentile(samples, ticks, 0.95), worst,
           (double)sqrtf(game.player.velocity.x * game.player.velocity.x +
                         game.player.velocity.y * game.player.velocity.y),
           (double)minimumSpeed,
           (double)(game.player.position.x - (float)(centre - 60)),
           (unsigned long long)drilled,
           (unsigned long long)(cells / (uint64_t)ticks),
           (unsigned long long)(dirty / (uint64_t)ticks),
           game.detach.stats.autoDetachSucceeded, game.damage.stats.cellsCarved,
           game.damage.stats.fractureSplits,
           DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies,
           game.interaction.stats.bodyCellsDrilled,
           (unsigned int)events.count, worstTick, worstChunks, worstCells);
    GameUnload(&game);
}

static void PrintMemory(const World *world)
{
    size_t cellCount = (size_t)world->width * (size_t)world->height;
    size_t chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    size_t lightCount = (size_t)world->lightColumns * (size_t)world->lightRows;
    size_t cells = cellCount * sizeof(*world->cells);
    /* Four byte-flag arrays (active, next-active, dirty, light-dirty) plus the
       two schedule column arrays and their two per-row counters. */
    size_t chunkMetadata = chunkCount * 4u * sizeof(uint8_t) +
                           chunkCount * 2u * sizeof(int32_t) +
                           (size_t)world->chunkRows * 2u * sizeof(int32_t);
    size_t lighting = lightCount * 6u * sizeof(float);
    size_t renderStaging =
        WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE * sizeof(Color) * 2u;
    size_t estimated = sizeof(*world) + cells + chunkMetadata + lighting;

    printf("memory: Cell=%zu B  cells=%.2f MiB  persistent_pixels=0.00 MiB  "
           "chunks=%.2f MiB  lighting=%.2f MiB  estimated=%.2f MiB  "
           "render_staging=%.2f KiB  rss=%.2f MiB  peak_rss=%.2f MiB\n",
           sizeof(Cell), (double)cells / 1048576.0,
           (double)chunkMetadata / 1048576.0,
           (double)lighting / 1048576.0,
           (double)estimated / 1048576.0,
           (double)renderStaging / 1024.0,
           (double)CurrentRssBytes() / 1048576.0,
           (double)PeakRssBytes() / 1048576.0);
}

int main(int argc, char **argv)
{
    static const Scenario scenarios[] = {
        {"settled world", SetupSettled, NULL},
        {"falling sand", SetupSand, NULL},
        {"large water", SetupWater, NULL},
        {"fire and lava", SetupFireLava, NULL},
        {"large explosion", SetupExplosion, StepExplosion},
        {"mass destruction", SetupDestruction, StepDestruction},
        {"boost drilling", SetupDrilling, StepDrilling},
        {"force ability", SetupForce, StepForce},
        {"cryo ability", SetupCryo, StepCryo},
        {"chaotic mixed", SetupMixed, StepMixed},
    };
    World world;
    BenchContext context = {0};
    double samples[BENCH_DEFAULT_TICKS * 2];
    double start;
    double initialized;
    double generated;
    double regenerated;
    size_t scenarioIndex;
    int ticks = BENCH_DEFAULT_TICKS;

    if (argc == 3 && strcmp(argv[1], "--ticks") == 0) {
        ticks = atoi(argv[2]);
    }
    if (ticks < 10 || ticks > (int)(sizeof(samples) / sizeof(samples[0]))) {
        fprintf(stderr, "--ticks must be between 10 and %zu\n",
                sizeof(samples) / sizeof(samples[0]));
        return 2;
    }

    start = NowSeconds();
    if (!WorldInit(&world, BENCH_WORLD_WIDTH, BENCH_WORLD_HEIGHT)) {
        fprintf(stderr, "benchmark: WorldInit failed\n");
        return 1;
    }
    initialized = NowSeconds();
    WorldGenerate(&world, BENCH_SEED);
    generated = NowSeconds();
    WorldGenerate(&world, BENCH_SEED);
    regenerated = NowSeconds();

    printf("Emberfall headless benchmark: %dx%d, %d ticks/scenario, seed=0x%llx\n",
           world.width, world.height, ticks, (unsigned long long)BENCH_SEED);
    printf("startup: init=%.3f ms  generate=%.3f ms  regenerate=%.3f ms\n",
           (initialized - start) * 1000.0,
           (generated - initialized) * 1000.0,
           (regenerated - generated) * 1000.0);
    PrintMemory(&world);
    printf("render: headless (lighting solve, texture uploads and uploaded bytes: n/a)\n");

    context.world = &world;
    context.centerX = world.width / 2;
    context.ticks = ticks;
    for (scenarioIndex = 0u;
         scenarioIndex < sizeof(scenarios) / sizeof(scenarios[0]);
         ++scenarioIndex) {
        RunScenario(&context, &scenarios[scenarioIndex], samples);
    }
    RunLightingBenchmark(&context, samples);
    {
        /* 32 bodies of 64x32 is 65536 occupied cells: the shipped cell budget
           exactly, and the worst case the budgets are meant to bound. */
        static const TerrainBenchScenario terrainScenarios[] = {
            {"idle",              0,                  16, 12, false, MAX_TERRAIN_BODIES},
            {"1 awake",           1,                  16, 12, false, MAX_TERRAIN_BODIES},
            {"16 awake",          16,                 16, 12, false, MAX_TERRAIN_BODIES},
            {"32 awake",          MAX_TERRAIN_BODIES, 16, 12, false, MAX_TERRAIN_BODIES},
            {"32 sleeping",       MAX_TERRAIN_BODIES, 16, 12, true,  MAX_TERRAIN_BODIES},
            {"32 shipped budget", MAX_TERRAIN_BODIES, 16, 12, false, 0},
            {"cells at budget",   MAX_TERRAIN_BODIES, 64, 32, false, MAX_TERRAIN_BODIES},
            {"cells asleep",      MAX_TERRAIN_BODIES, 64, 32, true,  MAX_TERRAIN_BODIES},
        };
        size_t terrainIndex;

        for (terrainIndex = 0u;
             terrainIndex < sizeof(terrainScenarios) / sizeof(terrainScenarios[0]);
             ++terrainIndex) {
            RunDynamicTerrainBenchmark(&context, samples,
                                       terrainScenarios[terrainIndex]);
        }
    }
    {
        static const DetachBenchScenario detachScenarios[] = {
            /*  name                  destroy frags anchored blastY   radius */
            {"no destruction",        false,  0,    false,   0,                       0},
            {"blast in solid rock",   true,   0,    false,   DETACH_BENCH_GROUND + 12, 8},
            {"one fragment",          true,   1,    false,   DETACH_BENCH_GROUND - 14, 20},
            {"four fragments",        true,   4,    false,   DETACH_BENCH_GROUND - 14, 20},
            {"large anchored slab",   true,   0,    true,    DETACH_BENCH_GROUND - 20, 12},
        };
        size_t detachIndex;

        for (detachIndex = 0u;
             detachIndex < sizeof(detachScenarios) / sizeof(detachScenarios[0]);
             ++detachIndex) {
            RunDetachBenchmark(&context, samples, detachScenarios[detachIndex]);
        }
    }
    {
        static const ImpulseBenchScenario impulseScenarios[] = {
            {"no blast",  false, TERRAIN_BLAST_RADIAL},
            {"explosion", true,  TERRAIN_BLAST_RADIAL},
            {"force cone", true, TERRAIN_BLAST_CONE},
        };
        size_t impulseIndex;

        for (impulseIndex = 0u;
             impulseIndex < sizeof(impulseScenarios) / sizeof(impulseScenarios[0]);
             ++impulseIndex) {
            RunImpulseBenchmark(&context, samples, impulseScenarios[impulseIndex]);
        }
    }
    RunTraversalBenchmark(samples, ticks);

    printf("final_peak_rss=%.2f MiB\n", (double)PeakRssBytes() / 1048576.0);
    WorldUnload(&world);
    return 0;
}
