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
    centerChunkX = x / WORLD_CHUNK_SIZE;
    centerChunkY = y / WORLD_CHUNK_SIZE;
    minimumChunkX = centerChunkX - (x % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    maximumChunkX = centerChunkX +
                    (x % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    minimumChunkY = centerChunkY - (y % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    maximumChunkY = centerChunkY +
                    (y % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    for (chunkY = minimumChunkY; chunkY <= maximumChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = minimumChunkX; chunkX <= maximumChunkX; ++chunkX) {
            size_t index;

            if (chunkX < 0 || chunkX >= world->chunkColumns ||
                chunkY < 0 || chunkY >= world->chunkRows) {
                continue;
            }
            index = (size_t)chunkY * (size_t)world->chunkColumns + (size_t)chunkX;
            world->activeChunks[index] = 1u;
            world->nextActiveChunks[index] = 1u;
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
    cell->material = (uint8_t)material;
    cell->temperature = MaterialInitialTemperature(material);
    cell->lifetime = 0;
    cell->effectStamp = 0;
}

void WorldSetCellRaw(World *world, int x, int y, CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    cell->material = (uint8_t)material;
    cell->temperature = MaterialInitialTemperature(material);
    cell->lifetime = 0;
    cell->effectStamp = 0;
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
    world->width = width;
    world->height = height;
    world->chunkColumns = (width + WORLD_CHUNK_SIZE - 1) / WORLD_CHUNK_SIZE;
    world->chunkRows = (height + WORLD_CHUNK_SIZE - 1) / WORLD_CHUNK_SIZE;
    cellCount = (size_t)width * (size_t)height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    world->cells = calloc(cellCount, sizeof(*world->cells));
    world->activeChunks = calloc(chunkCount, sizeof(*world->activeChunks));
    world->nextActiveChunks = calloc(chunkCount, sizeof(*world->nextActiveChunks));
    world->lightColumns = (width + WORLD_LIGHT_SCALE - 1) / WORLD_LIGHT_SCALE;
    world->lightRows = (height + WORLD_LIGHT_SCALE - 1) / WORLD_LIGHT_SCALE;
    lightCount = (size_t)world->lightColumns * (size_t)world->lightRows;
    world->lightSky = calloc(lightCount, sizeof(*world->lightSky));
    world->lightEmber = calloc(lightCount, sizeof(*world->lightEmber));
    world->lightShownSky = calloc(lightCount, sizeof(*world->lightShownSky));
    world->lightShownEmber = calloc(lightCount, sizeof(*world->lightShownEmber));
    world->lightEmission = calloc(lightCount, sizeof(*world->lightEmission));
    world->lightOpacity = calloc(lightCount, sizeof(*world->lightOpacity));
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
    if (world->cells != NULL) {
        size_t cellIndex;

        for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            world->cells[cellIndex].temperature = AMBIENT_TEMPERATURE;
        }
    }

    if (world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL || world->dirtyChunks == NULL ||
        world->lightDirtyChunks == NULL ||
        world->lightSky == NULL || world->lightEmber == NULL ||
        world->lightShownSky == NULL || world->lightShownEmber == NULL ||
        world->lightEmission == NULL || world->lightOpacity == NULL) {
        WorldUnload(world);
        return false;
    }

    /* The shown copies start impossible so the first draw re-lights every
       chunk. */
    {
        size_t lightIndex;

        for (lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
            world->lightShownSky[lightIndex] = -1.0f;
            world->lightShownEmber[lightIndex] = -1.0f;
        }
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
    free(world->dirtyChunks);
    free(world->lightDirtyChunks);
    free(world->lightSky);
    free(world->lightEmber);
    free(world->lightShownSky);
    free(world->lightShownEmber);
    free(world->lightEmission);
    free(world->lightOpacity);
    memset(world, 0, sizeof(*world));
}

void WorldCountActiveState(World *world)
{
    int chunkY;

    world->activeChunkCount = 0;
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;

            if (world->activeChunks[chunkIndex] != 0u) {
                ++world->activeChunkCount;
            }
        }
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
    if (firstChunkX < 0) firstChunkX = 0;
    if (firstChunkY < 0) firstChunkY = 0;
    if (lastChunkX >= world->chunkColumns) lastChunkX = world->chunkColumns - 1;
    if (lastChunkY >= world->chunkRows) lastChunkY = world->chunkRows - 1;
    if (firstChunkX > lastChunkX || firstChunkY > lastChunkY) {
        return;
    }

    for (chunkY = firstChunkY; chunkY <= lastChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = firstChunkX; chunkX <= lastChunkX; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int y;
            bool needsSimulation = false;

            if (world->activeChunks[chunkIndex] != 0u ||
                world->nextActiveChunks[chunkIndex] != 0u) {
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

                    if (MaterialIsDynamic(material) ||
                        fabsf(cell->temperature -
                              MaterialInitialTemperature(material)) > 0.05f) {
                        needsSimulation = true;
                        break;
                    }
                }
            }
            if (needsSimulation) {
                world->activeChunks[chunkIndex] = 1u;
                world->nextActiveChunks[chunkIndex] = 1u;
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
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int y;

            if (world->activeChunks[chunkIndex] == 0u) {
                continue;
            }
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
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

CellMaterial WorldGetCell(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return MATERIAL_ROCK;
    }
    return WorldCellConst(world, x, y)->material;
}

float WorldGetTemperature(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return 20.0f;
    }
    return WorldCellConst(world, x, y)->temperature;
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
