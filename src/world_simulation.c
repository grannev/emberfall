/* The per-tick motion rules: what falls, what flows, what rises, and the fixed
 * traversal that applies them.
 *
 * Ordering is the invariant that matters here. Rows run bottom-to-top so a
 * falling cell cannot move twice in one tick, horizontal direction alternates
 * per row and tick so material does not drift in one direction forever, and
 * `updatedTick` is the exact guard behind both.
 */
#include "world_internal.h"

#include <math.h>
#include <string.h>

#include "world_thermal.h"

void WorldMoveCell(World *world, int fromX, int fromY, int toX, int toY)
{
    Cell *from = WorldCell(world, fromX, fromY);
    Cell *to = WorldCell(world, toX, toY);
    Cell moving = *from;

    *from = *to;
    *to = moving;
    to->updatedTick = world->tick;
    from->updatedTick = world->tick;
    WorldWakeCellAndNeighbors(world, fromX, fromY);
    WorldWakeCellAndNeighbors(world, toX, toY);
}

static bool WorldTryMoveInto(World *world, int x, int y, int targetX, int targetY,
                             bool allowWaterSwap)
{
    CellMaterial target;

    if (!WorldInBounds(world, targetX, targetY)) {
        return false;
    }

    target = WorldMaterialAt(world, targetX, targetY);
    if (target == MATERIAL_EMPTY || (allowWaterSwap && target == MATERIAL_WATER)) {
        WorldMoveCell(world, x, y, targetX, targetY);
        return true;
    }

    return false;
}

static void WorldUpdateSand(World *world, int x, int y, int direction)
{
    if (WorldTryMoveInto(world, x, y, x, y + 1, true)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y + 1, true)) {
        return;
    }
    (void)WorldTryMoveInto(world, x, y, x - direction, y + 1, true);
}

static void WorldUpdateLiquid(World *world, int x, int y, int direction, bool viscous)
{
    if (viscous && ((world->tick + (uint32_t)x + (uint32_t)y) % 3u != 0u)) {
        return;
    }

    if (WorldTryMoveInto(world, x, y, x, y + 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y + 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x - direction, y + 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y, false)) {
        return;
    }
    (void)WorldTryMoveInto(world, x, y, x - direction, y, false);
}

static void WorldUpdateGasMotion(World *world, int x, int y, int direction, bool slow)
{
    if (slow && ((world->tick + (uint32_t)x + (uint32_t)y) & 1u) != 0u) {
        return;
    }

    if (WorldTryMoveInto(world, x, y, x, y - 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y - 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x - direction, y - 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y, false)) {
        return;
    }
    (void)WorldTryMoveInto(world, x, y, x - direction, y, false);
}

static void WorldUpdateGas(World *world, int x, int y, int direction, bool smoke)
{
    Cell *cell = WorldCell(world, x, y);
    uint16_t maximumLife = smoke
                               ? (uint16_t)(150u + CoordinateHash(x, y) % 100u)
                               : 420u;

    if (cell->lifetime < UINT16_MAX) {
        ++cell->lifetime;
    }
    if (cell->lifetime >= maximumLife) {
        WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
        WorldCell(world, x, y)->updatedTick = world->tick;
        return;
    }

    WorldUpdateGasMotion(world, x, y, direction, smoke);
}

static void WorldUpdateFire(World *world, int x, int y, int direction)
{
    Cell *cell = WorldCell(world, x, y);
    uint16_t maximumLife = (uint16_t)(42u + CoordinateHash(x, y) % 48u);

    if (cell->lifetime < UINT16_MAX) {
        ++cell->lifetime;
    }
    /* One burning cell cannot ignite an unlimited chain of ordinary dirt. */
    WorldHeatNeighbors(world, x, y, FIRE_NEIGHBOR_HEAT_PER_TICK, 0.0f);

    if (cell->lifetime % 12u == 0u && WorldMaterialAt(world, x, y - 1) == MATERIAL_EMPTY) {
        WorldSetCellRaw(world, x, y - 1, MATERIAL_SMOKE);
        WorldCell(world, x, y - 1)->updatedTick = world->tick;
    }

    if (cell->lifetime >= maximumLife) {
        CellMaterial residue = (CoordinateHash(x, y) + world->tick) % 4u == 0u
                                   ? MATERIAL_ASH
                                   : MATERIAL_SMOKE;
        WorldSetCellRaw(world, x, y, residue);
        WorldCell(world, x, y)->updatedTick = world->tick;
        return;
    }

    if (cell->lifetime > 8u) {
        WorldUpdateGasMotion(world, x, y, direction, true);
    }
}

