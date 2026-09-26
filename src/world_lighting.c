/* The coarse two-channel light field. See world_lighting.h for the shape of the
 * solve and why it is a column walk plus two raster sweeps rather than a flood
 * fill.
 */
#include "world_lighting.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include <raymath.h>

#include "world_internal.h"

static int WorldLightIndex(const World *world, int lightX, int lightY)
{
    return lightY * world->lightColumns + lightX;
}

/* What one solve works on: the four planes, how far apart their rows are,
   and the columns of them to solve. Straight into the world's own planes when
   the window lies inside the map; into a window-sized copy when it crosses
   the seam where the world wraps, so that the sweeps — which run along a row
   from one column to the next — can carry light across the seam as they do
   across any other column. `origin` is the world column, unwrapped, that
   column zero of the planes stands for; it is what places the lamp. */
typedef struct WorldLightView {
    float *sky;
    float *ember;
    const float *emission;
    const float *opacity;
    int stride;
    int first;
    int last;
    int rows;
    int origin;
} WorldLightView;

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
            bool enclosed;
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
                    /* Plants stand behind the world, and cast nothing on
                       it: a tree that shaded the air under its crown down
                       to the ground read as a black pillar in the sky. */
                    if (info->solid && !info->flora) {
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

                /* Air with the back layer behind it is inside the ground,
                   and air without it is out under the sky. Carried as a
                   trace of opacity far under anything that blocks, so the
                   sky seed can tell open air that lost the sun to an
                   overhang from a cave. */
                enclosed = WorldBackWallAt(world, firstX + WORLD_LIGHT_SCALE / 2,
                                           firstY + WORLD_LIGHT_SCALE / 2) !=
                           MATERIAL_EMPTY;
                if (opacity == 0.0f && enclosed) {
                    opacity = WORLD_LIGHT_ENCLOSED;
                }

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
static void WorldSeedSky(const WorldLightView *view)
{
    const int columns = view->stride;
    float *sky = view->sky;
    const float *opacity = view->opacity;
    int lightY;
    int lightX;

    for (lightX = view->first; lightX <= view->last; ++lightX) {
        sky[lightX] = opacity[lightX] > 0.35f ? 0.0f : 1.0f;
    }
    for (lightY = 1; lightY < view->rows; ++lightY) {
        const float *above = sky + (size_t)(lightY - 1) * (size_t)columns;
        const float *blocks = opacity + (size_t)lightY * (size_t)columns;
        float *row = sky + (size_t)lightY * (size_t)columns;

        for (lightX = view->first; lightX <= view->last; ++lightX) {
            if (blocks[lightX] > 0.35f) {
                row[lightX] = 0.0f;
            } else if (blocks[lightX] == 0.0f) {
                /* Open air under an overhang still has the rest of the sky
                   round it: the shade fades out below whatever cast it,
                   over some hundred cells, instead of running down to the
                   ground from an island in orbit. */
                row[lightX] = fminf(1.0f, above[lightX] + WORLD_SKY_RECOVERY);
            } else {
                row[lightX] = above[lightX];
            }
        }
    }
}

/* Ember starts as whatever the material itself gives off, plus the caller's
   movable light. The player's lamp is ember rather than sky on purpose: it
   should warm a tunnel the way a flare does, not read as a hole cut through to
   daylight. */
/* The lamp's column in the view's planes. The lamp is where the player is,
   and the player is in the unwrapped space around the camera, so its column
   is taken relative to the view's origin and then moved by whole turns of
   the world until it lands on the view — which is the copy of the world the
   player is actually standing in. */
static int WorldLampViewColumn(const World *world, const WorldLightView *view)
{
    int column = (int)floorf(world->pointLight.x / (float)WORLD_LIGHT_SCALE) -
                 view->origin;
    int middle = (view->first + view->last) / 2;

    while (column - middle > world->lightColumns / 2) {
        column -= world->lightColumns;
    }
    while (middle - column > world->lightColumns / 2) {
        column += world->lightColumns;
    }
    return column;
}

static void WorldSeedEmber(const World *world, const WorldLightView *view)
{
    int lightX;
    int lightY;

    for (lightY = 0; lightY < view->rows; ++lightY) {
        size_t index = (size_t)lightY * (size_t)view->stride + (size_t)view->first;
        int span = view->last - view->first + 1;
        int offset;

        for (offset = 0; offset < span; ++offset) {
            view->ember[index + (size_t)offset] = view->emission[index + (size_t)offset];
        }
    }

    if (world->pointLightStrength > 0.0f && world->pointLightRadius > 0.0f) {
        float radius = world->pointLightRadius / (float)WORLD_LIGHT_SCALE;
        int centerX = WorldLampViewColumn(world, view);
        int centerY = (int)(world->pointLight.y / (float)WORLD_LIGHT_SCALE);
        int span = (int)ceilf(radius);

        for (lightY = centerY - span; lightY <= centerY + span; ++lightY) {
            for (lightX = centerX - span; lightX <= centerX + span; ++lightX) {
                float dx = (float)(lightX - centerX);
                float dy = (float)(lightY - centerY);
                float distance = sqrtf(dx * dx + dy * dy);
                float value;
                size_t index;

                if (lightX < view->first || lightY < 0 || lightX > view->last ||
                    lightY >= view->rows || distance > radius) {
                    continue;
                }
                value = world->pointLightStrength * (1.0f - distance / radius);
                index = (size_t)lightY * (size_t)view->stride + (size_t)lightX;
                if (value > view->ember[index]) {
                    view->ember[index] = value;
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
static void WorldRowTransmission(const World *world, const WorldLightView *view,
                                 int lightY)
{
    const float *opacity = view->opacity + (size_t)lightY * (size_t)view->stride;
    float *through = world->lightScratch;
    int lightX;

    for (lightX = view->first; lightX <= view->last; ++lightX) {
        through[lightX] =
            WORLD_LIGHT_OPEN_TRANSMISSION +
            (WORLD_LIGHT_SOLID_TRANSMISSION - WORLD_LIGHT_OPEN_TRANSMISSION) *
                opacity[lightX];
    }
}

/* The first row of the window with anything in it for the light to act on:
   opacity, emission, or the player's lamp. Every row above it is open air,
   seeded at full sky and no ember, and the downward sweep leaves such a row
   exactly as it was seeded — so it is not swept. Most of a world four
   thousand cells tall is such rows. */
static int WorldLightTopRow(const World *world, const WorldLightView *view)
{
    const int columns = view->stride;
    int top = view->rows;
    int lightY;

    if (world->pointLightStrength > 0.0f && world->pointLightRadius > 0.0f) {
        int lampTop = (int)(world->pointLight.y / (float)WORLD_LIGHT_SCALE) -
                      (int)ceilf(world->pointLightRadius / (float)WORLD_LIGHT_SCALE);

        if (lampTop < 0) lampTop = 0;
        if (lampTop < top) top = lampTop;
    }
    for (lightY = 0; lightY < top; ++lightY) {
        const float *opacity = view->opacity + (size_t)lightY * (size_t)columns;
        const float *emission = view->emission + (size_t)lightY * (size_t)columns;
        float any = 0.0f;
        int lightX;

        for (lightX = view->first; lightX <= view->last; ++lightX) {
            any = WorldMaximum(any, WorldMaximum(opacity[lightX], emission[lightX]));
        }
        if (any > 0.0f) {
            return lightY;
        }
    }
    return top;
}

/* Below half a step of the eight-bit texture the field is uploaded into:
   ember this faint is zero on screen, and the upward sweep stops carrying it
   into empty sky once a whole row of it has fallen this far. */
#define WORLD_LIGHT_EMBER_FLOOR (0.5f / 255.0f)

static float WorldRowMaximum(const float *row, int firstColumn, int lastColumn)
{
    float maximum = 0.0f;
    int lightX;

    for (lightX = firstColumn; lightX <= lastColumn; ++lightX) {
        maximum = WorldMaximum(maximum, row[lightX]);
    }
    return maximum;
}

static void WorldSolveView(World *world, const WorldLightView *view)
{
    const int columns = view->stride;
    const int rows = view->rows;
    const float *through = world->lightScratch;
    int lightY;
    int top;
    /* Diagonal neighbours are one and a half cells away, near enough; the exact
       root of two costs a call and changes nothing visible. */
    const float diagonal = 0.87f;

    WorldSeedSky(view);
    WorldSeedEmber(world, view);
    top = WorldLightTopRow(world, view);
    world->lightStats.skippedRows = 0;

    /* Down from the first row with something in it: above it the sweep would
       write back exactly what the seed wrote. */
    for (lightY = top; lightY < rows; ++lightY) {
        size_t rowOffset = (size_t)lightY * (size_t)columns;
        size_t aboveOffset = rowOffset - (size_t)columns;

        WorldRowTransmission(world, view, lightY);
        WorldSweepRow(view->sky + rowOffset, view->ember + rowOffset,
                      lightY > 0 ? view->sky + aboveOffset : NULL,
                      lightY > 0 ? view->ember + aboveOffset : NULL,
                      through, view->first, view->last, 1, diagonal);
    }

    for (lightY = rows - 1; lightY >= 0; --lightY) {
        size_t rowOffset = (size_t)lightY * (size_t)columns;
        size_t belowOffset = rowOffset + (size_t)columns;

        WorldRowTransmission(world, view, lightY);
        WorldSweepRow(view->sky + rowOffset, view->ember + rowOffset,
                      lightY + 1 < rows ? view->sky + belowOffset : NULL,
                      lightY + 1 < rows ? view->ember + belowOffset : NULL,
                      through, view->first, view->last, -1, diagonal);
        /* Up into empty sky the ember only fades, three per cent a row; once
           a whole row of it is below what the texture can show, every row
           above would be too, and they keep the zero they were seeded with.
           Sky is already full there and nothing up the column can raise it. */
        if (lightY < top &&
            WorldRowMaximum(view->ember + rowOffset, view->first, view->last) <
                WORLD_LIGHT_EMBER_FLOOR) {
            world->lightStats.skippedRows = lightY + top;
            break;
        }
    }
    if (lightY < 0) {
        world->lightStats.skippedRows = top;
    }
}

/* Makes sure the window planes can hold `width` columns. Grows only: the
   largest window the camera has ever asked for, which is bounded by the
   world's own width. */
static bool WorldLightWindowReserve(World *world, int width)
{
    size_t needed = (size_t)width * (size_t)world->lightRows;
    float *planes;

    if (needed <= world->lightWindowCapacity) {
        return true;
    }
    planes = realloc(world->lightWindow, needed * 4u * sizeof(*planes));
    if (planes == NULL) {
        return false;
    }
    world->lightWindow = planes;
    world->lightWindowCapacity = needed;
    return true;
}

/* Solves columns `firstColumn`..`lastColumn`, which are unwrapped: either
   end may lie past the seam. */
static void WorldSolveLight(World *world, int firstColumn, int lastColumn)
{
    const int columns = world->lightColumns;
    const int rows = world->lightRows;
    WorldLightView view;
    int width = lastColumn - firstColumn + 1;
    int lightY;

    if (firstColumn >= 0 && lastColumn < columns) {
        view.sky = world->lightSky;
        view.ember = world->lightEmber;
        view.emission = world->lightEmission;
        view.opacity = world->lightOpacity;
        view.stride = columns;
        view.first = firstColumn;
        view.last = lastColumn;
        view.rows = rows;
        view.origin = 0;
        WorldSolveView(world, &view);
        return;
    }

    /* Across the seam: the window's inputs are copied into planes of its own
       width, in order, solved there, and the result copied back. */
    if (!WorldLightWindowReserve(world, width)) {
        return;
    }
    {
        size_t plane = (size_t)width * (size_t)rows;
        float *emission = world->lightWindow;
        float *opacity = emission + plane;

        view.sky = opacity + plane;
        view.ember = view.sky + plane;
        view.emission = emission;
        view.opacity = opacity;
        view.stride = width;
        view.first = 0;
        view.last = width - 1;
        view.rows = rows;
        view.origin = firstColumn;
        for (lightY = 0; lightY < rows; ++lightY) {
            size_t source = (size_t)lightY * (size_t)columns;
            size_t target = (size_t)lightY * (size_t)width;
            int offset;

            for (offset = 0; offset < width; ++offset) {
                int column = WorldWrapColumn(firstColumn + offset, columns);

                emission[target + (size_t)offset] = world->lightEmission[source + (size_t)column];
                opacity[target + (size_t)offset] = world->lightOpacity[source + (size_t)column];
            }
        }
        WorldSolveView(world, &view);
        for (lightY = 0; lightY < rows; ++lightY) {
            size_t source = (size_t)lightY * (size_t)width;
            size_t target = (size_t)lightY * (size_t)columns;
            int offset;

            for (offset = 0; offset < width; ++offset) {
                int column = WorldWrapColumn(firstColumn + offset, columns);

                world->lightSky[target + (size_t)column] = view.sky[source + (size_t)offset];
                world->lightEmber[target + (size_t)column] = view.ember[source + (size_t)offset];
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
    /* The world wraps, so the window is not clamped to it: it may run past
       either end, and the solve carries it across the seam. A window as wide
       as the world is the whole world, from column zero. */
    if (lastColumn - firstColumn + 1 >= world->lightColumns) {
        firstColumn = 0;
        lastColumn = world->lightColumns - 1;
    }
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
