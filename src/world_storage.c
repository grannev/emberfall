/* Cell storage and the chunk bookkeeping built on top of it.
 *
 * Owns allocation, the raw cell writes every other world module goes through,
 * and the active/dirty chunk flags. Nothing here decides what a material does;
 * it decides where a cell lives, who is allowed to be asleep, and which chunks
 * the renderer and the light solver still owe work.
 */
#include "world_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Adds one chunk to whichever schedule is currently being filled. Idempotent:
   a chunk already in that schedule is not added twice, which is what keeps the
   compact lists free of duplicates. Static on purpose — see world_internal.h. */
static void WorldScheduleChunk(World *world, int chunkX, int chunkY)
{
    uint8_t *flags;
    int32_t *counts;
    int32_t *columns;
    size_t index;

    if (chunkX < 0 || chunkX >= world->chunkColumns || chunkY < 0 ||
        chunkY >= world->chunkRows) {
        return;
    }
    /* A wake raised while a tick is running belongs to the next tick. The set
       being simulated is frozen at the start of WorldUpdate, so the iteration
       order stays fixed and a chunk cannot appear halfway through its own row.
       The cost is at most one tick of latency before a newly disturbed chunk
       runs, which no invariant depends on: `updatedTick` already guarantees one
       move per cell per tick, and the wake itself guarantees the chunk runs. */
    flags = world->simulating ? world->nextActiveChunks : world->activeChunks;
    counts = world->simulating ? world->nextRowCount : world->activeRowCount;
    columns = world->simulating ? world->nextRowColumns : world->activeRowColumns;

    index = WorldChunkIndex(world, chunkX, chunkY);
    if (flags[index] != 0u) {
        return;
    }
    flags[index] = 1u;
    columns[(size_t)chunkY * (size_t)world->chunkColumns +
            (size_t)counts[chunkY]] = (int32_t)chunkX;
    ++counts[chunkY];
    /* Keep the reported figure exact even between ticks. It used to be
       recomputed only by WorldUpdate, so a laser fired between two ticks left
       the HUD and any caller reading a stale count. */
    if (!world->simulating) {
        ++world->activeChunkCount;
    }
}

void WorldWakeCellAndNeighbors(World *world, int x, int y)
{
    int centerChunkX;
    int centerChunkY;
    int minimumChunkX;
    int maximumChunkX;
    int minimumChunkY;
    int maximumChunkY;
    int chunkY;

    if (world->activeChunks == NULL || world->nextActiveChunks == NULL ||
        !WorldInBounds(world, x, y)) {
        return;
    }

    /* A cell only ever influences its immediate neighbours, so it needs to wake
       an adjacent chunk only when it sits against that chunk's border. Waking a
       full 3x3 block from the middle of a chunk marked nine chunks - over nine
       thousand cells - for a change that could not leave one of them. */
    /* The world wraps: the chunk left of the first column is the last one,
       and the last column borders the first chunk even when the width is
       not a whole number of chunks. */
    x = WorldWrapX(world, x);
    centerChunkX = x / WORLD_CHUNK_SIZE;
    centerChunkY = y / WORLD_CHUNK_SIZE;
    minimumChunkX = centerChunkX - (x % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    maximumChunkX = centerChunkX +
                    (x % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ||
                             x == world->width - 1
                         ? 1
                         : 0);
    minimumChunkY = centerChunkY - (y % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    maximumChunkY = centerChunkY +
                    (y % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    for (chunkY = minimumChunkY; chunkY <= maximumChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = minimumChunkX; chunkX <= maximumChunkX; ++chunkX) {
            int wrappedX = WorldWrapColumn(chunkX, world->chunkColumns);
            size_t index;

            if (chunkY < 0 || chunkY >= world->chunkRows) {
                continue;
            }
            index = (size_t)chunkY * (size_t)world->chunkColumns + (size_t)wrappedX;
            WorldScheduleChunk(world, wrappedX, chunkY);
            if (world->dirtyChunks != NULL) {
                world->dirtyChunks[index] = 1u;
                world->lightDirtyChunks[index] = 1u;
            }
        }
    }
}

/* Generation writes millions of cells, but none of them has interacted yet.
   Keeping them asleep lets a huge map stream its simulation around the player;
   WorldActivateRegion wakes generated dynamics before they enter play, while
   every actual mutation still uses the ordinary local wake path. */
void WorldSetGeneratedCell(World *world, int x, int y,
                                  CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    WorldCountMaterialChange(world, x, y, (CellMaterial)cell->material, material);
    cell->material = (uint8_t)material;
    cell->temperature = MaterialInitialTemperature(material);
    /* A plant's cells carry the plant they belong to. */
    cell->lifetime = MaterialIsFlora(material) ? world->generationPlant : 0u;
    cell->effectStamp = 0;
    cell->heatHeld = 0;
    cell->shade = WorldShadeFor(x, y, material) & 63u;
}

uint8_t WorldShadeFor(int x, int y, CellMaterial material)
{
    uint32_t value = (uint32_t)x * 0x9e3779b1u ^ (uint32_t)y * 0x85ebca77u ^
                     (uint32_t)material * 0xc2b2ae3du;

    value ^= value >> 15;
    value *= 0x2c1b3c6du;
    value ^= value >> 12;
    return (uint8_t)(value & 63u);
}

uint8_t WorldGetShade(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return 0u;
    }
    return (uint8_t)WorldCellConst(world, x, y)->shade;
}

void WorldSetShade(World *world, int x, int y, uint8_t shade)
{
    Cell *cell;

    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return;
    }
    cell = WorldCell(world, x, y);
    if (cell->material == MATERIAL_EMPTY || cell->shade == (shade & 63u)) {
        return;
    }
    cell->shade = shade & 63u;
    /* Only what the cell looks like changed: the page has to be rebuilt, and
       nothing has to be simulated. */
    if (world->dirtyChunks != NULL) {
        world->dirtyChunks[WorldChunkIndex(world, WorldWrapX(world, x) / WORLD_CHUNK_SIZE,
                                           y / WORLD_CHUNK_SIZE)] = 1u;
    }
}

