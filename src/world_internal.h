#ifndef WORLD_INTERNAL_H
#define WORLD_INTERNAL_H

/* Shared internals of the world module: cell addressing, chunk waking and the
   raw writes every world source file needs. Nothing outside src/world_*.c
   should include this; the public surface is world.h.

   The small accessors are `static inline` on purpose. They run several times
   per cell per simulation tick, and the whole point of splitting world.c was to
   separate responsibilities, not to insert a cross-module call into the hottest
   loop in the project. */

#include <stddef.h>
#include <stdint.h>

#include "materials.h"
#include "world.h"

#define FIRE_NEIGHBOR_HEAT_PER_TICK 0.65f
/* Lava heats whatever it touches, but a rock cell must never reach its melt
   threshold from lava alone: otherwise one pocket turns the entire map to lava,
   the way an unbudgeted fire would burn every connected dirt cell. Rock relaxes
   toward ambient at 0.6% of the gap per tick, so at the 720C threshold it sheds
   about 4.2C per tick. Keeping the per-neighbour contribution well under that
   share leaves a boundary cell glowing near 700C forever without melting. */
#define LAVA_NEIGHBOR_HEAT_PER_TICK 3.0f
/* The cap, not the rate, is what keeps a pocket from melting its lining: even
   a cell heated from eight sides at once lands well under rock's 720C. */
#define LAVA_PASSIVE_HEAT_CAP 660.0f
/* How long a capped source's hold on a neighbour outlives the push. See
   Cell.heatHeld for why it is two. */
#define WORLD_HEAT_HOLD_TICKS 2u
/* Friction heat left on a drilled tunnel wall. Deliberately below the water
   steam point (108) and far below the dirt ignition point (175). */
#define DRILL_WALL_TEMPERATURE 96.0f
/* How far a liquid may run sideways in one tick.
 *
 * One cell a tick is the default a falling-sand liquid gets, and it is why
 * water here settled into standing wedges and sloped surfaces instead of a
 * level one: displacement crosses a pool at one cell per tick, and a pool is
 * hundreds of cells wide. Six is enough that a surface flattens within a moment
 * of being disturbed, which is the only thing a player reads as water. Lava
 * keeps a short run on purpose — it is supposed to crawl. */
#define WORLD_WATER_DISPERSION 16
#define WORLD_LAVA_DISPERSION 2
/* How many times a surface liquid cell may slide along the top of a pool
   without finding anywhere to fall before it lies still. Each slide is a run
   of up to WORLD_LIQUID_WANDER_REACH cells in the direction of the last one,
   so fifteen carry a grain the length of the widest pool the generator makes;
   and a grain with nowhere to go stops costing its chunks within a moment.
   Fifteen because the counter shares `lifetime` with the pressure head and
   the direction of the last slide (world_fluid.h). */
#define WORLD_LIQUID_WANDER_LIMIT 15u

/* The world wraps: its right edge is joined to its left, so a column index is
   taken modulo the width wherever it is used, and every x is in the world.
   Only a row can be outside it. Branch first on the common case — almost every
   x is already inside — so the hot loops pay a compare, not a division. */
static inline int WorldWrapColumn(int x, int width)
{
    /* One unsigned compare covers both ends. */
    if ((unsigned)x < (unsigned)width) {
        return x;
    }
    /* Everything that moves is kept within half a turn of the character, who
       is kept inside the map, so one width either way almost always does. */
    x += x < 0 ? width : -width;
    if ((unsigned)x < (unsigned)width) {
        return x;
    }
    x %= width;
    return x < 0 ? x + width : x;
}

static inline int WorldWrapX(const World *world, int x)
{
    return WorldWrapColumn(x, world->width);
}

static inline bool WorldInBounds(const World *world, int x, int y)
{
    (void)x;
    return y >= 0 && y < world->height;
}

static inline size_t WorldIndex(const World *world, int x, int y)
{
    return (size_t)y * (size_t)world->width + (size_t)WorldWrapX(world, x);
}

static inline size_t WorldChunkIndex(const World *world, int chunkX, int chunkY)
{
    return (size_t)chunkY * (size_t)world->chunkColumns + (size_t)chunkX;
}

static inline Cell *WorldCell(World *world, int x, int y)
{
    return &world->cells[WorldIndex(world, x, y)];
}

static inline const Cell *WorldCellConst(const World *world, int x, int y)
{
    return &world->cells[WorldIndex(world, x, y)];
}

/* Same contract as the public WorldGetCell — outside the map reads as rock, so
   the world edge behaves like an unbreakable wall — without its null checks,
   which internal callers have already satisfied. */
/* The back layer behind cell (x, y): MATERIAL_EMPTY where there is none,
   which is the sky. */
static inline CellMaterial WorldBackWallAt(const World *world, int x, int y)
{
    int column;

    if (y < 0 || y >= world->height || world->backWalls == NULL) {
        return MATERIAL_EMPTY;
    }
    column = WorldWrapX(world, x) / WORLD_BACK_WALL_SCALE;
    return (CellMaterial)world->backWalls[(size_t)(y / WORLD_BACK_WALL_SCALE) *
                                              (size_t)world->backWallColumns +
                                          (size_t)column];
}

static inline CellMaterial WorldMaterialAt(const World *world, int x, int y)
{
    if (!WorldInBounds(world, x, y)) {
        return MATERIAL_ROCK;
    }
    return (CellMaterial)WorldCellConst(world, x, y)->material;
}

/* The tick and effect counters as a cell stores them. Both skip zero so that
   never-written cells, whose stamps are zero, can never be mistaken for
   already-handled ones. See the Cell comment in world.h. */
