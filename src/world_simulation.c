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

#include "world_fluid.h"
#include "world_thermal.h"

void WorldMoveCell(World *world, int fromX, int fromY, int toX, int toY)
{
    Cell *from = WorldCell(world, fromX, fromY);
    Cell *to = WorldCell(world, toX, toY);
    Cell moving = *from;

    /* A swap across a chunk border carries each material into the other
       chunk; within a chunk the counts do not move. */
    WorldCountMaterialChange(world, fromX, fromY, (CellMaterial)from->material,
                             (CellMaterial)to->material);
    WorldCountMaterialChange(world, toX, toY, (CellMaterial)to->material,
                             (CellMaterial)from->material);
    *from = *to;
    *to = moving;
    to->updatedTick = WorldTickStamp(world);
    from->updatedTick = WorldTickStamp(world);
    /* A liquid's head was measured where it stood and means nothing where
       it has gone; carried along, a deep cell's head arriving in a shallow
       column reads as pressure there, its neighbours take it up, and every
       lift it causes carries another. Zero is the least any neighbour can be
       offered, and the cell's next update recomputes it. Either end of the
       swap may be the liquid. */
    if (MaterialIsLiquid((CellMaterial)to->material)) {
        WorldLiquidSetHead(to, 0u);
    }
    if (MaterialIsLiquid((CellMaterial)from->material)) {
        WorldLiquidSetHead(from, 0u);
    }
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

/* One sideways run, up to `reach` cells, ending at the first cell with a drop
   under it or, when the cell is under pressure, at the furthest clear cell.

   A liquid that may only step one cell a tick does not level. A pool a hundred
   cells wide needs a hundred ticks to carry one cell of displacement from one
   end to the other, and by then more has arrived — so what stood there was a
   wedge that never flattened. Running several cells at once is what lets a
   surface find its level, which is the only thing a player reads as water.

   The run stops at a hole rather than passing over it: falling beats spreading,
   and a cell that skipped a gap would drain a pool from its middle instead of
   from its edge.

   A surface cell — one with nothing above it — is different. It always takes
   a drop, and it may slide along the top of other liquid a bounded number of
   times, which is how a grain left standing proud of a surface wanders until
   it finds somewhere lower to fall; that wander is what takes the last cell of
   slope out of a pool. What it never does is slide onto ground at its own
   height, and it does not wander forever. Both used to happen: every shoreline
   on the map sloshed one cell back and forth for the whole session, and a lake
   with a single grain sitting proud of a perfectly level surface skated that
   grain across the pool until the world was unloaded. Each kept its chunks
   awake for as long as the world existed. A cell with liquid on top of it is
   under pressure and spreads as before: that is what flattens a poured column
   and fills a tub. A cell with rock on top of it is under pressure too — it is
   the water in a pipe, and a pipe with a hole in it fills the hole from the
   side, since nothing can fall into it from above.

   The wander budget lives in the low bits of `lifetime`, beside the head the
   pressure model keeps there: a wander counts, anything that is progress — a
   drop, a fall, a spread under liquid — resets it, and the counter travels with
   the cell because WorldMoveCell swaps whole cells. A spread under rock does
   not reset it, or a cell could leave a ceiling, wander back under it and
   leave again for ever. */
static bool WorldFlowSideways(World *world, int x, int y, int direction,
                              int reach, bool pressed)
{
    Cell *cell = WorldCell(world, x, y);
    int furthest = 0;
    bool drop = false;
    int step;

    for (step = 1; step <= reach; ++step) {
        int probeX = x + direction * step;

        if (!WorldInBounds(world, probeX, y)) break;
        if (WorldMaterialAt(world, probeX, y) != MATERIAL_EMPTY) break;
        furthest = step;
        if (WorldInBounds(world, probeX, y + 1) &&
            WorldMaterialAt(world, probeX, y + 1) == MATERIAL_EMPTY) {
            drop = true;
            break;
        }
    }
    if (furthest == 0) {
        return false;
    }
    if (drop || pressed) {
        /* Pressed liquid flows toward lower pressure and nowhere else. The
           liquid on the far side of the empty stretch, if there is any, says
           which way that is: a hole between two cells goes to the one under
           more head, so a hole walks to the higher surface and the liquid,
           net, flows away from it; and a cell pressed from both ends of a
           pocket stays put instead of sweeping to one end and back for ever. */
        if (pressed && !drop &&
            !WorldFluidMayFlowToward(world, x, y, direction, furthest)) {
            return false;
        }
        if (drop || MaterialIsLiquid(WorldMaterialAt(world, x, y - 1))) {
            WorldLiquidSetWander(cell, 0u, direction);
        }
    } else {
        if (!MaterialIsLiquid(WorldMaterialAt(world, x + direction * furthest,
                                              y + 1))) {
            return false;
        }
        if (WorldLiquidWander(cell) >= WORLD_LIQUID_WANDER_LIMIT) {
            return false;
        }
        WorldLiquidSetWander(cell, WorldLiquidWander(cell) + 1u, direction);
    }
    {
        uint32_t head = WorldLiquidHead(cell);

        if (!WorldTryMoveInto(world, x, y, x + direction * furthest, y, false)) {
            return false;
        }
        /* The move cleared the cell's head, as every move does. A cell that
           stepped one cell into a hole in the liquid gets it back, less what
           one cell costs: a hole walking down a channel toward the surface
           that pushes it must not cut the head behind it, or every lift would
           wait for the pressure to cross the channel again. */
        if (furthest == 1 && pressed) {
            WorldLiquidSetHead(WorldCell(world, x + direction, y),
                               head > WORLD_LIQUID_HEAD_LOSS
                                   ? head - WORLD_LIQUID_HEAD_LOSS : 0u);
        }
    }
    return true;
}

static bool WorldLiquidFalls(World *world, int x, int y, int direction)
{
    static const int sideways[3] = {0, 1, -1};
    int attempt;

    for (attempt = 0; attempt < 3; ++attempt) {
        int targetX = x + sideways[attempt] * direction;

        if (WorldTryMoveInto(world, x, y, targetX, y + 1, false)) {
            /* Falling is progress: the cell may wander again from wherever it
               lands, and whatever head it had was measured somewhere else. It
               has moved, so it is addressed at its new home. */
            WorldCell(world, targetX, y + 1)->lifetime = 0;
            return true;
        }
    }
    return false;
}

static void WorldUpdateLiquid(World *world, int x, int y, int direction,
                              int reach, bool viscous)
{
    if (viscous && ((world->tick + (uint32_t)x + (uint32_t)y) % 3u != 0u)) {
        return;
    }

    if (WorldLiquidFalls(world, x, y, direction)) {
        return;
    }
    /* Pressure. A surface cell refreshes the column under it and may lift
       its bottom; a cell with liquid or rock over it carries its head on from
       its neighbours. Either way the head a neighbour reads from this cell is
       at most a tick old. */
    if (WorldLiquidIsSurface(world, x, y)) {
        if (WorldFluidSurfaceStep(world, x, y)) {
            return;
        }
        /* The way it went last time, and the other way only when that is
           blocked. A grain that chose afresh every tick walked a pool at
           random and spent its budget going nowhere; one that keeps going
           reaches the far end of the pool, or the grain that got there
           before it, and stops there — which is how the surplus poured in at
           one end becomes a new layer laid down from the other. */
        int last = WorldLiquidWanderDirection(WorldCell(world, x, y));

        if (WorldFlowSideways(world, x, y, last, WORLD_LIQUID_WANDER_REACH,
                              false)) {
            return;
        }
        (void)WorldFlowSideways(world, x, y, -last, WORLD_LIQUID_WANDER_REACH,
                                false);
        return;
    }
    /* A cell with rock over it is pressed only when something presses it:
       a head of at least half a cell arriving from liquid that stands higher
       somewhere. With less it is water lying in a pipe, and water lying in a
       pipe beside a pocket of air lies still — run as a pressed cell, it
       swept to the far end of the pocket and back every tick for ever. Read
       from the update, not from the stored value, so the head is this
       tick's. A cell with liquid over it is pressed by that liquid and keeps
       no head of its own. */
    if (!MaterialIsLiquid(WorldMaterialAt(world, x, y - 1)) &&
        WorldFluidUpdateHead(world, x, y) < WORLD_LIQUID_HEAD_PER_CELL / 2u) {
        int last = WorldLiquidWanderDirection(WorldCell(world, x, y));

        if (WorldFlowSideways(world, x, y, last, WORLD_LIQUID_WANDER_REACH,
                              false)) {
            return;
        }
        (void)WorldFlowSideways(world, x, y, -last, WORLD_LIQUID_WANDER_REACH,
                                false);
        return;
    }
    if (WorldFlowSideways(world, x, y, direction, reach, true)) {
        return;
    }
    (void)WorldFlowSideways(world, x, y, -direction, reach, true);
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
        WorldCell(world, x, y)->updatedTick = WorldTickStamp(world);
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
        WorldCell(world, x, y - 1)->updatedTick = WorldTickStamp(world);
    }

    if (cell->lifetime >= maximumLife) {
        CellMaterial residue = (CoordinateHash(x, y) + world->tick) % 4u == 0u
                                   ? MATERIAL_ASH
                                   : MATERIAL_SMOKE;
        WorldSetCellRaw(world, x, y, residue);
        WorldCell(world, x, y)->updatedTick = WorldTickStamp(world);
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

    if (cell->updatedTick == WorldTickStamp(world)) {
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
                WorldUpdateLiquid(world, x, y, direction,
                                  WORLD_WATER_DISPERSION, false);
            }
            break;
        case MATERIAL_LAVA:
            if (!WorldTryMaterialReaction(world, x, y)) {
                WorldHeatNeighbors(world, x, y, LAVA_NEIGHBOR_HEAT_PER_TICK,
                                   LAVA_PASSIVE_HEAT_CAP);
                WorldBurnDirt(world, x, y);
                WorldUpdateLiquid(world, x, y, direction,
                                  WORLD_LAVA_DISPERSION, true);
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
        case MATERIAL_RUBBLE:
            WorldUpdateSand(world, x, y, direction);
            break;
        default:
            break;
    }
}

/* Insertion sort of one chunk row's active columns. Wakes append in whatever
   order they happened, but the traversal order is part of the simulation's
   contract — a row is walked left to right or right to left, never in the order
   the wakes arrived — so the row is put back in ascending order first. The
   arrays are tens of entries long even in the busiest scenes, which is exactly
   where insertion sort is the right choice. */
static void WorldSortRow(int32_t *columns, int count)
{
    int i;

    for (i = 1; i < count; ++i) {
        int32_t value = columns[i];
        int j = i - 1;

        while (j >= 0 && columns[j] > value) {
            columns[j + 1] = columns[j];
            --j;
        }
        columns[j + 1] = value;
    }
}

void WorldUpdate(World *world)
{
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL) {
        return;
    }

    world->lastTickStats = (WorldTickStats){0};
    world->reactionCount = 0;
    world->fluid.lifts = 0;
    world->fluid.headChanges = 0;
    ++world->tick;
    /* Cells hold the low sixteen bits of this counter, and an unwritten cell
       holds zero, so the truncated value must never be zero — otherwise every
       never-updated cell in an awake chunk would read as already handled and
       skip a tick together, once every 65 536 ticks. */
    if (WorldTickStamp(world) == 0u) {
        ++world->tick;
    }
    /* From here to the swap, `activeChunks` is the frozen schedule and every
       wake lands in `nextActiveChunks` instead. */
    world->simulating = true;
    /* Pushed liquid moves before the traversal, so a cell an impulse carried
       is stamped and does not also flow this tick. */
    WorldFluidStepImpulses(world);
    WorldFrostStep(world);

    for (chunkY = world->chunkRows - 1; chunkY >= 0; --chunkY) {
        int minimumY = chunkY * WORLD_CHUNK_SIZE;
        int maximumY = minimumY + WORLD_CHUNK_SIZE - 1;
        int activeInRow = (int)world->activeRowCount[chunkY];
        int32_t *rowColumns = world->activeRowColumns +
                              (size_t)chunkY * (size_t)world->chunkColumns;
        int y;

        if (activeInRow == 0) {
            continue;
        }
        WorldSortRow(rowColumns, activeInRow);
        if (maximumY >= world->height) maximumY = world->height - 1;
        for (y = maximumY; y >= minimumY; --y) {
            bool reverse = ((world->tick + (uint32_t)y) & 1u) != 0u;
            int slotStart = reverse ? activeInRow - 1 : 0;
            int slotEnd = reverse ? -1 : activeInRow;
            int slotStep = reverse ? -1 : 1;
            int slot;

            for (slot = slotStart; slot != slotEnd; slot += slotStep) {
                int chunkX = (int)rowColumns[slot];
                int minimumX = chunkX * WORLD_CHUNK_SIZE;
                int maximumX = minimumX + WORLD_CHUNK_SIZE;
                int start;
                int end;
                int step;
                int x;

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
    world->simulating = false;

    {
        uint8_t *previousChunks = world->activeChunks;
        int32_t *previousColumns = world->activeRowColumns;
        int32_t *previousCount = world->activeRowCount;

        /* Mark the set that was actually simulated, not the one that will run
           next tick: a chunk that settles and goes to sleep still owes the
           texture its final frame. Walking the schedule rather than the whole
           chunk grid is the point of the lists — a settled world touches
           nothing here at all. */
        for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
            int slot;

            for (slot = 0; slot < (int)previousCount[chunkY]; ++slot) {
                size_t chunkIndex =
                    WorldChunkIndex(world,
                                    (int)previousColumns[(size_t)chunkY *
                                                             (size_t)world->chunkColumns +
                                                         (size_t)slot],
                                    chunkY);

                world->dirtyChunks[chunkIndex] = 1u;
                world->lightDirtyChunks[chunkIndex] = 1u;
                /* Clearing as we go leaves the retired buffer empty and ready
                   to be filled by the next tick, with no full-array memset. */
                previousChunks[chunkIndex] = 0u;
            }
            previousCount[chunkY] = 0;
        }

        world->activeChunks = world->nextActiveChunks;
        world->activeRowColumns = world->nextRowColumns;
        world->activeRowCount = world->nextRowCount;
        world->nextActiveChunks = previousChunks;
        world->nextRowColumns = previousColumns;
        world->nextRowCount = previousCount;
    }
    WorldCountActiveState(world);
}
