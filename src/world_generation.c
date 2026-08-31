/* World generation and the spawn query.
 *
 * Layout is expressed as fractions of the world while feature sizes stay
 * absolute, so a larger map gets more caves and pockets of the same scale
 * rather than the same layout stretched out. Everything written here goes
 * through WorldSetGeneratedCell, which leaves chunks asleep: a freshly
 * generated map has not interacted yet, and WorldActivateRegion is what streams
 * its dynamics into play around the player.
 */
#include "world_internal.h"

#include <math.h>
#include <string.h>

#include <raymath.h>

static void WorldFillEllipse(World *world, int centerX, int centerY,
                             int radiusX, int radiusY, CellMaterial material)
{
    int x;
    int y;

    for (y = centerY - radiusY; y <= centerY + radiusY; ++y) {
        for (x = centerX - radiusX; x <= centerX + radiusX; ++x) {
            float dx = (float)(x - centerX) / (float)radiusX;
            float dy = (float)(y - centerY) / (float)radiusY;

            if (dx * dx + dy * dy <= 1.0f) {
                WorldSetGeneratedCell(world, x, y, material);
            }
        }
    }
}

/* The surface is a sum of three sine octaves with randomised phase, evaluated
   per column. Keeping it a function instead of a precomputed array is what
   removes the old fixed 512-wide stack buffer and its width limit. */
typedef struct SurfaceProfile {
    float base;
    float amplitude[3];
    float frequency[3];
    float phase[3];
} SurfaceProfile;

static float RandomRange(float minimum, float maximum)
{
    return minimum + (float)GetRandomValue(0, 10000) / 10000.0f * (maximum - minimum);
}

static int SurfaceHeightAt(const SurfaceProfile *profile, int x)
{
    float height = profile->base;
    int octave;

    for (octave = 0; octave < 3; ++octave) {
        height += sinf((float)x * profile->frequency[octave] +
                       profile->phase[octave]) * profile->amplitude[octave];
    }
    return (int)height;
}

/* A rock-lined pocket holding a liquid: the shell keeps the contents from
   draining into whatever caves the generator carved next to it. */
static void WorldPlacePocket(World *world, int centerX, int centerY,
                             int radiusX, int radiusY, CellMaterial fill)
{
    WorldFillEllipse(world, centerX, centerY, radiusX + 4, radiusY + 4,
                     MATERIAL_ROCK);
    WorldFillEllipse(world, centerX, centerY, radiusX, radiusY, MATERIAL_EMPTY);
    WorldFillEllipse(world, centerX, centerY + 2, radiusX - 2, radiusY - 2, fill);
}

