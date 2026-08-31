/* The bridge from simulation state to pixels.
 *
 * This is the only place that turns a Cell into a Color, and it deliberately
 * sits inside the world module rather than in the renderer: it needs the
 * material table, the light field and the dirty-chunk flags, none of which
 * should leave the world's own headers. What it hands the renderer is a plain
 * rectangle of pixels, so the renderer stays free to decide how they reach the
 * GPU.
 */
#include "world_render_data.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

#include "world_internal.h"
#include "world_lighting.h"

static unsigned char ChannelWithVariation(unsigned char base, signed char spread,
                                         int variation)
{
    int value = (int)base + variation * (int)spread / 2;

    return (unsigned char)Clamp((float)value, 0.0f, 255.0f);
}

/* Solid cells glow toward ember as they approach their own phase threshold, so
   a laser preheating rock and a freshly drilled tunnel wall are both readable. */
static Color MaterialHeatTint(Color base, const MaterialInfo *info, float temperature)
{
    float heat;

    if (temperature < 60.0f || !info->solid || !info->onHeat.enabled ||
        info->onHeat.threshold <= 60.0f) {
        return base;
    }

    /* Square root keeps the low end readable: rock melts at 720, so a linear
       ramp would hide every temperature a drill or a short laser burst leaves. */
    heat = sqrtf(Clamp((temperature - 60.0f) / (info->onHeat.threshold - 60.0f),
                       0.0f, 1.0f));
    base.r = (unsigned char)((float)base.r + (245.0f - (float)base.r) * heat);
    base.g = (unsigned char)((float)base.g + (96.0f - (float)base.g) * heat * 0.8f);
    base.b = (unsigned char)((float)base.b * (1.0f - heat * 0.75f));
    return base;
}

/* Takes the light level rather than sampling it: this runs for every cell of
   every dirty chunk, and doing the bilinear lookup here — with its floor and its
   clamps — cost more than the rest of drawing put together. The caller walks a
   chunk in order and can hoist all of that out of the loop. */
static Color MaterialPixel(const World *world, const Cell *cell, int x, int y,
                           float red, float green, float blue)
{
    const MaterialInfo *info = MaterialAt(cell->material);
    Color color = info->color;
    int variation;

    if (cell->material == MATERIAL_EMPTY) {
        /* Empty space is a depth gradient rather than a flat colour. */
        unsigned char glow = (unsigned char)(10 + (y * 10) / world->height);

        color = (Color){5, glow, (unsigned char)(18 + glow), 255};
    } else {
        variation = (int)(CoordinateHash(x, y) % 13u) - 6;
        color.r = ChannelWithVariation(color.r, info->variationR, variation);
        color.g = ChannelWithVariation(color.g, info->variationG, variation);
        color.b = ChannelWithVariation(color.b, info->variationB, variation);
        color = MaterialHeatTint(color, info, cell->temperature);

        /* An emitter lights itself; dimming lava by its own falloff would make
           the middle of a lake darker than its shore. */
        if (info->emission >= 0.999f) {
            return color;
        }
    }

    /* Alpha carries material translucency and has nothing to do with light.
       Only the warm channel can exceed the original value, so a single ceiling
       test per channel is enough and no libm clamp is needed. */
    red *= (float)color.r;
    green *= (float)color.g;
    blue *= (float)color.b;
    color.r = (unsigned char)(red > 255.0f ? 255.0f : red);
    color.g = (unsigned char)(green > 255.0f ? 255.0f : green);
    color.b = (unsigned char)(blue > 255.0f ? 255.0f : blue);
    return color;
}

void WorldPrepareVisible(World *world, Rectangle visible,
                         WorldRenderChunkVisitor visitor, void *context)
{
    Color uploadPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
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
    WorldUpdateLighting(world);

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
            world->dirtyChunks[chunkIndex] = 0u;
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
                    Color pixel = MaterialPixel(world, WorldCellConst(world, x, y),
                                                x, y, red, green, blue);

                    uploadPixels[(size_t)(y - minimumY) *
                                     (size_t)(maximumX - minimumX) +
                                 (size_t)(x - minimumX)] = pixel;
                }
            }
            /* At 16384 cells wide, uploading one full-width band for a local
               change moves tens of MiB. A 32x32 stack staging block keeps the
               source contiguous without any frame allocation and uploads only
               the chunk that was rebuilt. */
            visitor(context,
                    (Rectangle){(float)minimumX, (float)minimumY,
                                (float)(maximumX - minimumX),
                                (float)(maximumY - minimumY)},
                    uploadPixels);
        }
    }
}