static void WorldUpdateCellAt(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);
    int direction = ((CoordinateHash(x, y) + world->tick) & 1u) != 0u ? 1 : -1;
    float temperatureBefore;

    if (cell->updatedTick == world->tick) {
        return;
    }

    /* A chunk stays awake because something actually happened in it, not merely
       because it contains a dynamic or hot cell. Movement and material changes
       already wake their own neighbourhood, so a meaningful temperature change
       is the remaining case. This is what lets a settled sand pile or the
       interior of a lava lake sleep while its boundary keeps working: whatever
       later disturbs them - a drill, an explosion, a cell moving nearby - wakes
       the surrounding chunks on its way through. */
    temperatureBefore = cell->temperature;
    if (WorldUpdateTemperatureState(world, x, y)) {
        return;
    }
    if (fabsf(cell->temperature - temperatureBefore) > 0.05f) {
        WorldWakeCellAndNeighbors(world, x, y);
    }

    switch (cell->material) {
        case MATERIAL_SAND:
            WorldUpdateSand(world, x, y, direction);
            break;
        case MATERIAL_WATER:
            if (!WorldTryMaterialReaction(world, x, y)) {
                WorldUpdateLiquid(world, x, y, direction, false);
            }
            break;
        case MATERIAL_LAVA:
            if (!WorldTryMaterialReaction(world, x, y)) {
                WorldHeatNeighbors(world, x, y, LAVA_NEIGHBOR_HEAT_PER_TICK,
                                   LAVA_PASSIVE_HEAT_CAP);
                WorldBurnDirt(world, x, y);
                WorldUpdateLiquid(world, x, y, direction, true);
            }
            break;
        case MATERIAL_STEAM:
            WorldUpdateGas(world, x, y, direction, false);
            break;
        case MATERIAL_SMOKE:
            WorldUpdateGas(world, x, y, direction, true);
            break;
        case MATERIAL_FIRE:
            WorldUpdateFire(world, x, y, direction);
            break;
        case MATERIAL_ASH:
            WorldUpdateSand(world, x, y, direction);
            break;
        default:
            break;
    }
}

void WorldUpdate(World *world)
{
    size_t chunkCount;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL) {
        return;
    }

    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    world->lastTickStats = (WorldTickStats){0};
    memset(world->nextActiveChunks, 0, chunkCount * sizeof(*world->nextActiveChunks));
    world->reactionCount = 0;
    ++world->tick;
    if (world->tick == 0u) {
        world->tick = 1u;
    }

    for (chunkY = world->chunkRows - 1; chunkY >= 0; --chunkY) {
        int minimumY = chunkY * WORLD_CHUNK_SIZE;
        int maximumY = minimumY + WORLD_CHUNK_SIZE - 1;
        int y;

        if (maximumY >= world->height) maximumY = world->height - 1;
        for (y = maximumY; y >= minimumY; --y) {
            bool reverse = ((world->tick + (uint32_t)y) & 1u) != 0u;
            int chunkStart = reverse ? world->chunkColumns - 1 : 0;
            int chunkEnd = reverse ? -1 : world->chunkColumns;
            int chunkStep = reverse ? -1 : 1;
            int chunkX;

            for (chunkX = chunkStart; chunkX != chunkEnd; chunkX += chunkStep) {
                size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                    (size_t)chunkX;
                int minimumX;
                int maximumX;
                int start;
                int end;
                int step;
                int x;

                if (world->activeChunks[chunkIndex] == 0u) {
                    continue;
                }
                minimumX = chunkX * WORLD_CHUNK_SIZE;
                maximumX = minimumX + WORLD_CHUNK_SIZE;
                if (maximumX > world->width) maximumX = world->width;
                if (y == maximumY) {
                    ++world->lastTickStats.processedChunks;
                    world->lastTickStats.processedCells +=
                        (uint64_t)(maximumX - minimumX) *
                        (uint64_t)(maximumY - minimumY + 1);
                }
                start = reverse ? maximumX - 1 : minimumX;
                end = reverse ? minimumX - 1 : maximumX;
                step = reverse ? -1 : 1;

                for (x = start; x != end; x += step) {
                    WorldUpdateCellAt(world, x, y);
                }
            }
        }
    }

    {
        uint8_t *previousChunks = world->activeChunks;
        size_t chunkIndex;

        /* Mark the set that was actually simulated, not the one that will run
           next tick: a chunk that settles and goes to sleep still owes the
           texture its final frame. */
        for (chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            world->dirtyChunks[chunkIndex] |= world->activeChunks[chunkIndex];
            world->lightDirtyChunks[chunkIndex] |= world->activeChunks[chunkIndex];
        }
        world->activeChunks = world->nextActiveChunks;
        world->nextActiveChunks = previousChunks;
    }
    WorldCountActiveState(world);
}