void WorldGenerate(World *world)
{
    SurfaceProfile profile;
    float areaRatio;
    int caveCount;
    int pocketCount;
    int bankCount;
    int pillarCount;
    int dirtDepth;
    int feature;
    int octave;
    int x;
    size_t cellIndex;
    size_t cellCount;
    size_t chunkCount;

    if (world == NULL || world->cells == NULL) {
        return;
    }

    cellCount = (size_t)world->width * (size_t)world->height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    memset(world->cells, 0, cellCount * sizeof(*world->cells));
    memset(world->activeChunks, 0, chunkCount * sizeof(*world->activeChunks));
    memset(world->nextActiveChunks, 0, chunkCount * sizeof(*world->nextActiveChunks));
    memset(world->dirtyChunks, 1, chunkCount * sizeof(*world->dirtyChunks));
    memset(world->lightDirtyChunks, 1,
           chunkCount * sizeof(*world->lightDirtyChunks));
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        world->cells[cellIndex].temperature = AMBIENT_TEMPERATURE;
    }
    world->tick = 0;
    world->effectSerial = 0;

    /* Layout is expressed as fractions of the world, but feature sizes stay
       absolute: a larger world gets more caves and pockets of the same scale,
       not the same layout stretched out. */
    profile.base = (float)world->height * 0.36f;
    profile.amplitude[0] = (float)world->height * 0.045f;
    profile.amplitude[1] = (float)world->height * 0.031f;
    profile.amplitude[2] = (float)world->height * 0.014f;
    profile.frequency[0] = RandomRange(0.008f, 0.014f);
    profile.frequency[1] = RandomRange(0.026f, 0.042f);
    profile.frequency[2] = RandomRange(0.070f, 0.110f);
    for (octave = 0; octave < 3; ++octave) {
        profile.phase[octave] = RandomRange(0.0f, 6.283f);
    }
    dirtDepth = (int)((float)world->height * 0.20f);

    for (x = 0; x < world->width; ++x) {
        int surfaceY = SurfaceHeightAt(&profile, x);
        int y;

        if (surfaceY < 1) {
            surfaceY = 1;
        }
        for (y = surfaceY; y < world->height; ++y) {
            int depth = y - surfaceY;
            CellMaterial material =
                depth > dirtDepth + (int)(CoordinateHash(x, y) % 17u)
                    ? MATERIAL_ROCK
                    : MATERIAL_DIRT;

            WorldSetGeneratedCell(world, x, y, material);
        }
    }

    areaRatio = (float)world->width * (float)world->height / (512.0f * 288.0f);
    caveCount = (int)(31.0f * areaRatio);
    pocketCount = (int)(2.0f * areaRatio);
    bankCount = (int)(0.9f * areaRatio);
    pillarCount = (int)(1.0f * areaRatio);
    if (caveCount < 1) caveCount = 1;
    if (pocketCount < 1) pocketCount = 1;
    if (bankCount < 1) bankCount = 1;
    if (pillarCount < 1) pillarCount = 1;

    /* Each cave is a short chain of overlapping ellipses, which reads as a
       hollowed-out chamber rather than the identical egg a single ellipse
       gives. */
    for (feature = 0; feature < caveCount; ++feature) {
        int centerX = GetRandomValue(20, world->width - 21);
        int centerY = GetRandomValue((int)((float)world->height * 0.48f),
                                     (int)((float)world->height * 0.91f));
        int lobes = GetRandomValue(2, 4);
        int lobe;

        for (lobe = 0; lobe < lobes; ++lobe) {
            int radiusX = GetRandomValue(8, 24);
            int radiusY = GetRandomValue(5, 13);

            WorldFillEllipse(world, centerX, centerY, radiusX, radiusY,
                             MATERIAL_EMPTY);
            centerX += GetRandomValue(-18, 18);
            centerY += GetRandomValue(-9, 9);
        }
    }

    /* Water sits above the lava band so the two only meet when the player digs
       between them. */
    for (feature = 0; feature < pocketCount; ++feature) {
        int centerX = GetRandomValue(40, world->width - 41);
        int centerY = GetRandomValue((int)((float)world->height * 0.55f),
                                     (int)((float)world->height * 0.70f));

        WorldPlacePocket(world, centerX, centerY, GetRandomValue(18, 30),
                         GetRandomValue(11, 18), MATERIAL_WATER);
    }
    for (feature = 0; feature < pocketCount; ++feature) {
        int centerX = GetRandomValue(40, world->width - 41);
        int centerY = GetRandomValue((int)((float)world->height * 0.78f),
                                     (int)((float)world->height * 0.93f));

        WorldPlacePocket(world, centerX, centerY, GetRandomValue(16, 27),
                         GetRandomValue(9, 14), MATERIAL_LAVA);
    }

    /* Loose sand banks on the surface, which immediately demonstrate falling
       cell physics wherever the player happens to start. */
    for (feature = 0; feature < bankCount; ++feature) {
        int bankWidth = GetRandomValue(48, 92);
        int bankStart = GetRandomValue(4, world->width - bankWidth - 5);
        int bankDepth = GetRandomValue(12, 24);
        int column;

        for (column = 0; column < bankWidth; ++column) {
            int worldX = bankStart + column;
            int surfaceY = SurfaceHeightAt(&profile, worldX);
            int crown = (int)(8.0f * sinf((float)column / (float)bankWidth * PI));
            int y;

            for (y = surfaceY - 1 - crown; y < surfaceY + bankDepth; ++y) {
                WorldSetGeneratedCell(world, worldX, y, MATERIAL_SAND);
            }
        }
    }

    /* Breakable dirt pillars: obvious first laser and drill targets. */
    for (feature = 0; feature < pillarCount; ++feature) {
        /* Wide and low enough to read as a standing butte. Narrow, tall columns
           looked like stray needles poking out of the skyline. One height for
           the whole pillar: rolling it per column made them ragged. */
        int pillarWidth = GetRandomValue(15, 28);
        int pillarX = GetRandomValue(4, world->width - pillarWidth - 5);
        int pillarHeight = GetRandomValue(16, 34);
        int column;

        for (column = 0; column < pillarWidth; ++column) {
            int worldX = pillarX + column;
            int surfaceY = SurfaceHeightAt(&profile, worldX);
            int y;

            for (y = surfaceY - pillarHeight; y < surfaceY + 48; ++y) {
                WorldSetGeneratedCell(world, worldX, y, MATERIAL_DIRT);
            }
        }
    }

    WorldCountActiveState(world);
}

Vector2 WorldPlayerSpawn(const World *world)
{
    int x;
    int y;

    if (world == NULL || world->cells == NULL) {
        return (Vector2){0.0f, 0.0f};
    }

    /* Drop straight down from the middle of the sky and stop short of the first
       solid cell, so the spawn is open air whatever the generator produced. */
    x = world->width / 2;
    for (y = 0; y < world->height; ++y) {
        if (MaterialIsSolid(WorldMaterialAt(world, x, y))) {
            break;
        }
    }
    y -= 10;
    if (y < 4) {
        y = 4;
    }
    return (Vector2){(float)x + 0.5f, (float)y + 0.5f};
}
