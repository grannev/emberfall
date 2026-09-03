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

static inline bool WorldInBounds(const World *world, int x, int y)
{
    return x >= 0 && x < world->width && y >= 0 && y < world->height;
}

static inline size_t WorldIndex(const World *world, int x, int y)
{
    return (size_t)y * (size_t)world->width + (size_t)x;
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

/* world_simulation.c */
void WorldMoveCell(World *world, int fromX, int fromY, int toX, int toY);

#endif
