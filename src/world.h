#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "rng.h"

#define WORLD_CHUNK_SIZE 32
/* Light is solved on a coarser grid than the cells. Eight divides the chunk
   size, so every light cell belongs to exactly one chunk and the dirty-chunk
   bookkeeping stays exact. The field is smooth and sampled bilinearly, so a
   finer grid buys no visible detail and costs four times the solve. */
#define WORLD_LIGHT_SCALE 8

typedef enum CellMaterial {
    MATERIAL_EMPTY = 0,
    MATERIAL_DIRT,
    MATERIAL_ROCK,
    MATERIAL_SAND,
    MATERIAL_WATER,
    MATERIAL_LAVA,
    MATERIAL_STEAM,
    MATERIAL_SMOKE,
    MATERIAL_FIRE,
    MATERIAL_ASH,
    MATERIAL_ICE,
    /* Flora. Static solids that grow on a surface rather than fall onto it:
       they hold their shape like rock, weigh almost nothing, and burn. Four
       rather than one because what grows on a surface is most of what tells the
       player which biome they are standing in — a dune with a pine on it is not
       a dune. */
    MATERIAL_WOOD,
    MATERIAL_LEAF,
    MATERIAL_GRASS,
    MATERIAL_CACTUS,
    MATERIAL_COUNT
} CellMaterial;

/* Large horizontal generation regions. Biomes choose terrain shape and
   material composition; material physics remains defined by CellMaterial and
   the single material table. */
typedef enum WorldBiome {
    WORLD_BIOME_TEMPERATE = 0,
    WORLD_BIOME_DUNES,
    WORLD_BIOME_FROST,
    WORLD_BIOME_VOLCANIC,
    WORLD_BIOME_COUNT
} WorldBiome;

/* Fourteen million of these exist, so every byte here is 13.5 MiB on the
   production map and the layout is a memory decision before it is anything
   else. Fields are ordered widest first so the struct packs to 12 bytes with a
   single byte of tail padding. */
typedef struct Cell {
    float temperature;
    /* Both stamps are compared for equality against a world counter and are
       therefore only meaningful modulo their own width. Sixteen bits cost 8 MiB
       less than thirty-two and the failure they admit is bounded and tiny: a
       cell whose stamp happens to equal the truncated counter is skipped by
       exactly one tick, or missed by exactly one effect, and behaves normally
       again immediately afterwards. For `updatedTick` that requires a cell to
       have sat awake and untouched for exactly a multiple of 65 536 ticks —
       eighteen minutes — and costs it one frame of falling. The counters skip
       the value zero precisely so that never-written cells, which are the one
       population large enough for this to be visible, can never collide. */
    uint16_t updatedTick;
    uint16_t effectStamp;
    /* Age of the temporary materials: fire, smoke and steam. The longest life
       any of them has is 420 ticks. */
    uint16_t lifetime;
    /* MATERIAL_COUNT is deliberately kept below 256. Storing the enum as an
       int wasted four bytes in every cell; on the 16384-wide world that was
       about 54 MiB for no gameplay value. */
    uint8_t material;
} Cell;

_Static_assert(MATERIAL_COUNT <= UINT8_MAX, "Cell.material no longer fits in uint8_t");
_Static_assert(sizeof(Cell) == 12, "Cell layout grew; recheck large-world memory");

#define MAX_WORLD_REACTIONS 64

typedef struct WorldReactionEvent {
    Vector2 position;
} WorldReactionEvent;

/* Regions a destructive operation cut solid material out of, since the log was
   last cleared.

   The world records these and nothing more. It does not know that terrain
   bodies exist and must not learn: this is a fact about the world itself —
   "structural material was removed here" — and it is the only thing the
   cellular simulation contributes to automatic detachment.

   Only explicit destructive operations write here. Ordinary simulation does
   not: sand that falls has not cut anything, and a connectivity search after
   every settled grain is exactly the full-world scan this design exists to
   avoid. */
#define MAX_WORLD_DESTRUCTION_REGIONS 8

/* The largest box the log will aggregate into one entry. Two cuts far enough
   apart stay two entries rather than becoming one box with untouched ground in
   the middle. Whoever consumes the log has to be able to look at a whole entry
   plus some ground around it, so this is deliberately smaller than any sensible
   search window; terrain_detach.c asserts the relation it needs. */
#define WORLD_DESTRUCTION_MAX_SPAN 96

/* Inclusive cell bounds. Ints rather than a Rectangle because these are cells,
   not a drawing area, and rounding a region is how an off-by-one becomes a
   wrong answer about what is connected to what. */
typedef struct WorldDestructionRegion {
    int minimumX;
    int minimumY;
    int maximumX;
    int maximumY;
} WorldDestructionRegion;

