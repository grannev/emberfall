/* The coarse two-channel light field. See world_lighting.h for the shape of the
 * solve and why it is a column walk plus two raster sweeps rather than a flood
 * fill.
 */
#include "world_lighting.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

#include "world_internal.h"

/* ---- Lighting ------------------------------------------------------------
 *
 * Light is solved on a grid WORLD_LIGHT_SCALE times coarser than the cells.
 * Two derived fields feed it: `lightEmission`, how much a block of cells gives
 * off, and `lightOpacity`, how much of it is solid. Both are refreshed only for
 * dirty chunks, because terrain only changes where the simulation is awake.
 *
 * The solve itself is a seed followed by two raster sweeps. Sky light is filled
 * per column from the top and needs no iteration, which is what keeps the open
 * surface uniformly bright no matter how tall the world is; the sweeps then
 * carry that light, plus every emitter, sideways and into overhangs, attenuated
 * by whatever it passes through. Two sweeps are not an exact flood fill around
 * a hairpin corridor, but they are stable, allocation-free, and close enough
 * that the error is invisible at four cells per sample.
 */
static int WorldLightIndex(const World *world, int lightX, int lightY)
{
    return lightY * world->lightColumns + lightX;
}

/* Rebuilds emission and opacity for one chunk's worth of light cells. */
static void WorldRefreshLightBlock(World *world, int chunkX, int chunkY)
{
    int firstLightX = chunkX * WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int firstLightY = chunkY * WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lastLightX = firstLightX + WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lastLightY = firstLightY + WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lightX;
    int lightY;

    if (lastLightX > world->lightColumns) lastLightX = world->lightColumns;
    if (lastLightY > world->lightRows) lastLightY = world->lightRows;

    for (lightY = firstLightY; lightY < lastLightY; ++lightY) {
        for (lightX = firstLightX; lightX < lastLightX; ++lightX) {
            int firstX = lightX * WORLD_LIGHT_SCALE;
            int firstY = lightY * WORLD_LIGHT_SCALE;
            int lastX = firstX + WORLD_LIGHT_SCALE;
            int lastY = firstY + WORLD_LIGHT_SCALE;
            float emission = 0.0f;
            int solid = 0;
            int samples = 0;
            int x;
            int y;

            if (lastX > world->width) lastX = world->width;
            if (lastY > world->height) lastY = world->height;

            for (y = firstY; y < lastY; ++y) {
                for (x = firstX; x < lastX; ++x) {
                    const Cell *cell = WorldCellConst(world, x, y);
                    const MaterialInfo *info = MaterialAt(cell->material);
                    float heatGlow = (cell->temperature - WORLD_LIGHT_HEAT_FLOOR) /
                                     WORLD_LIGHT_HEAT_SPAN;

                    ++samples;
                    if (info->solid) {
                        ++solid;
                    }
                    /* The brightest cell in the block wins rather than the mean:
                       a single lava cell in a wall is a light source, and
                       averaging would dim it into nothing. */
                    if (info->emission > emission) {
                        emission = info->emission;
                    }
                    if (cell->material != MATERIAL_EMPTY && heatGlow > emission) {
                        emission = Clamp(heatGlow, 0.0f, 1.0f);
                    }
                }
            }

            world->lightEmission[WorldLightIndex(world, lightX, lightY)] = emission;
            world->lightOpacity[WorldLightIndex(world, lightX, lightY)] =
                samples > 0 ? (float)solid / (float)samples : 0.0f;
        }
    }
}

static void WorldSeedLight(World *world)
{
    int lightX;
    int lightY;
    size_t lightCount = (size_t)world->lightColumns * (size_t)world->lightRows;
    size_t index;

    for (index = 0; index < lightCount; ++index) {
        world->lightSky[index] = 0.0f;
        world->lightEmber[index] = world->lightEmission[index];
    }

    /* Sky light: fill each column from the top while it stays open. Doing this
       as a column walk rather than as propagation is what lets open air stay at
       full brightness however deep the world is. */
    for (lightX = 0; lightX < world->lightColumns; ++lightX) {
        for (lightY = 0; lightY < world->lightRows; ++lightY) {
            int index2 = WorldLightIndex(world, lightX, lightY);

            if (world->lightOpacity[index2] > 0.35f) {
                break;
            }
            world->lightSky[index2] = 1.0f;
        }
    }

    /* The player's own lamp is ember rather than sky: it should warm a tunnel
       the way a flare does, not read as a hole cut through to daylight. */
    if (world->pointLightStrength > 0.0f && world->pointLightRadius > 0.0f) {
        float radius = world->pointLightRadius / (float)WORLD_LIGHT_SCALE;
        int centerX = (int)(world->pointLight.x / (float)WORLD_LIGHT_SCALE);
        int centerY = (int)(world->pointLight.y / (float)WORLD_LIGHT_SCALE);
        int span = (int)ceilf(radius);

        for (lightY = centerY - span; lightY <= centerY + span; ++lightY) {
            for (lightX = centerX - span; lightX <= centerX + span; ++lightX) {
                float dx = (float)(lightX - centerX);
                float dy = (float)(lightY - centerY);
                float distance = sqrtf(dx * dx + dy * dy);
                float value;
                int index2;

                if (lightX < 0 || lightY < 0 || lightX >= world->lightColumns ||
                    lightY >= world->lightRows || distance > radius) {
                    continue;
                }
                value = world->pointLightStrength * (1.0f - distance / radius);
                index2 = WorldLightIndex(world, lightX, lightY);
                if (value > world->lightEmber[index2]) {
                    world->lightEmber[index2] = value;
                }
            }
        }
    }
}

