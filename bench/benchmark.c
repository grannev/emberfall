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
#include "terrain_physics.h"
#include "world_lighting.h"

#define BENCH_WORLD_WIDTH 16384
#define BENCH_WORLD_HEIGHT 864
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

    printf("final_peak_rss=%.2f MiB\n", (double)PeakRssBytes() / 1048576.0);
    WorldUnload(&world);
    return 0;
}