typedef struct LaserResult {
    Vector2 position;
    CellMaterial material;
    bool hit;
} LaserResult;

/* Work performed by the most recent fixed simulation tick. These counters are
   deliberately structural rather than time-based: they stay meaningful across
   machines and make performance regressions testable without flaky deadlines. */
typedef struct WorldTickStats {
    uint64_t processedCells;
    uint32_t processedChunks;
} WorldTickStats;

typedef struct World {
    int width;
    int height;
    Cell *cells;
    uint32_t tick;
    uint32_t effectSerial;
    /* The seed the current terrain was generated from, and the stream every
       later world mutation draws from. Both are part of the world's state on
       purpose: a world plus the inputs applied to it must replay identically,
       and that is impossible if an effect draws from a generator shared with
       the frame loop. */
    uint64_t seed;
    Rng rng;
    WorldTickStats lastTickStats;
    WorldReactionEvent reactions[MAX_WORLD_REACTIONS];
    int reactionCount;
    /* Destructive cuts awaiting a detach check. Aggregated on write, drained by
       whoever runs the check; `destructionDropped` counts the regions the log
       had no room for, which is a refusal to look rather than a lost mutation:
       the world is correct either way, some terrain merely stays static. */
    WorldDestructionRegion destruction[MAX_WORLD_DESTRUCTION_REGIONS];
    int destructionCount;
    int destructionDropped;
    int chunkColumns;
    int chunkRows;
    int activeChunkCount;
    /* The simulation schedule, kept in two representations because both are
       needed: a flag per chunk for O(1) membership, and a compact per-chunk-row
       list of active columns for iteration. Without the lists a settled world
       still walked every chunk slot of every row — 442 000 of them per tick on
       the production map — to discover it had nothing to do.

       `activeChunks` is the set being simulated and is frozen for the duration
       of a tick; `nextActiveChunks` accumulates what the tick woke. They swap
       at the end of WorldUpdate. */
    uint8_t *activeChunks;
    uint8_t *nextActiveChunks;
    int32_t *activeRowColumns;
    int32_t *activeRowCount;
    int32_t *nextRowColumns;
    int32_t *nextRowCount;
    /* True only inside WorldUpdate. A wake during a tick schedules the next
       one; a wake between ticks — a laser, a settling particle, a drilled
       tunnel — schedules the tick about to run. */
    bool simulating;
    /* Chunks whose pixels changed since the last upload. The simulation already
       tracks where work happens; the renderer reuses that instead of rebuilding
       the whole texture every frame. */
    uint8_t *dirtyChunks;
    /* Chunks whose light inputs are stale. Separate from `dirtyChunks` because
       the two are consumed at different times: light must be refreshed once,
       wherever the terrain changed, while a pixel rebuild waits until the chunk
       is on screen and so may stay pending for many frames. Sharing one flag
       makes the light refresh re-scan every off-screen chunk every frame. */
    uint8_t *lightDirtyChunks;
    /* Coarse light field. `emission` and `opacity` are derived from the cells and
       refreshed only where chunks are dirty; `light` is solved from them every
       draw; `lightShown` is the copy the current texture was built from, so a
       chunk can be re-lit without anything in it having changed. */
    int lightColumns;
    int lightRows;
    /* Two channels, not one. A single intensity can darken but cannot colour,
       so a lava lake lit its own cavern in grey. `lightSky` is daylight reaching
       down from the surface, `lightEmber` is everything that burns, and the
       difference between them is what warms the light near a fire. */
    float *lightSky;
    float *lightEmber;
    float *lightShownSky;
    float *lightShownEmber;
    float *lightEmission;
    float *lightOpacity;
    /* How much daylight the sky is giving, 0 at midnight and 1 at noon. Sky
       light is seeded per column from the top, so scaling the seed is the whole
       of night: a column open to the sky simply receives less, the two sweeps
       carry less into every overhang, and the ground the sun was reaching goes
       as dark as the ground it never reached. Nothing else in the simulation
       reads it — night changes what can be seen, not what happens. */
    float daylight;
    /* One movable light the caller owns, so the player can carry their own glow
       into a tunnel that has no other source. */
    Vector2 pointLight;
    float pointLightRadius;
    float pointLightStrength;
    /* Daylight the last solve used, so a sky that is still brightening or
       dimming re-solves and a sky that has settled does not. */
    float solvedDaylight;
    /* State of the light the last solve was run for, so a still scene can skip
       the solve entirely. */
    Vector2 solvedPointLight;
    float solvedPointLightStrength;
    uint32_t solvedTick;
    /* The column window the last solve covered. Sky light is re-solved when the
       terrain changed or when the window moved onto columns the last solve did
       not reach; a lamp moving inside an unchanged window needs only ember. */
    int solvedFirstColumn;
    int solvedLastColumn;
    bool lightSolved;
} World;