static float WorldLightTransmission(const World *world, int index)
{
    float opacity = world->lightOpacity[index];

    return WORLD_LIGHT_OPEN_TRANSMISSION +
           (WORLD_LIGHT_SOLID_TRANSMISSION - WORLD_LIGHT_OPEN_TRANSMISSION) * opacity;
}

/* Carries both channels across one edge. They share the geometry, so solving
   them together costs far less than two separate sweeps. */
static void WorldSpreadLight(World *world, int sourceIndex, float transmission,
                             float *bestSky, float *bestEmber)
{
    float sky = world->lightSky[sourceIndex] * transmission;
    float ember = world->lightEmber[sourceIndex] * transmission;

    if (sky > *bestSky) {
        *bestSky = sky;
    }
    if (ember > *bestEmber) {
        *bestEmber = ember;
    }
}

static float WorldQuantiseLight(float value)
{
    return floorf(Clamp(value, 0.0f, 1.0f) * WORLD_LIGHT_STEPS) / WORLD_LIGHT_STEPS;
}

static void WorldSolveLight(World *world)
{
    int lightX;
    int lightY;
    /* Diagonal neighbours are one and a half cells away, near enough; the exact
       root of two costs a call and changes nothing visible. */
    const float diagonal = 0.87f;

    WorldSeedLight(world);

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        for (lightX = 0; lightX < world->lightColumns; ++lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            float transmission = WorldLightTransmission(world, index);
            float sky = world->lightSky[index];
            float ember = world->lightEmber[index];

            if (lightX > 0) {
                WorldSpreadLight(world, index - 1, transmission, &sky, &ember);
            }
            if (lightY > 0) {
                WorldSpreadLight(world, index - world->lightColumns, transmission,
                                 &sky, &ember);
                if (lightX > 0) {
                    WorldSpreadLight(world, index - world->lightColumns - 1,
                                     transmission * diagonal, &sky, &ember);
                }
                if (lightX + 1 < world->lightColumns) {
                    WorldSpreadLight(world, index - world->lightColumns + 1,
                                     transmission * diagonal, &sky, &ember);
                }
            }
            world->lightSky[index] = sky;
            world->lightEmber[index] = ember;
        }
    }

    for (lightY = world->lightRows - 1; lightY >= 0; --lightY) {
        for (lightX = world->lightColumns - 1; lightX >= 0; --lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            float transmission = WorldLightTransmission(world, index);
            float sky = world->lightSky[index];
            float ember = world->lightEmber[index];

            if (lightX + 1 < world->lightColumns) {
                WorldSpreadLight(world, index + 1, transmission, &sky, &ember);
            }
            if (lightY + 1 < world->lightRows) {
                WorldSpreadLight(world, index + world->lightColumns, transmission,
                                 &sky, &ember);
                if (lightX > 0) {
                    WorldSpreadLight(world, index + world->lightColumns - 1,
                                     transmission * diagonal, &sky, &ember);
                }
                if (lightX + 1 < world->lightColumns) {
                    WorldSpreadLight(world, index + world->lightColumns + 1,
                                     transmission * diagonal, &sky, &ember);
                }
            }
            world->lightSky[index] = WorldQuantiseLight(sky);
            world->lightEmber[index] = WorldQuantiseLight(ember);
        }
    }
}

/* A chunk whose light moved owes the texture a rebuild even though none of its
   cells changed. Without this the incremental renderer would show stale
   lighting: carving a shaft would brighten nothing until something moved. */
