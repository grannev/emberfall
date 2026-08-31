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

/* Sky light: fill each column from the top while it stays open. Doing this as
   a column walk rather than as propagation is what lets open air stay at full
   brightness however deep the world is, and it is also why the solve window
   costs sky nothing — a column is solved independently of its neighbours. */
static void WorldSeedSky(World *world, int firstColumn, int lastColumn)
{
    int lightX;

    for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
        int lightY;
        bool open = true;

        for (lightY = 0; lightY < world->lightRows; ++lightY) {
            int index = WorldLightIndex(world, lightX, lightY);

            if (open && world->lightOpacity[index] > 0.35f) {
                open = false;
            }
            world->lightSky[index] = open ? 1.0f : 0.0f;
        }
    }
}

/* Ember starts as whatever the material itself gives off, plus the caller's
   movable light. The player's lamp is ember rather than sky on purpose: it
   should warm a tunnel the way a flare does, not read as a hole cut through to
   daylight. */
static void WorldSeedEmber(World *world, int firstColumn, int lastColumn)
{
    int lightX;
    int lightY;

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        int index = WorldLightIndex(world, firstColumn, lightY);
        int span = lastColumn - firstColumn + 1;
        int offset;

        for (offset = 0; offset < span; ++offset) {
            world->lightEmber[index + offset] = world->lightEmission[index + offset];
        }
    }

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
                int index;

                if (lightX < firstColumn || lightY < 0 || lightX > lastColumn ||
                    lightY >= world->lightRows || distance > radius) {
                    continue;
                }
                value = world->pointLightStrength * (1.0f - distance / radius);
                index = WorldLightIndex(world, lightX, lightY);
                if (value > world->lightEmber[index]) {
                    world->lightEmber[index] = value;
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

/* Two raster sweeps: forward carries light down and right, backward carries it
   up and left. Two sweeps are not an exact flood fill around a hairpin
   corridor, but they are stable, allocation-free, and close enough that the
   error is invisible at eight cells per sample.

   Both channels are carried in the same pass. Solving them separately was
   tried — sky only changes when the terrain does, so a lamp moving every frame
   could in principle have skipped it — and measured worse: the two channels
   share the transmission lookup and the whole index calculation, and in a game
   whose core verb is digging, the terrain changes often enough that the second
   pass costs more than the skipped one saves. Digging went from 1.6 ms to
   3.2 ms per frame; flying gained 0.2 ms. */
static void WorldSolveLight(World *world, int firstColumn, int lastColumn)
{
    int lightX;
    int lightY;
    /* Diagonal neighbours are one and a half cells away, near enough; the exact
       root of two costs a call and changes nothing visible. */
    const float diagonal = 0.87f;

    WorldSeedSky(world, firstColumn, lastColumn);
    WorldSeedEmber(world, firstColumn, lastColumn);

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            float transmission = WorldLightTransmission(world, index);
            float sky = world->lightSky[index];
            float ember = world->lightEmber[index];

            if (lightX > firstColumn) {
                WorldSpreadLight(world, index - 1, transmission, &sky, &ember);
            }
            if (lightY > 0) {
                WorldSpreadLight(world, index - world->lightColumns, transmission,
                                 &sky, &ember);
                if (lightX > firstColumn) {
                    WorldSpreadLight(world, index - world->lightColumns - 1,
                                     transmission * diagonal, &sky, &ember);
                }
                if (lightX < lastColumn) {
                    WorldSpreadLight(world, index - world->lightColumns + 1,
                                     transmission * diagonal, &sky, &ember);
                }
            }
            world->lightSky[index] = sky;
            world->lightEmber[index] = ember;
        }
    }

    for (lightY = world->lightRows - 1; lightY >= 0; --lightY) {
        for (lightX = lastColumn; lightX >= firstColumn; --lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            float transmission = WorldLightTransmission(world, index);
            float sky = world->lightSky[index];
            float ember = world->lightEmber[index];

            if (lightX < lastColumn) {
                WorldSpreadLight(world, index + 1, transmission, &sky, &ember);
            }
            if (lightY + 1 < world->lightRows) {
                WorldSpreadLight(world, index + world->lightColumns, transmission,
                                 &sky, &ember);
                if (lightX > firstColumn) {
                    WorldSpreadLight(world, index + world->lightColumns - 1,
                                     transmission * diagonal, &sky, &ember);
                }
                if (lightX < lastColumn) {
                    WorldSpreadLight(world, index + world->lightColumns + 1,
                                     transmission * diagonal, &sky, &ember);
                }
            }
            /* Quantised so the renderer can compare light exactly instead of
               against a tolerance; a tolerance drifts, and a sample that moves
               less than it each frame is never rebuilt. */
            world->lightSky[index] = WorldQuantiseLight(sky);
            world->lightEmber[index] = WorldQuantiseLight(ember);
        }
    }
}

/* A chunk whose light moved owes the texture a rebuild even though none of its
   cells changed. Without this the incremental renderer would show stale
   lighting: carving a shaft would brighten nothing until something moved. */
static void WorldMarkRelitChunks(World *world, int firstColumn, int lastColumn)
{
    int lightX;
    int lightY;
    int perChunk = WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
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
void WorldUpdateLighting(World *world, Rectangle visible)
{
    bool sourceMoved;
    bool terrainChanged = false;
    int firstColumn;
    int lastColumn;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->lightDirtyChunks == NULL) {
        return;
    }

    /* The window the solve actually covers: what the camera can see, plus a
       margin wide enough that anything outside it arrives invisible. */
    firstColumn = (int)floorf(visible.x / (float)WORLD_LIGHT_SCALE) -
                  WORLD_LIGHT_WINDOW_MARGIN;
    lastColumn = (int)floorf((visible.x + visible.width) /
                             (float)WORLD_LIGHT_SCALE) +
                 WORLD_LIGHT_WINDOW_MARGIN;
    if (firstColumn < 0) firstColumn = 0;
    if (lastColumn > world->lightColumns - 1) lastColumn = world->lightColumns - 1;
    if (firstColumn > lastColumn) {
        return;
    }

    sourceMoved = WorldPointLightMoved(world);
    /* The light inputs are refreshed for every dirty chunk, not only visible
       ones: a chunk that scrolls into view later must not still be showing the
       emission and opacity of terrain that has since burned away. */
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

    {
        /* Terrain only changes on a fixed tick, so re-solving more often than
           the world ticks is pure waste at high frame rates. An effect that
           writes cells between ticks — a laser, a settling particle — waits at
           most one tick to be lit, which is below the threshold of notice. A
           moving light is the exception: it has to track the player smoothly. */
        bool terrainSettled = terrainChanged && world->tick != world->solvedTick;
        bool windowMoved = firstColumn != world->solvedFirstColumn ||
                           lastColumn != world->solvedLastColumn;

        if (!sourceMoved && !terrainSettled && !windowMoved &&
            world->lightSolved) {
            return;
        }
        WorldSolveLight(world, firstColumn, lastColumn);
        WorldMarkRelitChunks(world, firstColumn, lastColumn);

        world->solvedPointLight = world->pointLight;
        world->solvedPointLightStrength = world->pointLightStrength;
        world->solvedTick = world->tick;
        world->solvedFirstColumn = firstColumn;
        world->solvedLastColumn = lastColumn;
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