void WorldSetCellRaw(World *world, int x, int y, CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    WorldCountMaterialChange(world, x, y, (CellMaterial)cell->material, material);
    cell->material = (uint8_t)material;
    cell->temperature = MaterialInitialTemperature(material);
    cell->lifetime = 0;
    cell->effectStamp = 0;
    cell->heatHeld = 0;
    cell->shade = WorldShadeFor(x, y, material) & 63u;
    WorldWakeCellAndNeighbors(world, x, y);
}

bool WorldInit(World *world, int width, int height)
{
    size_t cellCount;
    size_t chunkCount;
    size_t lightCount;

    if (world == NULL || width <= 0 || height <= 0) {
        return false;
    }
    /* A malformed material table cannot produce a world anyone can play, and
       failing here names the problem instead of letting it surface later as a
       cell that quietly deletes itself. */
    if (!MaterialsValidate()) {
        return false;
    }

    memset(world, 0, sizeof(*world));
    /* Full daylight until a caller says otherwise. A world that has never been
       told the time of day is a world at noon, not a world at midnight: every
       headless test builds one and expects to be able to see it. */
    world->daylight = 1.0f;
    world->width = width;
    world->height = height;
    world->chunkColumns = (width + WORLD_CHUNK_SIZE - 1) / WORLD_CHUNK_SIZE;
    world->chunkRows = (height + WORLD_CHUNK_SIZE - 1) / WORLD_CHUNK_SIZE;
    cellCount = (size_t)width * (size_t)height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    world->cells = calloc(cellCount, sizeof(*world->cells));
    world->activeChunks = calloc(chunkCount, sizeof(*world->activeChunks));
    world->nextActiveChunks = calloc(chunkCount, sizeof(*world->nextActiveChunks));
    world->activeRowColumns = calloc(chunkCount, sizeof(*world->activeRowColumns));
    world->nextRowColumns = calloc(chunkCount, sizeof(*world->nextRowColumns));
    world->activeRowCount = calloc((size_t)world->chunkRows,
                                   sizeof(*world->activeRowCount));
    world->nextRowCount = calloc((size_t)world->chunkRows,
                                 sizeof(*world->nextRowCount));
    world->chunkWater = calloc(chunkCount, sizeof(*world->chunkWater));
    world->chunkLava = calloc(chunkCount, sizeof(*world->chunkLava));
    world->lightColumns = (width + WORLD_LIGHT_SCALE - 1) / WORLD_LIGHT_SCALE;
    world->lightRows = (height + WORLD_LIGHT_SCALE - 1) / WORLD_LIGHT_SCALE;
    lightCount = (size_t)world->lightColumns * (size_t)world->lightRows;
    world->lightSky = calloc(lightCount, sizeof(*world->lightSky));
    world->lightEmber = calloc(lightCount, sizeof(*world->lightEmber));
    world->lightEmission = calloc(lightCount, sizeof(*world->lightEmission));
    world->lightOpacity = calloc(lightCount, sizeof(*world->lightOpacity));
    world->lightScratch = calloc((size_t)world->lightColumns,
                                 sizeof(*world->lightScratch));
    world->backWallColumns = (width + WORLD_BACK_WALL_SCALE - 1) / WORLD_BACK_WALL_SCALE;
    world->backWallRows = (height + WORLD_BACK_WALL_SCALE - 1) / WORLD_BACK_WALL_SCALE;
    world->backWalls = calloc((size_t)world->backWallColumns * (size_t)world->backWallRows,
                              sizeof(*world->backWalls));
    world->backWallVisit = calloc((size_t)WORLD_BACK_WALL_WINDOW * WORLD_BACK_WALL_WINDOW,
                                  sizeof(*world->backWallVisit));
    world->backWallQueue = calloc((size_t)WORLD_BACK_WALL_WINDOW * WORLD_BACK_WALL_WINDOW,
                                  sizeof(*world->backWallQueue));
    world->dirtyChunks = malloc(chunkCount * sizeof(*world->dirtyChunks));
    world->lightDirtyChunks = malloc(chunkCount * sizeof(*world->lightDirtyChunks));
    if (world->dirtyChunks != NULL) {
        /* Nothing has been uploaded yet, so every chunk owes the texture a
           first full write. */
        memset(world->dirtyChunks, 1, chunkCount * sizeof(*world->dirtyChunks));
    }
    if (world->lightDirtyChunks != NULL) {
        memset(world->lightDirtyChunks, 1,
               chunkCount * sizeof(*world->lightDirtyChunks));
    }
    /* The cells are deliberately not touched here. A zeroed cell is an empty
       cell at rest — empty has no temperature — so the pages of a sky that is
       never written are never materialised, and the world costs what the
       ground in it costs. See WORLD_GROUND_ROWS. */

    if (world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL || world->activeRowColumns == NULL ||
        world->nextRowColumns == NULL || world->activeRowCount == NULL ||
        world->nextRowCount == NULL || world->chunkWater == NULL ||
        world->chunkLava == NULL || world->dirtyChunks == NULL ||
        world->lightDirtyChunks == NULL || world->backWalls == NULL ||
        world->backWallVisit == NULL || world->backWallQueue == NULL ||
        world->lightSky == NULL || world->lightEmber == NULL ||
        world->lightEmission == NULL || world->lightOpacity == NULL ||
        world->lightScratch == NULL) {
        WorldUnload(world);
        return false;
    }

    return true;
}