static void WorldMarkRelitChunks(World *world)
{
    int lightX;
    int lightY;
    int perChunk = WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        for (lightX = 0; lightX < world->lightColumns; ++lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            int chunkX;
            int chunkY;

            if (world->lightSky[index] == world->lightShownSky[index] &&
                world->lightEmber[index] == world->lightShownEmber[index]) {
                continue;
            }
            world->lightShownSky[index] = world->lightSky[index];
            world->lightShownEmber[index] = world->lightEmber[index];

            /* Cells sample the light field bilinearly, so a changed sample
               reaches one light cell in every direction and can therefore fall
               into a neighbouring chunk. */
            for (chunkY = (lightY - 1) / perChunk; chunkY <= (lightY + 1) / perChunk;
                 ++chunkY) {
                for (chunkX = (lightX - 1) / perChunk;
                     chunkX <= (lightX + 1) / perChunk; ++chunkX) {
                    if (chunkX >= 0 && chunkY >= 0 && chunkX < world->chunkColumns &&
                        chunkY < world->chunkRows) {
                        world->dirtyChunks[(size_t)chunkY *
                                               (size_t)world->chunkColumns +
                                           (size_t)chunkX] = 1u;
                    }
                }
            }
        }
    }
}

static float WorldSampleLightField(const float *field, int columns, int lowX,
                                   int highX, float blendX, int lowY, int highY,
                                   float blendY)
{
    const float *low = field + (size_t)lowY * (size_t)columns;
    const float *high = field + (size_t)highY * (size_t)columns;
    float top = low[lowX] + (low[highX] - low[lowX]) * blendX;
    float bottom = high[lowX] + (high[highX] - high[lowX]) * blendX;

    return top + (bottom - top) * blendY;
}

/* Bilinear sample of the coarse field at a cell. Nearest sampling would show the
   light grid as visible blocks. */
float WorldLightAt(const World *world, int x, int y)
{
    int lowX;
    int highX;
    int lowY;
    int highY;
    float blendX;
    float blendY;
    float sky;
    float ember;

    if (world == NULL || world->lightSky == NULL) {
        return 1.0f;
    }

    WorldLightAxis(world->lightColumns, x, &lowX, &highX, &blendX);
    WorldLightAxis(world->lightRows, y, &lowY, &highY, &blendY);
    sky = WorldSampleLightField(world->lightSky, world->lightColumns, lowX, highX,
                                blendX, lowY, highY, blendY);
    ember = WorldSampleLightField(world->lightEmber, world->lightColumns, lowX,
                                  highX, blendX, lowY, highY, blendY);
    return WORLD_MINIMUM_LIGHT + (1.0f - WORLD_MINIMUM_LIGHT) * fmaxf(sky, ember);
}

/* The light only has to be re-solved when its source has actually moved far
   enough to change a sample; a light drifting inside one light cell changes
   nothing the field can represent. */
static bool WorldPointLightMoved(const World *world)
{
    return fabsf(world->pointLight.x - world->solvedPointLight.x) >=
               (float)WORLD_LIGHT_SCALE * 0.5f ||
           fabsf(world->pointLight.y - world->solvedPointLight.y) >=
               (float)WORLD_LIGHT_SCALE * 0.5f ||
           world->pointLightStrength != world->solvedPointLightStrength;
}

/* Emission and opacity follow the terrain, so they only need refreshing where
   chunks are already dirty, but the solve is global and can dirty further
   chunks that were merely re-lit.

   The solve is the one part of drawing that is not proportional to what
   changed, so it must not run on a world where nothing did. A renderer whose
   whole design is to sleep with the simulation cannot afford a few
   milliseconds of unconditional work every frame. */
void WorldUpdateLighting(World *world)
{
    bool sourceMoved;
    bool terrainChanged = false;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->lightDirtyChunks == NULL) {
        return;
    }

    sourceMoved = WorldPointLightMoved(world);
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t index = WorldChunkIndex(world, chunkX, chunkY);

            if (world->lightDirtyChunks[index] != 0u) {
                WorldRefreshLightBlock(world, chunkX, chunkY);
                world->lightDirtyChunks[index] = 0u;
                terrainChanged = true;
            }
        }
    }

    /* Terrain only changes on a fixed tick, so re-solving more often than the
       world ticks is pure waste at high frame rates. An effect that writes cells
       between ticks — a laser, a settling particle — waits at most one tick to
       be lit, which is below the threshold of notice. A moving light is the
       exception: it has to track the player smoothly. */
    if (sourceMoved || (terrainChanged && world->tick != world->solvedTick) ||
        !world->lightSolved) {
        WorldSolveLight(world);
        WorldMarkRelitChunks(world);
        world->solvedPointLight = world->pointLight;
        world->solvedPointLightStrength = world->pointLightStrength;
        world->solvedTick = world->tick;
        world->lightSolved = true;
    }
}

void WorldSetPointLight(World *world, Vector2 position, float radius, float strength)
{
    if (world == NULL) {
        return;
    }
    world->pointLight = position;
    world->pointLightRadius = radius;
    world->pointLightStrength = strength;
}