bool WorldInit(World *world, int width, int height);
void WorldUnload(World *world);
/* Generates terrain from `seed` and stores it. The same seed always produces
   the same world, which is what makes bug reports, regression tests and
   benchmark scenarios repeatable. */
void WorldGenerate(World *world, uint64_t seed);
/* The nominal biome at a column. Terrain parameters blend near boundaries, so
   this is an identity/debug query rather than a hard material border. */
WorldBiome WorldBiomeAt(const World *world, int x);
const char *WorldBiomeName(WorldBiome biome);
Vector2 WorldPlayerSpawn(const World *world);
/* Wakes generated dynamic or heated cells inside a streamed gameplay region.
   Actual cell mutations wake themselves regardless of this region. */
void WorldActivateRegion(World *world, Rectangle region);
void WorldUpdate(World *world);
/* Position of the caller-owned light, applied on the next draw. A strength of
   zero disables it. */
void WorldSetPointLight(World *world, Vector2 position, float radius, float strength);
/* Sets how much daylight the sky gives, clamped to 0..1. Applied on the next
   solve, like the point light. */
void WorldSetDaylight(World *world, float daylight);

CellMaterial WorldGetCell(const World *world, int x, int y);
int WorldCountDynamicCells(const World *world);
float WorldGetTemperature(const World *world, int x, int y);
void WorldSetTemperature(World *world, int x, int y, float temperature);
bool WorldMaterialIsSolid(CellMaterial material);
void WorldSetCell(World *world, int x, int y, CellMaterial material);

/* Notes that solid material was cut out of the given inclusive cell bounds.
   Destructive world effects call this for themselves; a caller outside the
   world module needs it only when it removes structural cells by some other
   route. Overlapping or touching regions are merged, so an operation that hits
   the same area many times in a tick costs one entry rather than many. */
void WorldRecordDestruction(World *world, int minimumX, int minimumY,
                            int maximumX, int maximumY);
/* Empties the log. Whoever consumes the regions is responsible for this;
   nothing clears them implicitly, so a tick that never runs a detach check does
   not silently discard what it was told. */
void WorldClearDestruction(World *world);

void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
int WorldDrillCircle(World *world, int centerX, int centerY, int radius);
/* The dent a heavy blow leaves, plus the fractures running out of it.
 *
 * A crater rather than a hole: the bowl is wider than it is deep, so the impact
 * reads as something enormous having struck a surface rather than as a shot
 * having been fired into it. The cracks are short deterministic rays through
 * whatever is still solid — no stress model, no propagation, just the shape a
 * player recognises as "that hit hard".
 *
 * `direction` is the way the blow was travelling; cracks favour it and the
 * surface either side of it. Everything is bounded by the radius and the crack
 * length, and the damage is logged the way every other destructive cut is. */
void WorldApplyPunch(World *world, Vector2 at, Vector2 direction, int radius,
                     int crackCount, int crackLength);
void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius);
/* What an explosion leaves behind, as opposed to what it removes.
 *
 * A circle of deleted cells is a hole punched in a picture: the rim is smooth,
 * nothing around it changed, and the rock the blast did not reach looks exactly
 * as it did a moment before. This is the same event with the consequences left
 * in — a crater whose rim is torn rather than drawn, a ring of rock left glowing
 * around it, and branching fractures thrown out into the ground beyond, which
 * carry the blast's reach much further than its radius and are what make the
 * next shot land in rock that already remembers the last one.
 *
 * `rockToLavaChance` is the same molten-slag chance WorldDestroyCircle takes.
 * Everything is bounded by `coreRadius` and `crackLength`, and the damage is
 * logged the way every other destructive cut is, so what it cuts free falls. */
void WorldApplyBlast(World *world, Vector2 at, int coreRadius,
                     float rockToLavaChance, int crackCount, int crackLength);
/* One heavy blow along a cone: throws dynamic cells a long way and scours a thin
   layer off the exposed face of anything solid. `spreadCosine` is the cosine of
   the cone's half angle; `reach` is how far the nearest cells are thrown. */
void WorldApplyForceBlast(World *world, Vector2 origin, Vector2 direction,
                          float length, float spreadCosine, int reach);
/* Where the beam first meets solid material, changing nothing. Split out of
   WorldApplyLaser so a caller can find out what the beam would hit before
   deciding whether it is the beam's real target: a detached body standing in
   front of that wall has to stop the beam, and the wall behind it must not be
   burned by a shot that never reached it. */
LaserResult WorldBeamHit(const World *world, Vector2 start, Vector2 end);
LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
/* Thermal inverse of the laser: chills everything along the ray, freezing water
   to ice and settling lava back into rock. */
LaserResult WorldApplyChill(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
const char *WorldMaterialName(CellMaterial material);

#endif
