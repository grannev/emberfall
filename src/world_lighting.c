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

/* Sky light: a column is open from the top down to the first block that is
   mostly solid, and open air is seeded at one. Walked row by row rather than
   column by column — the field is row-major, and a column walk touched a new
   cache line for every sample — using the row above as the record of whether
   the column is still open: at seed time it holds nothing but the seed.

   Seeded at one, not at the daylight: the channel is how much of the day
   reaches a sample, and the day itself is applied where the light is drawn.
   Doing it per column rather than by propagation is what lets open air stay
   at full brightness however deep the world is, and why the solve window
   costs the sky nothing: a column is seeded independently of its neighbours. */
static void WorldSeedSky(World *world, int firstColumn, int lastColumn)
{
    const int columns = world->lightColumns;
    float *sky = world->lightSky;
    const float *opacity = world->lightOpacity;
    int lightY;
    int lightX;

    for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
        sky[lightX] = opacity[lightX] > 0.35f ? 0.0f : 1.0f;
    }
    for (lightY = 1; lightY < world->lightRows; ++lightY) {
        const float *above = sky + (size_t)(lightY - 1) * (size_t)columns;
        const float *blocks = opacity + (size_t)lightY * (size_t)columns;
        float *row = sky + (size_t)lightY * (size_t)columns;

        for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
            row[lightX] = blocks[lightX] > 0.35f ? 0.0f : above[lightX];
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

static inline float WorldMaximum(float a, float b)
{
    return a > b ? a : b;
}

/* One row of one sweep, both channels: each row receives what its
   already-swept neighbour row `beside` carries into it, then the light runs
   along the row in `direction`.

   Written as two passes over the row rather than one because the two do
   different kinds of work. Everything that comes from the neighbouring row is
   independent from sample to sample and vectorises; what comes from the
   previous sample in the same row is a dependency chain, a decayed running
   maximum, which is one multiply and one compare per sample and runs at the
   speed of the chain rather than of the memory. The two channels are carried
   through that chain side by side on purpose: they are independent, so the
   processor overlaps them, and two chains cost little more than one. */
/* What one channel of `row` receives from the neighbouring row, straight
   across and from both diagonals. The edges of the window are peeled off so
   that the interior loop has no condition in it and vectorises. */
static void WorldReceiveRow(float *restrict row, const float *restrict beside,
                            const float *restrict transmission, int firstColumn,
                            int lastColumn, float diagonal)
{
    int lightX;

    if (firstColumn == lastColumn) {
        row[firstColumn] = WorldMaximum(
            row[firstColumn], transmission[firstColumn] * beside[firstColumn]);
        return;
    }
    row[firstColumn] = WorldMaximum(
        row[firstColumn],
        transmission[firstColumn] *
            WorldMaximum(beside[firstColumn], diagonal * beside[firstColumn + 1]));
    for (lightX = firstColumn + 1; lightX < lastColumn; ++lightX) {
        float across = WorldMaximum(
            beside[lightX],
            WorldMaximum(diagonal * beside[lightX - 1],
                         diagonal * beside[lightX + 1]));

        row[lightX] = WorldMaximum(row[lightX], transmission[lightX] * across);
    }
    row[lastColumn] = WorldMaximum(
        row[lastColumn],
        transmission[lastColumn] *
            WorldMaximum(beside[lastColumn], diagonal * beside[lastColumn - 1]));
}

static void WorldSweepRow(float *restrict sky, float *restrict ember,
                          const float *skyBeside, const float *emberBeside,
                          const float *restrict transmission, int firstColumn,
                          int lastColumn, int direction, float diagonal)
{
    int lightX;
    float carrySky = 0.0f;
    float carryEmber = 0.0f;

    if (skyBeside != NULL) {
        WorldReceiveRow(sky, skyBeside, transmission, firstColumn, lastColumn,
                        diagonal);
        WorldReceiveRow(ember, emberBeside, transmission, firstColumn, lastColumn,
                        diagonal);
    }
    if (direction > 0) {
        for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
            float through = transmission[lightX];

            carrySky = WorldMaximum(sky[lightX], carrySky * through);
            carryEmber = WorldMaximum(ember[lightX], carryEmber * through);
            sky[lightX] = carrySky;
            ember[lightX] = carryEmber;
        }
    } else {
        for (lightX = lastColumn; lightX >= firstColumn; --lightX) {
            float through = transmission[lightX];

            carrySky = WorldMaximum(sky[lightX], carrySky * through);
            carryEmber = WorldMaximum(ember[lightX], carryEmber * through);
            sky[lightX] = carrySky;
            ember[lightX] = carryEmber;
        }
    }
}

/* Two raster sweeps: forward carries light down and right, backward carries it
   up and left. Two sweeps are not an exact flood fill around a hairpin
   corridor, but they are stable, allocation-free, and close enough that the
   error is invisible at eight cells per sample.

   Both channels are carried in the same sweep. Solving them separately was
   tried — sky only changes when the terrain does, so a lamp moving every frame
   could in principle have skipped it — and measured worse: the two channels
   share the transmission lookup and the whole index calculation, and in a game
   whose core verb is digging, the terrain changes often enough that the second
   pass costs more than the skipped one saves. Digging went from 1.6 ms to
   3.2 ms per frame; flying gained 0.2 ms.

   The transmission of a row is resolved once per row into a scratch row the
   world owns, and read by both channels; that is the shared opacity lookup
   the paragraph above is about. */
static void WorldRowTransmission(const World *world, int lightY, int firstColumn,
                                 int lastColumn)
{
    const float *opacity = world->lightOpacity +
                           (size_t)lightY * (size_t)world->lightColumns;
    float *through = world->lightScratch;
    int lightX;

    for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
        through[lightX] =
            WORLD_LIGHT_OPEN_TRANSMISSION +
            (WORLD_LIGHT_SOLID_TRANSMISSION - WORLD_LIGHT_OPEN_TRANSMISSION) *
                opacity[lightX];
    }
}

static void WorldSolveLight(World *world, int firstColumn, int lastColumn)
{
    const int columns = world->lightColumns;
    const int rows = world->lightRows;
    const float *through = world->lightScratch;
    int lightY;
    /* Diagonal neighbours are one and a half cells away, near enough; the exact
       root of two costs a call and changes nothing visible. */
    const float diagonal = 0.87f;

    WorldSeedSky(world, firstColumn, lastColumn);
    WorldSeedEmber(world, firstColumn, lastColumn);

    for (lightY = 0; lightY < rows; ++lightY) {
        size_t rowOffset = (size_t)lightY * (size_t)columns;
        size_t aboveOffset = rowOffset - (size_t)columns;

        WorldRowTransmission(world, lightY, firstColumn, lastColumn);
        WorldSweepRow(world->lightSky + rowOffset, world->lightEmber + rowOffset,
                      lightY > 0 ? world->lightSky + aboveOffset : NULL,
                      lightY > 0 ? world->lightEmber + aboveOffset : NULL,
                      through, firstColumn, lastColumn, 1, diagonal);
    }

    for (lightY = rows - 1; lightY >= 0; --lightY) {
        size_t rowOffset = (size_t)lightY * (size_t)columns;
        size_t belowOffset = rowOffset + (size_t)columns;

        WorldRowTransmission(world, lightY, firstColumn, lastColumn);
        WorldSweepRow(world->lightSky + rowOffset, world->lightEmber + rowOffset,
                      lightY + 1 < rows ? world->lightSky + belowOffset : NULL,
                      lightY + 1 < rows ? world->lightEmber + belowOffset : NULL,
                      through, firstColumn, lastColumn, -1, diagonal);
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
