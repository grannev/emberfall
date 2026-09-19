/* The bridge from simulation state to dirty pixel regions.
 *
 * This module owns dirty-chunk traversal and delegates material/temperature
 * conversion to material_render so static pages and detached bodies share one
 * palette path. What it hands the renderer is a plain rectangle of unlit
 * pixels, so the renderer stays free to decide how those pixels reach the GPU
 * and how they are lit once there.
 */
#include "world_render_data.h"

#include <stddef.h>

#include <raymath.h>

#include "material_render.h"
#include "world_internal.h"

void WorldMarkRegionDirty(World *world, Rectangle region)
{
    int firstChunkX;
    int lastChunkX;
    int firstChunkY;
    int lastChunkY;
    int chunkY;

    if (world == NULL || world->dirtyChunks == NULL || region.width <= 0.0f ||
        region.height <= 0.0f) {
        return;
    }

    firstChunkX = (int)floorf(region.x / (float)WORLD_CHUNK_SIZE);
    lastChunkX = (int)floorf((region.x + region.width - 1.0f) /
                             (float)WORLD_CHUNK_SIZE);
    firstChunkY = (int)floorf(region.y / (float)WORLD_CHUNK_SIZE);
    lastChunkY = (int)floorf((region.y + region.height - 1.0f) /
                             (float)WORLD_CHUNK_SIZE);
    if (firstChunkX < 0) firstChunkX = 0;
    if (firstChunkY < 0) firstChunkY = 0;
    if (lastChunkX > world->chunkColumns - 1) lastChunkX = world->chunkColumns - 1;
    if (lastChunkY > world->chunkRows - 1) lastChunkY = world->chunkRows - 1;

    for (chunkY = firstChunkY; chunkY <= lastChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = firstChunkX; chunkX <= lastChunkX; ++chunkX) {
            world->dirtyChunks[WorldChunkIndex(world, chunkX, chunkY)] = 1u;
        }
    }
}

void WorldPrepareVisible(World *world, Rectangle visible,
                         WorldRenderChunkVisitor visitor, void *context)
{
    Color uploadPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    Color emissivePixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    int firstVisibleColumn;
    int lastVisibleColumn;
    int firstVisibleRow;
    int lastVisibleRow;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->dirtyChunks == NULL ||
        visitor == NULL) {
        return;
    }

    /* Rebuilding costs what the player can see, not what the world is doing.
       Activity is spread over the whole map — a lava lake, a distant fire, a
       collapsing sand bank — while the camera shows a small window of it, and
       rebuilding a chunk nobody is looking at buys nothing. A skipped chunk
       keeps its dirty flag and is rebuilt on the frame it scrolls into view. */
    firstVisibleColumn = (int)floorf(visible.x / (float)WORLD_CHUNK_SIZE) - 1;
    lastVisibleColumn =
        (int)floorf((visible.x + visible.width) / (float)WORLD_CHUNK_SIZE) + 1;
    firstVisibleRow = (int)floorf(visible.y / (float)WORLD_CHUNK_SIZE) - 1;
    lastVisibleRow =
        (int)floorf((visible.y + visible.height) / (float)WORLD_CHUNK_SIZE) + 1;
    if (firstVisibleColumn < 0) firstVisibleColumn = 0;
    if (firstVisibleRow < 0) firstVisibleRow = 0;
    if (lastVisibleColumn > world->chunkColumns - 1) {
        lastVisibleColumn = world->chunkColumns - 1;
    }
    if (lastVisibleRow > world->chunkRows - 1) {
        lastVisibleRow = world->chunkRows - 1;
    }

    /* Rebuild only the chunks that changed. The simulation sleeps on a settled
       world, and so must the renderer. */
    for (chunkY = firstVisibleRow; chunkY <= lastVisibleRow; ++chunkY) {
        int chunkX;

        for (chunkX = firstVisibleColumn; chunkX <= lastVisibleColumn; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int width;
            int y;

            if (world->dirtyChunks[chunkIndex] == 0u) {
                continue;
            }
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;
            width = maximumX - minimumX;

            for (y = minimumY; y < maximumY; ++y) {
                const Cell *row = WorldCellConst(world, minimumX, y);
                Color *scene = uploadPixels + (size_t)(y - minimumY) * (size_t)width;
                Color *emissive =
                    emissivePixels + (size_t)(y - minimumY) * (size_t)width;
                MaterialRenderSample air = MaterialRenderAir(y, world->height);
                int x;

                for (x = 0; x < width; ++x) {
                    const Cell *cell = &row[x];
                    MaterialRenderSample sample =
                        cell->material == MATERIAL_EMPTY
                            ? air
                            : MaterialRenderCell((CellMaterial)cell->material,
                                                 cell->temperature,
                                                 minimumX + x, y);

                    scene[x] = sample.scene;
                    emissive[x] = sample.emissive;
                }
            }
            /* At 16384 cells wide, uploading one full-width band for a local
               change moves tens of MiB. A 32x32 stack staging block keeps the
               source contiguous without any frame allocation and uploads only
               the chunk that was rebuilt. */
            if (visitor(context,
                        (Rectangle){(float)minimumX, (float)minimumY,
                                    (float)width, (float)(maximumY - minimumY)},
                        uploadPixels, emissivePixels)) {
                world->dirtyChunks[chunkIndex] = 0u;
            }
        }
    }
}