void WorldUnload(World *world)
{
    if (world == NULL) {
        return;
    }

    free(world->cells);
    free(world->activeChunks);
    free(world->nextActiveChunks);
    free(world->activeRowColumns);
    free(world->nextRowColumns);
    free(world->activeRowCount);
    free(world->nextRowCount);
    free(world->chunkWater);
    free(world->chunkLava);
    free(world->dirtyChunks);
    free(world->lightDirtyChunks);
    free(world->backWalls);
    free(world->backWallVisit);
    free(world->backWallQueue);
    free(world->lightSky);
    free(world->lightEmber);
    free(world->lightEmission);
    free(world->lightOpacity);
    free(world->lightScratch);
    free(world->lightWindow);
    memset(world, 0, sizeof(*world));
}

void WorldCountActiveState(World *world)
{
    int chunkY;

    world->activeChunkCount = 0;
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        world->activeChunkCount += (int)world->activeRowCount[chunkY];
    }
}

void WorldActivateRegion(World *world, Rectangle region)
{
    int firstChunkX;
    int lastChunkX;
    int firstChunkY;
    int lastChunkY;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL || region.width <= 0.0f ||
        region.height <= 0.0f) {
        return;
    }

    firstChunkX = (int)floorf(region.x / (float)WORLD_CHUNK_SIZE);
    lastChunkX = (int)floorf((region.x + region.width - 0.001f) /
                            (float)WORLD_CHUNK_SIZE);
    firstChunkY = (int)floorf(region.y / (float)WORLD_CHUNK_SIZE);
    lastChunkY = (int)floorf((region.y + region.height - 0.001f) /
                            (float)WORLD_CHUNK_SIZE);
    /* Columns wrap and may run past either end; a region wider than the
       world is the whole world, once. */
    if (lastChunkX - firstChunkX + 1 > world->chunkColumns) {
        firstChunkX = 0;
        lastChunkX = world->chunkColumns - 1;
    }
    if (firstChunkY < 0) firstChunkY = 0;
    if (lastChunkY >= world->chunkRows) lastChunkY = world->chunkRows - 1;
    if (firstChunkX > lastChunkX || firstChunkY > lastChunkY) {
        return;
    }

    for (chunkY = firstChunkY; chunkY <= lastChunkY; ++chunkY) {
        int unwrappedX;

        for (unwrappedX = firstChunkX; unwrappedX <= lastChunkX; ++unwrappedX) {
            int chunkX = WorldWrapColumn(unwrappedX, world->chunkColumns);
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int y;
            bool needsSimulation = false;

            if (world->activeChunks[chunkIndex] != 0u) {
                continue;
            }
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;

            for (y = minimumY; y < maximumY && !needsSimulation; ++y) {
                int x;

                for (x = minimumX; x < maximumX; ++x) {
                    const Cell *cell = WorldCellConst(world, x, y);
                    CellMaterial material = (CellMaterial)cell->material;

                    /* Empty has no temperature to be away from. */
                    if (material == MATERIAL_EMPTY) {
                        continue;
                    }
                    if (MaterialIsDynamic(material) ||
                        fabsf(cell->temperature -
                              MaterialInitialTemperature(material)) > 0.05f) {
                        needsSimulation = true;
                        break;
                    }
                }
            }
            if (needsSimulation) {
                WorldScheduleChunk(world, chunkX, chunkY);
            }
        }
    }
    WorldCountActiveState(world);
}