static inline uint16_t WorldTickStamp(const World *world)
{
    return (uint16_t)world->tick;
}

/* Starts a new effect and returns the stamp its cells should carry. */
static inline uint16_t WorldNextEffectStamp(World *world)
{
    uint16_t stamp = (uint16_t)++world->effectSerial;

    if (stamp == 0u) {
        stamp = (uint16_t)++world->effectSerial;
    }
    return stamp;
}

static inline uint32_t CoordinateHash(int x, int y)
{
    uint32_t value = (uint32_t)x * 0x45d9f3bu;

    value ^= (uint32_t)y * 0x27d4eb2du;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return value;
}

/* The per-chunk material counts. `WorldCountMaterialChange` is the only way
   the counts move, and every function that writes a cell's material calls
   it: WorldSetGeneratedCell, WorldSetCellRaw and WorldMoveCell. A count that
   is ever too low skips a real reaction, so nothing writes `material` past
   them. */
static inline uint16_t *WorldMaterialCounter(World *world, size_t chunkIndex,
                                             CellMaterial material)
{
    if (material == MATERIAL_WATER) return &world->chunkWater[chunkIndex];
    if (material == MATERIAL_LAVA) return &world->chunkLava[chunkIndex];
    return NULL;
}

static inline void WorldCountMaterialChange(World *world, int x, int y,
                                            CellMaterial from, CellMaterial to)
{
    size_t chunkIndex;
    uint16_t *counter;

    if (from == to) {
        return;
    }
    chunkIndex = WorldChunkIndex(world, WorldWrapX(world, x) / WORLD_CHUNK_SIZE,
                                y / WORLD_CHUNK_SIZE);
    counter = WorldMaterialCounter(world, chunkIndex, from);
    if (counter != NULL && *counter > 0u) {
        --*counter;
    }
    counter = WorldMaterialCounter(world, chunkIndex, to);
    if (counter != NULL) {
        ++*counter;
    }
}

/* Whether `material` exists in the chunk holding (x, y) or in any chunk the
   cell touches across a border, which is every chunk one of its eight
   neighbours can lie in. Conservative: a true answer means "worth scanning",
   never "found". */
static inline bool WorldNeighbourhoodHolds(const World *world, int x, int y,
                                           CellMaterial material)
{
    const uint16_t *counts = material == MATERIAL_WATER ? world->chunkWater
                                                        : world->chunkLava;
    int chunkX = x / WORLD_CHUNK_SIZE;
    int chunkY = y / WORLD_CHUNK_SIZE;
    int firstChunkX = chunkX - (x % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    int lastChunkX = chunkX + (x % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    int firstChunkY = chunkY - (y % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    int lastChunkY = chunkY + (y % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    int probeY;

    if (counts == NULL) {
        return true;
    }
    if (firstChunkX < 0) firstChunkX = 0;
    if (firstChunkY < 0) firstChunkY = 0;
    if (lastChunkX > world->chunkColumns - 1) lastChunkX = world->chunkColumns - 1;
    if (lastChunkY > world->chunkRows - 1) lastChunkY = world->chunkRows - 1;
    for (probeY = firstChunkY; probeY <= lastChunkY; ++probeY) {
        int probeX;

        for (probeX = firstChunkX; probeX <= lastChunkX; ++probeX) {
            if (counts[WorldChunkIndex(world, probeX, probeY)] > 0u) {
                return true;
            }
        }
    }
    return false;
}

/* world_storage.c
 *
 * Every wake goes through WorldWakeCellAndNeighbors, which is the only caller
 * of the internal scheduler: the flag array and the compact per-row lists must
 * agree, and one entry point is what guarantees they do. A module that needs to
 * schedule work should wake a cell, not reach for the schedule. */
void WorldWakeCellAndNeighbors(World *world, int x, int y);
void WorldSetCellRaw(World *world, int x, int y, CellMaterial material);
void WorldSetGeneratedCell(World *world, int x, int y, CellMaterial material);
void WorldCountActiveState(World *world);

/* world_biomes.c */
void WorldGenerateBiomeTerrain(World *world);

/* What world_biomes.c shares with world_structures.c, which builds on the
   landscape it has made: the generated surface, the actual top of a column,
   the seed hashes every placement is drawn from, and a tree. Private to the
   generator. */
int WorldGenSurfaceY(const World *world, int x);
int WorldGenSolidY(const World *world, int x);
uint64_t WorldGenHash(uint64_t seed, int x, int y, uint64_t channel);
float WorldGenUnit(uint64_t seed, int x, int y, uint64_t channel);
Rng WorldGenFeatureRng(uint64_t seed, int feature, uint64_t channel);
bool WorldGenNearSpawn(const World *world, int x);
void WorldGenPlaceTree(World *world, int x, int groundY, Rng *rng);
/* A blade of meadow grass (or none) standing on the soil at (x, groundY). */
void WorldGenGrowGrass(World *world, int x, int groundY, Rng *rng);
/* Sets the back layer over a box of cells (inclusive, columns wrapped) to
   `material`, block by block. */
void WorldGenSetBackWall(World *world, int firstX, int firstY, int lastX, int lastY,
                         CellMaterial material);
/* The natural back layer: rock behind everything under the ground's
   surface, by biome and depth. */
void WorldGenerateBackWalls(World *world);
/* The structures, in the order the landscape needs them: underground
   before the surface is finished, the surface ruins before the sea is
   poured. */
void WorldGenerateUnderground(World *world);
void WorldGenerateRuins(World *world);

/* world_simulation.c */
void WorldMoveCell(World *world, int fromX, int fromY, int toX, int toY);

#endif
