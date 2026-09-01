/* The bridge from simulation state to dirty pixel regions.
 *
 * This module owns world-specific light sampling and dirty-chunk traversal,
 * then delegates material/temperature conversion to material_render so static
 * pages and detached bodies share one palette path. What it hands the renderer
 * is a plain rectangle of pixels, so the renderer stays free to decide how
 * those pixels reach the GPU.
 */
#include "world_render_data.h"

#include <stddef.h>

#include <raymath.h>

#include "material_render.h"
#include "world_internal.h"
#include "world_lighting.h"

/* Takes the light level rather than sampling it: this runs for every cell of
   every dirty chunk, and doing the bilinear lookup here — with its floor and its
   clamps — cost more than the rest of drawing put together. The caller walks a
   chunk in order and can hoist all of that out of the loop. */
static MaterialRenderSample MaterialPixel(const World *world, const Cell *cell,
                                          int x, int y, float red, float green,
                                          float blue)
{
    MaterialRenderSample sample;

    if (cell->material == MATERIAL_EMPTY) {
        /* Empty space is a depth gradient rather than a flat colour. */
        unsigned char glow = (unsigned char)(10 + (y * 10) / world->height);
        /* Keep the old cave-depth tint as a translucent veil: the procedural
           environment remains visible through air while world lighting can
           still darken tunnels and preserve foreground contrast. */
        unsigned char depthAlpha =
            (unsigned char)(150 + (y * 70) / world->height);
        Color color =
            (Color){5, glow, (unsigned char)(18 + glow), depthAlpha};

        color.r = (unsigned char)(red * (float)color.r);
        color.g = (unsigned char)(green * (float)color.g);
        color.b = (unsigned char)(blue * (float)color.b);
        sample.scene = color;
        sample.emissive = BLANK;
        return sample;
    }
    return MaterialRenderCell((CellMaterial)cell->material, cell->temperature,
                              x, y, red, green, blue);
}

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

    /* Light first: the solve is global and can dirty chunks that were only
       re-lit, so it must finish before any pixel is built. */
    WorldUpdateLighting(world, visible);

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
            int columnLow[WORLD_CHUNK_SIZE];
            int columnHigh[WORLD_CHUNK_SIZE];
            float columnBlend[WORLD_CHUNK_SIZE];
            int x;
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

            /* Bilinear light weights are the same for every row of the chunk,
               so they are resolved once here instead of per pixel. */
            for (x = minimumX; x < maximumX; ++x) {
                WorldLightAxis(world->lightColumns, x, &columnLow[x - minimumX],
                               &columnHigh[x - minimumX],
                               &columnBlend[x - minimumX]);
            }

            for (y = minimumY; y < maximumY; ++y) {
                int rowLow;
                int rowHigh;
                float rowBlend;
                const float *skyLow;
                const float *skyHigh;
                const float *emberLow;
                const float *emberHigh;

                WorldLightAxis(world->lightRows, y, &rowLow, &rowHigh, &rowBlend);
                skyLow = world->lightSky +
                         (size_t)rowLow * (size_t)world->lightColumns;
                skyHigh = world->lightSky +
                          (size_t)rowHigh * (size_t)world->lightColumns;
                emberLow = world->lightEmber +
                           (size_t)rowLow * (size_t)world->lightColumns;
                emberHigh = world->lightEmber +
                            (size_t)rowHigh * (size_t)world->lightColumns;

                for (x = minimumX; x < maximumX; ++x) {
                    int slot = x - minimumX;
                    int lowX = columnLow[slot];
                    int highX = columnHigh[slot];
                    float blend = columnBlend[slot];
                    float topSky = skyLow[lowX] +
                                   (skyLow[highX] - skyLow[lowX]) * blend;
                    float bottomSky = skyHigh[lowX] +
                                      (skyHigh[highX] - skyHigh[lowX]) * blend;
                    float topEmber = emberLow[lowX] +
                                     (emberLow[highX] - emberLow[lowX]) * blend;
                    float bottomEmber = emberHigh[lowX] +
                                        (emberHigh[highX] - emberHigh[lowX]) * blend;
                    float red;
                    float green;
                    float blue;

                    WorldLightTint(topSky + (bottomSky - topSky) * rowBlend,
                                   topEmber + (bottomEmber - topEmber) * rowBlend,
                                   &red, &green, &blue);
                    MaterialRenderSample sample =
                        MaterialPixel(world, WorldCellConst(world, x, y),
                                      x, y, red, green, blue);

                    size_t pixelIndex =
                        (size_t)(y - minimumY) *
                            (size_t)(maximumX - minimumX) +
                        (size_t)(x - minimumX);

                    uploadPixels[pixelIndex] = sample.scene;
                    emissivePixels[pixelIndex] = sample.emissive;
                }
            }
            /* At 16384 cells wide, uploading one full-width band for a local
               change moves tens of MiB. A 32x32 stack staging block keeps the
               source contiguous without any frame allocation and uploads only
               the chunk that was rebuilt. */
            if (visitor(context,
                        (Rectangle){(float)minimumX, (float)minimumY,
                                    (float)(maximumX - minimumX),
                                    (float)(maximumY - minimumY)},
                        uploadPixels, emissivePixels)) {
                world->dirtyChunks[chunkIndex] = 0u;
            }
        }
    }
}