/* Walking cells to produce one debug number is not worth doing every tick, so
   the exact count is computed only when something actually asks for it. */
int WorldCountDynamicCells(const World *world)
{
    int count = 0;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL) {
        return 0;
    }

    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int slot;

        for (slot = 0; slot < (int)world->activeRowCount[chunkY]; ++slot) {
            int chunkX = (int)world->activeRowColumns[(size_t)chunkY *
                                                          (size_t)world->chunkColumns +
                                                      (size_t)slot];
            int minimumX = chunkX * WORLD_CHUNK_SIZE;
            int maximumX = minimumX + WORLD_CHUNK_SIZE;
            int minimumY = chunkY * WORLD_CHUNK_SIZE;
            int maximumY = minimumY + WORLD_CHUNK_SIZE;
            int y;

            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;

            for (y = minimumY; y < maximumY; ++y) {
                int x;

                for (x = minimumX; x < maximumX; ++x) {
                    if (MaterialIsDynamic(WorldMaterialAt(world, x, y))) {
                        ++count;
                    }
                }
            }
        }
    }
    return count;
}

int WorldChunkMaterialCount(const World *world, int chunkX, int chunkY,
                            CellMaterial material)
{
    if (world == NULL || world->chunkWater == NULL || chunkX < 0 || chunkY < 0 ||
        chunkX >= world->chunkColumns || chunkY >= world->chunkRows) {
        return 0;
    }
    if (material == MATERIAL_WATER) {
        return world->chunkWater[WorldChunkIndex(world, chunkX, chunkY)];
    }
    if (material == MATERIAL_LAVA) {
        return world->chunkLava[WorldChunkIndex(world, chunkX, chunkY)];
    }
    return 0;
}

CellMaterial WorldGetCell(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return MATERIAL_ROCK;
    }
    return WorldCellConst(world, x, y)->material;
}

float WorldGetTemperature(const World *world, int x, int y)
{
    const Cell *cell;

    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return AMBIENT_TEMPERATURE;
    }
    cell = WorldCellConst(world, x, y);
    return cell->material == MATERIAL_EMPTY ? AMBIENT_TEMPERATURE
                                            : cell->temperature;
}

void WorldSetTemperature(World *world, int x, int y, float temperature)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return;
    }

    WorldCell(world, x, y)->temperature = temperature;
    WorldWakeCellAndNeighbors(world, x, y);
}

void WorldSetCell(World *world, int x, int y, CellMaterial material)
{
    if (world == NULL || world->cells == NULL) {
        return;
    }
    WorldSetCellRaw(world, x, y, material);
}

