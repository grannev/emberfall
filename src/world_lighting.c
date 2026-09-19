/* The coarse two-channel light field. See world_lighting.h for the shape of the
 * solve and why it is a column walk plus two raster sweeps rather than a flood
 * fill.
 */
#include "world_lighting.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

#include "world_internal.h"

static int WorldLightIndex(const World *world, int lightX, int lightY)
{
    return lightY * world->lightColumns + lightX;
}

/* Rebuilds emission and opacity for one chunk's worth of light cells, and
   reports whether either actually changed.

   The report is what stops a still scene from being re-lit sixty times a
   second. Every awake chunk marks its light inputs dirty every tick, and most
   awake chunks change nothing the light can see: water sloshing in a pool is
   not opaque, a lava lake glows exactly as it did a tick ago. The solve is the
   one part of drawing that is not proportional to what changed, so it must
   only run when an input it reads has. */
static bool WorldRefreshLightBlock(World *world, int chunkX, int chunkY)
{
    int firstLightX = chunkX * WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int firstLightY = chunkY * WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lastLightX = firstLightX + WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lastLightY = firstLightY + WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    bool changed = false;
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
                const Cell *row = WorldCellConst(world, firstX, y);

                for (x = 0; x < lastX - firstX; ++x) {
                    const Cell *cell = &row[x];
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

            {
                int index = WorldLightIndex(world, lightX, lightY);
                float opacity = samples > 0 ? (float)solid / (float)samples
                                            : 0.0f;

                /* Exact comparisons: both values are computed the same way
                   from the same cells, so an unchanged block reproduces them
                   bit for bit. */
                if (world->lightEmission[index] != emission ||
                    world->lightOpacity[index] != opacity) {
                    world->lightEmission[index] = emission;
                    world->lightOpacity[index] = opacity;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

/* Sky light: fill each column from the top while it stays open. Doing this as
   a column walk rather than as propagation is what lets open air stay at full
   brightness however deep the world is, and it is also why the solve window
   costs sky nothing — a column is solved independently of its neighbours.

   Seeded at one, not at the daylight: the channel is how much of the day
   reaches a sample, and the day itself is applied where the light is drawn. */
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

static inline float WorldLightTransmission(const World *world, int index)
{
    float opacity = world->lightOpacity[index];

    return WORLD_LIGHT_OPEN_TRANSMISSION +
           (WORLD_LIGHT_SOLID_TRANSMISSION - WORLD_LIGHT_OPEN_TRANSMISSION) * opacity;
}

/* Carries both channels across one edge. They share the geometry, so solving
   them together costs far less than two separate sweeps. */
static inline void WorldSpreadLight(const float *sky, const float *ember,
                                    int sourceIndex, float transmission,
                                    float *bestSky, float *bestEmber)
{
    float spreadSky = sky[sourceIndex] * transmission;
    float spreadEmber = ember[sourceIndex] * transmission;

    if (spreadSky > *bestSky) {
        *bestSky = spreadSky;
    }
    if (spreadEmber > *bestEmber) {
        *bestEmber = spreadEmber;
    }
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
    float *sky = world->lightSky;
    float *ember = world->lightEmber;
    const int columns = world->lightColumns;
    const int rows = world->lightRows;
    int lightX;
    int lightY;
    /* Diagonal neighbours are one and a half cells away, near enough; the exact
       root of two costs a call and changes nothing visible. */
    const float diagonal = 0.87f;

    WorldSeedSky(world, firstColumn, lastColumn);
    WorldSeedEmber(world, firstColumn, lastColumn);

    for (lightY = 0; lightY < rows; ++lightY) {
        int rowIndex = lightY * columns;
        int aboveIndex = rowIndex - columns;

        for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
            int index = rowIndex + lightX;
            float transmission = WorldLightTransmission(world, index);
            float bestSky = sky[index];
            float bestEmber = ember[index];

            if (lightX > firstColumn) {
                WorldSpreadLight(sky, ember, index - 1, transmission, &bestSky,
                                 &bestEmber);
            }
            if (lightY > 0) {
                int above = aboveIndex + lightX;

                WorldSpreadLight(sky, ember, above, transmission, &bestSky,
                                 &bestEmber);
                if (lightX > firstColumn) {
                    WorldSpreadLight(sky, ember, above - 1,
                                     transmission * diagonal, &bestSky,
                                     &bestEmber);
                }
                if (lightX < lastColumn) {
                    WorldSpreadLight(sky, ember, above + 1,
                                     transmission * diagonal, &bestSky,
                                     &bestEmber);
                }
            }
            sky[index] = bestSky;
            ember[index] = bestEmber;
        }
    }

    for (lightY = rows - 1; lightY >= 0; --lightY) {
        int rowIndex = lightY * columns;
        int belowIndex = rowIndex + columns;

        for (lightX = lastColumn; lightX >= firstColumn; --lightX) {
            int index = rowIndex + lightX;
            float transmission = WorldLightTransmission(world, index);
            float bestSky = sky[index];
            float bestEmber = ember[index];

            if (lightX < lastColumn) {
                WorldSpreadLight(sky, ember, index + 1, transmission, &bestSky,
                                 &bestEmber);
            }
            if (lightY + 1 < rows) {
                int below = belowIndex + lightX;

                WorldSpreadLight(sky, ember, below, transmission, &bestSky,
                                 &bestEmber);
                if (lightX > firstColumn) {
                    WorldSpreadLight(sky, ember, below - 1,
                                     transmission * diagonal, &bestSky,
                                     &bestEmber);
                }
                if (lightX < lastColumn) {
                    WorldSpreadLight(sky, ember, below + 1,
                                     transmission * diagonal, &bestSky,
                                     &bestEmber);
                }
            }
            sky[index] = bestSky;
            ember[index] = bestEmber;
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
   chunks are already dirty.

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
                if (WorldRefreshLightBlock(world, chunkX, chunkY)) {
                    terrainChanged = true;
                }
                world->lightDirtyChunks[index] = 0u;
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

        if (!sourceMoved && !terrainSettled && !windowMoved && world->lightSolved) {
            return;
        }
        WorldSolveLight(world, firstColumn, lastColumn);

        world->solvedPointLight = world->pointLight;
        world->solvedPointLightStrength = world->pointLightStrength;
        world->solvedTick = world->tick;
        world->solvedFirstColumn = firstColumn;
        world->solvedLastColumn = lastColumn;
        world->lightSolved = true;
        ++world->lightRevision;
    }
}

void WorldSetDaylight(World *world, float daylight)
{
    if (world == NULL) {
        return;
    }
    world->daylight = daylight < 0.0f ? 0.0f : (daylight > 1.0f ? 1.0f
                                                                : daylight);
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