/* Two regions are merged when their one-cell-expanded boxes meet. The extra
   cell matters: cuts that only touch corner to corner still describe one piece
   of damage, and treating them as two would run the detach check twice over
   almost the same ground. */
static bool WorldDestructionRegionsMeet(const WorldDestructionRegion *a,
                                        const WorldDestructionRegion *b)
{
    int unionWidth;
    int unionHeight;

    if (a->minimumX > b->maximumX + 1 || b->minimumX > a->maximumX + 1 ||
        a->minimumY > b->maximumY + 1 || b->minimumY > a->maximumY + 1) {
        return false;
    }
    /* Touching is not enough on its own: a merge that outgrows the span limit
       produces a box no search window can cover, which would lose both cuts
       instead of keeping two entries that each work. */
    unionWidth = (a->maximumX > b->maximumX ? a->maximumX : b->maximumX) -
                 (a->minimumX < b->minimumX ? a->minimumX : b->minimumX) + 1;
    unionHeight = (a->maximumY > b->maximumY ? a->maximumY : b->maximumY) -
                  (a->minimumY < b->minimumY ? a->minimumY : b->minimumY) + 1;
    return unionWidth <= WORLD_DESTRUCTION_MAX_SPAN &&
           unionHeight <= WORLD_DESTRUCTION_MAX_SPAN;
}

static void WorldDestructionAbsorb(WorldDestructionRegion *into,
                                   const WorldDestructionRegion *from)
{
    if (from->minimumX < into->minimumX) into->minimumX = from->minimumX;
    if (from->minimumY < into->minimumY) into->minimumY = from->minimumY;
    if (from->maximumX > into->maximumX) into->maximumX = from->maximumX;
    if (from->maximumY > into->maximumY) into->maximumY = from->maximumY;
}

void WorldRecordDestruction(World *world, int minimumX, int minimumY,
                            int maximumX, int maximumY)
{
    WorldDestructionRegion region;
    int index;

    if (world == NULL || world->cells == NULL || maximumX < minimumX ||
        maximumY < minimumY) {
        return;
    }
    /* Clipped to the world's rows here rather than at every call site: above
       and below the world reads as immovable rock, so a region reaching
       there describes damage that cannot exist. Columns are not clipped —
       the world wraps, and a region is kept in the coordinates it was cut
       in, which is the space around the player that every consumer of the
       log works in. */
    region.minimumX = minimumX;
    region.minimumY = minimumY < 0 ? 0 : minimumY;
    region.maximumX = maximumX;
    region.maximumY = maximumY > world->height - 1 ? world->height - 1 : maximumY;
    if (region.minimumX > region.maximumX || region.minimumY > region.maximumY) {
        return;
    }

    /* Absorb into the first entry this touches, then keep absorbing: growing an
       entry can bring it into contact with later ones, and leaving those
       separate would search almost the same cells twice. */
    for (index = 0; index < world->destructionCount; ++index) {
        WorldDestructionRegion *existing = &world->destruction[index];
        int other;

        if (!WorldDestructionRegionsMeet(existing, &region)) {
            continue;
        }
        WorldDestructionAbsorb(existing, &region);
        for (other = world->destructionCount - 1; other > index; --other) {
            if (!WorldDestructionRegionsMeet(existing, &world->destruction[other])) {
                continue;
            }
            WorldDestructionAbsorb(existing, &world->destruction[other]);
            world->destruction[other] = world->destruction[world->destructionCount - 1];
            --world->destructionCount;
        }
        return;
    }

    if (world->destructionCount >= MAX_WORLD_DESTRUCTION_REGIONS) {
        /* Refusing to look, not losing a mutation: the cells are already gone
           and the world is correct. Some terrain that could have come loose
           simply stays static until the next destructive event near it. */
        ++world->destructionDropped;
        return;
    }
    world->destruction[world->destructionCount++] = region;
}

void WorldClearDestruction(World *world)
{
    if (world == NULL) {
        return;
    }
    world->destructionCount = 0;
}

CellMaterial WorldGetBackWall(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL) {
        return MATERIAL_EMPTY;
    }
    return WorldBackWallAt(world, x, y);
}

uint16_t WorldGetPlant(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return 0u;
    }
    return MaterialIsFlora((CellMaterial)WorldCellConst(world, x, y)->material)
               ? WorldCellConst(world, x, y)->lifetime
               : 0u;
}
