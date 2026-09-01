/* Deterministic biome composition for WorldGenerate.
 *
 * Every decision is derived from the world seed and coordinates. The current
 * store is still filled eagerly, but the generator does not depend on the
 * order in which distant features are visited; this is the contract a future
 * chunk generator will need.
 */
#include "world_internal.h"

#include <raymath.h>

#define BIOME_REGION_WIDTH 1536
#define BIOME_BLEND_WIDTH 384
/* A boundary that runs dead straight down a fixed multiple of the region width
   is the one thing in the landscape that could only have been drawn by a
   program, and it is legible from a long way off. Warping the coordinate before
   it is divided into regions bends every boundary without any of the code below
   having to know that boundaries are not straight.

   The amplitude is far smaller than the wavelength, so the warp changes by well
   under one cell per cell and the warped coordinate still increases: a column
   never falls back into a region it has already left, and a boundary is crossed
   once rather than flickered across. */
#define BIOME_BOUNDARY_WARP 300
#define BIOME_WARP_WAVELENGTH 1100
/* Lattice of the field that decides, cell by cell, which of two blending biomes
   a cell belongs to. Large enough that the two materials interlock in fingers
   the eye reads as one ground giving way to another, small enough that a finger
   fits inside the blend band several times over. */
#define BIOME_BLEND_PATCH 22
/* Lattice rows the blend field is sampled at, which is the world's height in
   patches plus the row past the bottom. Sized for a world far taller than the
   production one; a taller world still generates correctly, it just stops
   interleaving below the last row this covers. */
#define BIOME_BLEND_ROWS_MAX 96
#define CAVE_FEATURE_SPACING 64
#define HYDROLOGY_FEATURE_SPACING 256
#define SURFACE_FEATURE_SPACING 512
#define SPAWN_PLATEAU_INNER 48
#define SPAWN_PLATEAU_OUTER 144
#define SPAWN_FEATURE_CLEARANCE 176

#define WORLD_RNG_STREAM_TERRAIN 1u

enum GenerationChannel {
    GENERATION_BIOME_ORDER = 1,
    GENERATION_CONTINENT = 2,
    GENERATION_HILLS = 3,
    GENERATION_DETAIL = 4,
    GENERATION_STRATA = 5,
    GENERATION_CAVES = 7,
    GENERATION_HYDROLOGY = 8,
    GENERATION_SURFACE_FEATURES = 9,
    GENERATION_BIOME_WARP = 10
};

typedef struct BiomeSurfaceShape {
    float baseHeight;
    float continentAmplitude;
    float hillAmplitude;
    float detailAmplitude;
} BiomeSurfaceShape;

typedef struct BiomeSample {
    WorldBiome first;
    WorldBiome second;
    float mix;
} BiomeSample;

static const BiomeSurfaceShape BIOME_SURFACES[WORLD_BIOME_COUNT] = {
    [WORLD_BIOME_TEMPERATE] = {0.38f, 0.042f, 0.030f, 0.008f},
    [WORLD_BIOME_DUNES] = {0.40f, 0.030f, 0.044f, 0.012f},
    [WORLD_BIOME_FROST] = {0.34f, 0.052f, 0.036f, 0.010f},
    [WORLD_BIOME_VOLCANIC] = {0.31f, 0.060f, 0.060f, 0.018f},
};

_Static_assert(sizeof(BIOME_SURFACES) / sizeof(BIOME_SURFACES[0]) ==
                   WORLD_BIOME_COUNT,
               "every biome needs a surface shape");

static int ClampInt(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int PositiveModulo(int value, int modulus)
{
    int result = value % modulus;

    return result < 0 ? result + modulus : result;
}

static float SmoothStep(float value)
{
    value = Clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

static float LerpFloat(float first, float second, float amount)
{
    return first + (second - first) * amount;
}

/* Stateless seed+coordinate hashing means inserting one cave cannot shift the
   lakes and landmarks which happen to be generated after it. */
static uint64_t GenerationHash(uint64_t seed, int x, int y, uint64_t channel)
{
    Rng mixer;
    uint64_t mixed = RngStreamSeed(seed, WORLD_RNG_STREAM_TERRAIN);

    mixed ^= (uint64_t)(uint32_t)x * 0x9e3779b185ebca87ull;
    mixed ^= (uint64_t)(uint32_t)y * 0xc2b2ae3d27d4eb4full;
    mixed ^= channel * 0x165667b19e3779f9ull;
    RngSeed(&mixer, mixed);
    return RngNext(&mixer);
}

static float GenerationUnit(uint64_t seed, int x, int y, uint64_t channel)
{
    return (float)(GenerationHash(seed, x, y, channel) >> 40) /
           (float)(1u << 24);
}

/* The transition dither is the only seed hash evaluated per terrain cell.
   Keep it to a compact 32-bit mixer; the heavier feature hash above is for the
   much smaller population of columns and feature descriptors. */
static uint32_t GenerationPatchHash(uint64_t seed, int x, int y)
{
    uint32_t value = (uint32_t)x * 0x45d9f3bu;

    value ^= (uint32_t)y * 0x27d4eb2du;
    value ^= (uint32_t)seed;
    value ^= (uint32_t)(seed >> 32) * 0x9e3779b9u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return value;
}

static float PatchUnit(uint64_t seed, int x, int y)
{
    return (float)(GenerationPatchHash(seed, x, y) >> 8) / (float)(1u << 24);
}

static Rng GenerationFeatureRng(uint64_t seed, int feature,
                                enum GenerationChannel channel)
{
    Rng rng;

    RngSeed(&rng, GenerationHash(seed, feature, 0, (uint64_t)channel));
    return rng;
}

static float ValueNoise1D(uint64_t seed, int x, int wavelength,
                          enum GenerationChannel channel)
{
    int lattice;
    int remainder;
    float amount;
    float first;
    float second;

    if (wavelength <= 0) return 0.0f;
    if (x < 0) x = 0;
    lattice = x / wavelength;
    remainder = x % wavelength;
    amount = SmoothStep((float)remainder / (float)wavelength);
    first = GenerationUnit(seed, lattice, 0, (uint64_t)channel) * 2.0f - 1.0f;
    second = GenerationUnit(seed, lattice + 1, 0, (uint64_t)channel) * 2.0f -
             1.0f;
    return LerpFloat(first, second, amount);
}

/* Bends a column sideways before it is turned into a region index, so that the
   seam between two biomes meanders instead of running straight down the map. */
static int BiomeWarpedX(const World *world, int x)
{
    float warp = ValueNoise1D(world->seed, x, BIOME_WARP_WAVELENGTH,
                              GENERATION_BIOME_WARP) *
                 (float)BIOME_BOUNDARY_WARP;

    return ClampInt(x + (int)warp, 0, world->width - 1);
}

/* The contour that separates two blending biomes: smooth value noise on a coarse
   lattice.

   White noise compared against the blend amount — which is what this replaced —
   interleaves the two materials correctly on average and still looks wrong:
   every cell decides alone, so the band between two biomes is television static
   rather than terrain. A smooth field makes neighbouring cells agree, so the
   same average produces connected patches that grow as the blend advances, and
   a little per-cell hash is mixed back in to keep the edge of a patch ragged
   instead of a clean curve.

   Half the world now lies inside a blend band, so this is evaluated for millions
   of cells and its cost is the generator's. The lattice is therefore resolved
   once per column — the four corners of a patch only change every twenty-two
   cells, and sampling them per cell cost more than everything else the generator
   does put together. */
typedef struct BiomeBlendColumn {
    int rows;
    float lattice[BIOME_BLEND_ROWS_MAX];
} BiomeBlendColumn;

static void BiomeBlendColumnInit(BiomeBlendColumn *column, const World *world,
                                 int x)
{
    int latticeX = x / BIOME_BLEND_PATCH;
    float alongX = SmoothStep((float)(x % BIOME_BLEND_PATCH) /
                              (float)BIOME_BLEND_PATCH);
    int rows = world->height / BIOME_BLEND_PATCH + 2;
    int row;

    if (rows > BIOME_BLEND_ROWS_MAX) rows = BIOME_BLEND_ROWS_MAX;
    column->rows = rows;
    for (row = 0; row < rows; ++row) {
        column->lattice[row] =
            LerpFloat(PatchUnit(world->seed, latticeX, row),
                      PatchUnit(world->seed, latticeX + 1, row), alongX);
    }
}

static float BiomeBlendAt(const BiomeBlendColumn *column, uint64_t seed, int x,
                          int y)
{
    int latticeY = y / BIOME_BLEND_PATCH;
    float alongY = SmoothStep((float)(y % BIOME_BLEND_PATCH) /
                              (float)BIOME_BLEND_PATCH);

    if (latticeY + 1 >= column->rows) {
        latticeY = column->rows - 2;
        alongY = 1.0f;
    }
    return LerpFloat(column->lattice[latticeY], column->lattice[latticeY + 1],
                     alongY) *
               0.82f +
           PatchUnit(seed, x, y) * 0.18f;
}

static WorldBiome BiomeForRegion(const World *world, int region)
{
    static const WorldBiome order[WORLD_BIOME_COUNT] = {
        WORLD_BIOME_TEMPERATE,
        WORLD_BIOME_DUNES,
        WORLD_BIOME_FROST,
        WORLD_BIOME_VOLCANIC,
    };
    uint64_t layout = GenerationHash(world->seed, 0, 0,
                                     GENERATION_BIOME_ORDER);
    int centerRegion = (world->width / 2) / BIOME_REGION_WIDTH;
    int relative = region - centerRegion;
    int direction = (layout & 4ull) != 0ull ? -1 : 1;
    int rotation = (int)(layout % (uint64_t)WORLD_BIOME_COUNT);
    int index = PositiveModulo(rotation + relative * direction,
                               WORLD_BIOME_COUNT);

    return order[index];
}

WorldBiome WorldBiomeAt(const World *world, int x)
{
    if (world == NULL || world->width <= 0) {
        return WORLD_BIOME_TEMPERATE;
    }
    x = ClampInt(x, 0, world->width - 1);
    return BiomeForRegion(world, BiomeWarpedX(world, x) / BIOME_REGION_WIDTH);
}

const char *WorldBiomeName(WorldBiome biome)
{
    static const char *const names[WORLD_BIOME_COUNT] = {
        [WORLD_BIOME_TEMPERATE] = "TEMPERATE BASIN",
        [WORLD_BIOME_DUNES] = "SHATTERED DUNES",
        [WORLD_BIOME_FROST] = "FROST SHELF",
        [WORLD_BIOME_VOLCANIC] = "EMBER WASTES",
    };

    if (biome < 0 || biome >= WORLD_BIOME_COUNT) return "UNKNOWN";
    return names[biome];
}

/* Regions remain cheap to query, while terrain parameters blend for 384 cells on
   either side of a boundary — half the width of a region, so a biome is a place
   the world gradually becomes rather than a place it switches to. */
static BiomeSample BiomeSampleAt(const World *world, int x)
{
    int lastRegion;
    int region;
    int localX;
    WorldBiome current;

    x = BiomeWarpedX(world, ClampInt(x, 0, world->width - 1));
    region = x / BIOME_REGION_WIDTH;
    lastRegion = (world->width - 1) / BIOME_REGION_WIDTH;
    localX = x - region * BIOME_REGION_WIDTH;
    current = BiomeForRegion(world, region);

    if (region > 0 && localX < BIOME_BLEND_WIDTH) {
        float amount = (float)(localX + BIOME_BLEND_WIDTH) /
                       (float)(BIOME_BLEND_WIDTH * 2);

        return (BiomeSample){BiomeForRegion(world, region - 1), current,
                             SmoothStep(amount)};
    }
    if (region < lastRegion &&
        localX >= BIOME_REGION_WIDTH - BIOME_BLEND_WIDTH) {
        float amount =
            (float)(localX - (BIOME_REGION_WIDTH - BIOME_BLEND_WIDTH)) /
            (float)(BIOME_BLEND_WIDTH * 2);

        return (BiomeSample){current, BiomeForRegion(world, region + 1),
                             SmoothStep(amount)};
    }
    return (BiomeSample){current, current, 0.0f};
}

static BiomeSurfaceShape BlendedSurfaceShape(const BiomeSample *sample)
{
    const BiomeSurfaceShape *first = &BIOME_SURFACES[sample->first];
    const BiomeSurfaceShape *second = &BIOME_SURFACES[sample->second];

    return (BiomeSurfaceShape){
        .baseHeight = LerpFloat(first->baseHeight, second->baseHeight,
                                sample->mix),
        .continentAmplitude =
            LerpFloat(first->continentAmplitude, second->continentAmplitude,
                      sample->mix),
        .hillAmplitude = LerpFloat(first->hillAmplitude, second->hillAmplitude,
                                   sample->mix),
        .detailAmplitude =
            LerpFloat(first->detailAmplitude, second->detailAmplitude,
                      sample->mix),
    };
}

static float SurfaceHeightRaw(const World *world, int x)
{
    BiomeSample sample = BiomeSampleAt(world, x);
    BiomeSurfaceShape shape = BlendedSurfaceShape(&sample);
    float worldHeight = (float)world->height;

    return worldHeight * shape.baseHeight +
           ValueNoise1D(world->seed, x, 1200, GENERATION_CONTINENT) *
               worldHeight * shape.continentAmplitude +
           ValueNoise1D(world->seed, x, 260, GENERATION_HILLS) * worldHeight *
               shape.hillAmplitude +
           ValueNoise1D(world->seed, x, 52, GENERATION_DETAIL) * worldHeight *
               shape.detailAmplitude;
}

static int SurfaceHeightAt(const World *world, int x)
{
    int centerX = world->width / 2;
    int distance = x - centerX;
    float height = SurfaceHeightRaw(world, x);
    int minimumY;
    int maximumY;

    if (distance < 0) distance = -distance;
    if (distance < SPAWN_PLATEAU_OUTER) {
        float amount = distance <= SPAWN_PLATEAU_INNER
                           ? 0.0f
                           : (float)(distance - SPAWN_PLATEAU_INNER) /
                                 (float)(SPAWN_PLATEAU_OUTER -
                                         SPAWN_PLATEAU_INNER);

        height = LerpFloat(SurfaceHeightRaw(world, centerX), height,
                           SmoothStep(amount));
    }

    minimumY = world->height > 16 ? 4 : 1;
    maximumY = world->height > 16 ? world->height - 8 : world->height - 1;
    return ClampInt((int)height, minimumY, maximumY);
}

static CellMaterial BiomeMaterialAt(const World *world, WorldBiome biome,
                                    uint64_t strata, int depth)
{
    int height = world->height;

    switch (biome) {
        case WORLD_BIOME_TEMPERATE: {
            int dirtDepth = height / 10 + 14 + (int)(strata % 13ull);

            return depth < dirtDepth ? MATERIAL_DIRT : MATERIAL_ROCK;
        }
        case WORLD_BIOME_DUNES: {
            int sandDepth = height / 32 + 12 + (int)(strata % 15ull);
            int drySoilDepth = sandDepth + height / 18 + 10;

            if (depth < sandDepth) return MATERIAL_SAND;
            return depth < drySoilDepth ? MATERIAL_DIRT : MATERIAL_ROCK;
        }
        case WORLD_BIOME_FROST: {
            int iceDepth = height / 144 + 4 + (int)(strata % 4ull);
            int frozenSoilDepth = iceDepth + height / 22 + 10;

            if (depth < iceDepth) return MATERIAL_ICE;
            return depth < frozenSoilDepth ? MATERIAL_DIRT : MATERIAL_ROCK;
        }
        case WORLD_BIOME_VOLCANIC:
            return MATERIAL_ROCK;
        case WORLD_BIOME_COUNT:
            break;
    }
    return MATERIAL_ROCK;
}

static CellMaterial BaseMaterialAt(const World *world,
                                   const BiomeSample *sample,
                                   const BiomeBlendColumn *blend, int x, int y,
                                   uint64_t strata, int depth)
{
    WorldBiome biome = sample->first;

    if (sample->first != sample->second) {
        if (BiomeBlendAt(blend, world->seed, x, y) < sample->mix) {
            biome = sample->second;
        }
    }
    return BiomeMaterialAt(world, biome, strata, depth);
}

static void WorldFillEllipse(World *world, int centerX, int centerY,
                             int radiusX, int radiusY,
                             CellMaterial material)
{
    int firstX;
    int lastX;
    int firstY;
    int lastY;
    int y;

    if (radiusX <= 0 || radiusY <= 0) return;
    firstX = ClampInt(centerX - radiusX, 0, world->width - 1);
    lastX = ClampInt(centerX + radiusX, 0, world->width - 1);
    firstY = ClampInt(centerY - radiusY, 0, world->height - 1);
    lastY = ClampInt(centerY + radiusY, 0, world->height - 1);

    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
            float dx = (float)(x - centerX) / (float)radiusX;
            float dy = (float)(y - centerY) / (float)radiusY;

            if (dx * dx + dy * dy <= 1.0f) {
                WorldSetGeneratedCell(world, x, y, material);
            }
        }
    }
}

static void WorldPlacePocket(World *world, int centerX, int centerY,
                             int radiusX, int radiusY, CellMaterial fill)
{
    WorldFillEllipse(world, centerX, centerY, radiusX + 4, radiusY + 4,
                     MATERIAL_ROCK);
    WorldFillEllipse(world, centerX, centerY, radiusX, radiusY,
                     MATERIAL_EMPTY);
    WorldFillEllipse(world, centerX, centerY + 2, radiusX - 2, radiusY - 2,
                     fill);
}

static void GenerateBaseTerrain(World *world)
{
    int x;

    for (x = 0; x < world->width; ++x) {
        BiomeSample sample = BiomeSampleAt(world, x);
        uint64_t strata = GenerationHash(world->seed, x, 0,
                                         GENERATION_STRATA);
        int surfaceY = SurfaceHeightAt(world, x);
        BiomeBlendColumn blend;
        int y;

        /* Only a column that straddles two biomes needs the blend field, and
           half of them do not. */
        if (sample.first != sample.second) {
            BiomeBlendColumnInit(&blend, world, x);
        } else {
            blend.rows = 0;
        }
        for (y = surfaceY; y < world->height; ++y) {
            WorldSetGeneratedCell(world, x, y,
                                  BaseMaterialAt(world, &sample, &blend, x, y,
                                                 strata, y - surfaceY));
        }
    }
}

static void GenerateCaves(World *world)
{
    int featureCount =
        (world->width + CAVE_FEATURE_SPACING - 1) / CAVE_FEATURE_SPACING;
    int feature;

    if (world->height < 48 || world->width < 10) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature, GENERATION_CAVES);
        int centerX = feature * CAVE_FEATURE_SPACING +
                      CAVE_FEATURE_SPACING / 2 + RngRange(&rng, -20, 20);
        int surfaceY;
        int minimumY;
        int maximumY;
        int centerY;
        int lobes;
        int lobe;
        WorldBiome biome;

        centerX = ClampInt(centerX, 4, world->width - 5);
        surfaceY = SurfaceHeightAt(world, centerX);
        minimumY = surfaceY + (world->height / 18 > 18
                                   ? world->height / 18
                                   : 18);
        maximumY = world->height - (world->height / 24 > 14
                                        ? world->height / 24
                                        : 14);
        if (minimumY >= maximumY) continue;
        centerY = RngRange(&rng, minimumY, maximumY);
        biome = WorldBiomeAt(world, centerX);
        lobes = RngRange(&rng, 3, 6);

        for (lobe = 0; lobe < lobes; ++lobe) {
            int radiusX;
            int radiusY;
            int stepX;
            int stepY;

            switch (biome) {
                case WORLD_BIOME_DUNES:
                    radiusX = RngRange(&rng, 10, 28);
                    radiusY = RngRange(&rng, 8, 20);
                    stepX = RngRange(&rng, -20, 20);
                    stepY = RngRange(&rng, -10, 10);
                    break;
                case WORLD_BIOME_FROST:
                    radiusX = RngRange(&rng, 7, 17);
                    radiusY = RngRange(&rng, 12, 27);
                    stepX = RngRange(&rng, -10, 10);
                    stepY = RngRange(&rng, -18, 18);
                    break;
                case WORLD_BIOME_VOLCANIC:
                    radiusX = RngRange(&rng, 17, 38);
                    radiusY = RngRange(&rng, 6, 14);
                    stepX = RngRange(&rng, -27, 27);
                    stepY = RngRange(&rng, -7, 7);
                    break;
                case WORLD_BIOME_TEMPERATE:
                case WORLD_BIOME_COUNT:
                default:
                    radiusX = RngRange(&rng, 14, 34);
                    radiusY = RngRange(&rng, 8, 18);
                    stepX = RngRange(&rng, -23, 23);
                    stepY = RngRange(&rng, -10, 10);
                    break;
            }

            WorldFillEllipse(world, centerX, centerY, radiusX, radiusY,
                             MATERIAL_EMPTY);
            centerX = ClampInt(centerX + stepX, 4, world->width - 5);
            centerY = ClampInt(centerY + stepY,
                               SurfaceHeightAt(world, centerX) + 12,
                               world->height - 10);
        }
    }
}

static void GenerateUndergroundFluids(World *world)
{
    int featureCount = (world->width + HYDROLOGY_FEATURE_SPACING - 1) /
                       HYDROLOGY_FEATURE_SPACING;
    int feature;

    if (world->height < 80 || world->width < 18) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature,
                                       GENERATION_HYDROLOGY);
        int centerX = feature * HYDROLOGY_FEATURE_SPACING +
                      HYDROLOGY_FEATURE_SPACING / 2 + RngRange(&rng, -52, 52);
        int surfaceY;
        int minimumY;
        int maximumY;
        CellMaterial liquid;
        WorldBiome biome;

        centerX = ClampInt(centerX, 8, world->width - 9);
        surfaceY = SurfaceHeightAt(world, centerX);
        biome = WorldBiomeAt(world, centerX);
        liquid = biome == WORLD_BIOME_VOLCANIC ? MATERIAL_LAVA : MATERIAL_WATER;
        minimumY = surfaceY + world->height /
                                   (biome == WORLD_BIOME_DUNES ? 4 : 6);
        maximumY = world->height - 24;
        if (minimumY >= maximumY) continue;

        WorldPlacePocket(world, centerX, RngRange(&rng, minimumY, maximumY),
                         RngRange(&rng, 14, 29), RngRange(&rng, 7, 15), liquid);
    }
}

static bool IsNearSpawn(const World *world, int x)
{
    int distance = x - world->width / 2;

    if (distance < 0) distance = -distance;
    return distance < SPAWN_FEATURE_CLEARANCE;
}

static void WorldPlaceMound(World *world, int centerX, int halfWidth,
                            int height, int foundationDepth,
                            CellMaterial material)
{
    int firstX = ClampInt(centerX - halfWidth, 0, world->width - 1);
    int lastX = ClampInt(centerX + halfWidth, 0, world->width - 1);
    int x;

    if (halfWidth <= 0 || height <= 0) return;
    for (x = firstX; x <= lastX; ++x) {
        float normalized = (float)(x - centerX) / (float)halfWidth;
        float crown = 1.0f - normalized * normalized;
        int surfaceY = SurfaceHeightAt(world, x);
        int topY = surfaceY - (int)(crown * crown * (float)height);
        int bottomY = ClampInt(surfaceY + foundationDepth, 0,
                               world->height - 1);
        int y;

        for (y = ClampInt(topY, 0, world->height - 1); y <= bottomY; ++y) {
            WorldSetGeneratedCell(world, x, y, material);
        }
    }
}

static void WorldPlaceSurfaceBasin(World *world, int centerX, int radiusX,
                                   int depth, CellMaterial liquid,
                                   bool frozen)
{
    int centerSurface = SurfaceHeightAt(world, centerX);
    int waterLine = centerSurface + 4;
    int firstX = ClampInt(centerX - radiusX, 0, world->width - 1);
    int lastX = ClampInt(centerX + radiusX, 0, world->width - 1);
    int x;

    if (radiusX <= 0 || depth <= 0) return;
    for (x = firstX; x <= lastX; ++x) {
        float dx = (float)(x - centerX) / (float)radiusX;
        float bowl = 1.0f - dx * dx;
        int naturalSurface = SurfaceHeightAt(world, x);
        int bottomY = naturalSurface + (int)(bowl * (float)depth);
        int y;

        bottomY = ClampInt(bottomY, naturalSurface, world->height - 2);
        for (y = ClampInt(naturalSurface - 2, 0, world->height - 1);
             y <= bottomY; ++y) {
            WorldSetGeneratedCell(world, x, y, MATERIAL_EMPTY);
        }
        if (bottomY < waterLine) continue;
        for (y = waterLine; y <= bottomY; ++y) {
            CellMaterial fill = liquid;

            if (frozen && y < waterLine + 3) fill = MATERIAL_ICE;
            WorldSetGeneratedCell(world, x, y, fill);
        }
    }
}

static void GenerateSurfaceFeatures(World *world)
{
    int featureCount = (world->width + SURFACE_FEATURE_SPACING - 1) /
                       SURFACE_FEATURE_SPACING;
    int feature;

    if (world->height < 96 || world->width < 26) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature,
                                       GENERATION_SURFACE_FEATURES);
        int centerX = feature * SURFACE_FEATURE_SPACING +
                      SURFACE_FEATURE_SPACING / 2 + RngRange(&rng, -72, 72);
        WorldBiome biome;

        centerX = ClampInt(centerX, 12, world->width - 13);
        if (IsNearSpawn(world, centerX)) continue;
        biome = WorldBiomeAt(world, centerX);

        switch (biome) {
            case WORLD_BIOME_TEMPERATE:
                if ((feature & 1) == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 44, 78),
                                           RngRange(&rng, 14, 25),
                                           MATERIAL_WATER, false);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 24, 45),
                                    RngRange(&rng, 12, 27), 18,
                                    MATERIAL_DIRT);
                }
                break;
            case WORLD_BIOME_DUNES:
                if (feature % 4 == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 28, 48),
                                           RngRange(&rng, 9, 15),
                                           MATERIAL_WATER, false);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 42, 76),
                                    RngRange(&rng, 14, 31), 12,
                                    MATERIAL_SAND);
                }
                break;
            case WORLD_BIOME_FROST:
                if ((feature & 1) == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 42, 70),
                                           RngRange(&rng, 13, 23),
                                           MATERIAL_WATER, true);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 18, 34),
                                    RngRange(&rng, 18, 38), 10,
                                    MATERIAL_ICE);
                }
                break;
            case WORLD_BIOME_VOLCANIC:
                if ((feature & 1) == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 31, 58),
                                           RngRange(&rng, 11, 20),
                                           MATERIAL_LAVA, false);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 22, 43),
                                    RngRange(&rng, 20, 44), 20,
                                    MATERIAL_ROCK);
                }
                break;
            case WORLD_BIOME_COUNT:
                break;
        }
    }
}

/* ---- flora ---------------------------------------------------------------
 *
 * What grows on a surface is most of what tells the player which biome they are
 * standing on. A dune and a frost shelf differ in the colour of their sand and
 * the shape of their hills, which is a difference you have to look for; a dune
 * with a cactus on it and a shelf with a pine on it is a difference you cannot
 * miss.
 *
 * Everything here is placed on the column it grows from, after the terrain and
 * its features are final, and only where that column's own surface is the
 * material the plant belongs on. Nothing floats, nothing is placed inside a
 * cave roof, and a plant is never written over anything but empty air.
 */

/* The topmost solid cell of a column, or -1 when the column is empty. */
static int SurfaceSolidY(const World *world, int x)
{
    int y;

    for (y = 0; y < world->height; ++y) {
        if (MaterialIsSolid(WorldMaterialAt(world, x, y))) return y;
    }
    return -1;
}

/* True only where every cell of the box is empty, so a plant never grows into a
   cliff face, another plant, or the roof of the cave it is standing over. */
static bool FloraSpaceIsClear(const World *world, int x, int y, int halfWidth,
                              int height)
{
    int row;

    for (row = y - height + 1; row <= y; ++row) {
        int column;

        for (column = x - halfWidth; column <= x + halfWidth; ++column) {
            if (!WorldInBounds(world, column, row)) return false;
            if (WorldMaterialAt(world, column, row) != MATERIAL_EMPTY) {
                return false;
            }
        }
    }
    return true;
}

static void FloraFillDisc(World *world, int centerX, int centerY, int radiusX,
                          int radiusY, CellMaterial material, uint64_t seed,
                          int salt)
{
    int y;

    for (y = centerY - radiusY; y <= centerY + radiusY; ++y) {
        int x;

        for (x = centerX - radiusX; x <= centerX + radiusX; ++x) {
            float dx = (float)(x - centerX) / (float)radiusX;
            float dy = (float)(y - centerY) / (float)radiusY;

            if (!WorldInBounds(world, x, y) || dx * dx + dy * dy > 1.0f) {
                continue;
            }
            /* A ragged edge, so a canopy reads as leaves rather than as a
               painted ellipse. */
            if (dx * dx + dy * dy > 0.55f &&
                PatchUnit(seed, x * 3 + salt, y * 3) < 0.34f) {
                continue;
            }
            if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) continue;
            WorldSetGeneratedCell(world, x, y, material);
        }
    }

    /* The ragged edge is drawn by a hash, and a hash left to itself strands
       single cells in mid air: a leaf with nothing touching it is not foliage,
       it is a dead pixel. Sweeping the same box afterwards is the cheapest
       honest fix — the erosion stays random, and nothing survives it alone. */
    for (y = centerY - radiusY; y <= centerY + radiusY; ++y) {
        int x;

        for (x = centerX - radiusX; x <= centerX + radiusX; ++x) {
            bool joined = false;
            int offsetX;

            if (!WorldInBounds(world, x, y) ||
                WorldMaterialAt(world, x, y) != material) {
                continue;
            }
            for (offsetX = -1; offsetX <= 1 && !joined; ++offsetX) {
                int offsetY;

                for (offsetY = -1; offsetY <= 1; ++offsetY) {
                    if (offsetX == 0 && offsetY == 0) continue;
                    if (!WorldInBounds(world, x + offsetX, y + offsetY)) continue;
                    if (WorldMaterialAt(world, x + offsetX, y + offsetY) !=
                        MATERIAL_EMPTY) {
                        joined = true;
                        break;
                    }
                }
            }
            if (!joined) {
                WorldSetGeneratedCell(world, x, y, MATERIAL_EMPTY);
            }
        }
    }
}

/* One limb, and the limbs that grow out of it.
 *
 * A trunk that goes straight up with an ellipse on top is a lollipop, and it is
 * the one shape that reads as a symbol for a tree rather than as a tree. What
 * makes a real one is that it divides: every limb is a shorter, thinner, more
 * crooked version of the limb it came from, and the leaves hang off the ends
 * rather than sitting on the top.
 *
 * The waver is the same idea as the broken edge of a beam — the shape is
 * decided by a hash rather than drawn — so that no two trees on a hillside are
 * the same tree, and none of them is straight.
 *
 * Depth is what bounds it: each level forks at most twice and is a fraction of
 * the length of the one above, so the whole tree is a few hundred cells however
 * generous the numbers look.
 */
static void FloraGrowLimb(World *world, float x, float y, float angle,
                          float length, int depth, int thickness, Rng *rng,
                          CellMaterial canopy, int canopyRadius)
{
    int steps = (int)length;
    int step;

    if (steps < 2 || depth < 0) {
        return;
    }
    for (step = 0; step < steps; ++step) {
        int cellX = (int)floorf(x);
        int cellY = (int)floorf(y);
        int side;

        if (!WorldInBounds(world, cellX, cellY)) return;
        /* A limb stops where it meets anything: it does not bore through a
           cliff, and it does not overwrite another tree. */
        if (WorldMaterialAt(world, cellX, cellY) != MATERIAL_EMPTY) {
            if (step > 1) break;
        } else {
            WorldSetGeneratedCell(world, cellX, cellY, MATERIAL_WOOD);
        }
        /* Thicker near the base, and the extra cells go on the side the limb is
           leaning away from, which is where a real one carries its weight. */
        for (side = 1; side < thickness; ++side) {
            int offsetX = cellX + (angle > -1.5708f ? -side : side);

            if (WorldInBounds(world, offsetX, cellY) &&
                WorldMaterialAt(world, offsetX, cellY) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, offsetX, cellY, MATERIAL_WOOD);
            }
        }
        x += cosf(angle);
        y += sinf(angle);
        /* Crooked, not curved: the drift is redrawn every step. */
        angle += ((float)RngRange(rng, -100, 100) / 100.0f) * 0.10f;
    }

    if (depth == 0) {
        if (canopy != MATERIAL_EMPTY && canopyRadius > 0) {
            FloraFillDisc(world, (int)floorf(x), (int)floorf(y), canopyRadius,
                          canopyRadius, canopy, world->seed,
                          RngRange(rng, 0, 255));
        }
        return;
    }

    {
        int forks = RngRange(rng, 2, 3);
        int fork;

        for (fork = 0; fork < forks; ++fork) {
            float spread = ((float)fork / (float)(forks - 1)) - 0.5f;
            float turn = spread * (0.9f + (float)RngRange(rng, 0, 40) * 0.01f);
            float shorter = length * (0.52f + (float)RngRange(rng, 0, 22) * 0.01f);

            FloraGrowLimb(world, x, y, angle + turn, shorter, depth - 1,
                          thickness > 1 ? thickness - 1 : 1, rng, canopy,
                          canopyRadius);
        }
    }
}

/* A tree that spreads: a leaning trunk that divides three times, with foliage
   only where the limbs end. */
static void FloraPlaceBroadleaf(World *world, int x, int groundY, Rng *rng,
                                int trunkHeight, int canopyRadius,
                                CellMaterial canopy)
{
    float lean = (float)RngRange(rng, -22, 22) * 0.01f;

    if (!FloraSpaceIsClear(world, x, groundY - 1, 1, trunkHeight / 2)) {
        return;
    }
    FloraGrowLimb(world, (float)x + 0.5f, (float)groundY - 0.5f,
                  -1.5708f + lean, (float)trunkHeight, 3, 3, rng, canopy,
                  canopyRadius);
}

/* A conifer: one straight spine with short limbs in tiers, each tier a little
   shorter than the one below it. The silhouette is a cone, and it is a cone
   because of where the limbs are, not because an ellipse was squashed. */
static void FloraPlaceConifer(World *world, int x, int groundY, Rng *rng,
                              int trunkHeight, CellMaterial canopy)
{
    int top = groundY - trunkHeight;
    int tiers = trunkHeight / 4;
    int tier;
    int y;

    if (!FloraSpaceIsClear(world, x, groundY - 1, 1, trunkHeight / 2)) {
        return;
    }
    for (y = groundY - 1; y >= top; --y) {
        if (!WorldInBounds(world, x, y)) return;
        if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) break;
        WorldSetGeneratedCell(world, x, y, MATERIAL_WOOD);
    }
    for (tier = 0; tier < tiers; ++tier) {
        /* Measured from the top down, so the tiers narrow toward the crown
           however tall the tree turned out. */
        float amount = (float)tier / (float)(tiers > 1 ? tiers - 1 : 1);
        float reach = 1.5f + amount * (float)trunkHeight * 0.26f;
        int tierY = top + 1 + (int)(amount * (float)(trunkHeight - 2));
        int side;

        for (side = -1; side <= 1; side += 2) {
            FloraGrowLimb(world, (float)x + 0.5f, (float)tierY + 0.5f,
                          side > 0 ? 0.45f : PI - 0.45f, reach, 0, 1, rng,
                          canopy, 1 + (int)(amount * 3.0f));
        }
    }
    /* A crown, so the spine does not end in a bare stick. */
    FloraFillDisc(world, x, top, 2, 3, canopy, world->seed,
                  RngRange(rng, 0, 255));
}

/* A column with two arms, which is the silhouette a desert reads by. */
static void FloraPlaceCactus(World *world, int x, int groundY, Rng *rng,
                             int height)
{
    int armY = groundY - height + RngRange(rng, 2, 5);
    int side = RngRange(rng, 0, 1) == 0 ? -1 : 1;
    bool thick;
    int y;

    if (!FloraSpaceIsClear(world, x, groundY - 1, 1, height)) {
        return;
    }
    thick = WorldInBounds(world, x + 1, groundY) &&
            MaterialIsSolid(WorldMaterialAt(world, x + 1, groundY));
    for (y = groundY - 1; y >= groundY - height; --y) {
        WorldSetGeneratedCell(world, x, y, MATERIAL_CACTUS);
        if (thick) WorldSetGeneratedCell(world, x + 1, y, MATERIAL_CACTUS);
    }
    for (y = armY; y >= armY - 4; --y) {
        WorldSetGeneratedCell(world, x + side * 3, y, MATERIAL_CACTUS);
    }
    WorldSetGeneratedCell(world, x + side, armY, MATERIAL_CACTUS);
    WorldSetGeneratedCell(world, x + side * 2, armY, MATERIAL_CACTUS);
}

static void GenerateFlora(World *world)
{
    int x;

    if (world->height < 96 || world->width < 64) return;

    for (x = 2; x < world->width - 4; ++x) {
        WorldBiome biome = WorldBiomeAt(world, x);
        int surface = SurfaceSolidY(world, x);
        CellMaterial ground;
        Rng rng;

        if (surface <= 8 || IsNearSpawn(world, x)) continue;
        ground = WorldMaterialAt(world, x, surface);
        /* One stream per column, so what grows at a column depends on the
           column and the seed and on nothing that was drawn before it — the
           contract a chunk generator will need. */
        RngSeed(&rng, GenerationHash(world->seed, x, 0,
                                     GENERATION_SURFACE_FEATURES + 40u));

        switch (biome) {
            case WORLD_BIOME_TEMPERATE:
                if (ground != MATERIAL_DIRT) break;
                /* The tree is decided first. Grass is placed on the cell a
                   trunk would stand on, and asking for the trunk afterwards
                   found that cell occupied — which is how raising the tree
                   chance made the forest thinner. */
                if (RngRange(&rng, 0, 999) < 48) {
                    FloraPlaceBroadleaf(world, x, surface, &rng,
                                        RngRange(&rng, 13, 24),
                                        RngRange(&rng, 3, 6), MATERIAL_LEAF);
                }
                /* Grass on almost every exposed cell of soil: it is the
                   cheapest thing that makes ground read as living. */
                if (RngRange(&rng, 0, 99) < 86 &&
                    WorldMaterialAt(world, x, surface - 1) == MATERIAL_EMPTY) {
                    int tuft = RngRange(&rng, 0, 99) < 34 ? 2 : 1;
                    int blade;

                    /* One cell alone is a tint on the ground; a tuft is
                       something the eye reads as growing. */
                    for (blade = 0; blade < tuft; ++blade) {
                        if (!WorldInBounds(world, x, surface - blade)) break;
                        if (blade > 0 &&
                            WorldMaterialAt(world, x, surface - blade) !=
                                MATERIAL_EMPTY) {
                            break;
                        }
                        WorldSetGeneratedCell(world, x, surface - blade,
                                              MATERIAL_GRASS);
                    }
                }
                break;
            case WORLD_BIOME_DUNES:
                if (ground != MATERIAL_SAND) break;
                if (RngRange(&rng, 0, 999) < 16) {
                    FloraPlaceCactus(world, x, surface, &rng,
                                     RngRange(&rng, 9, 17));
                }
                break;
            case WORLD_BIOME_FROST:
                if (ground != MATERIAL_ICE && ground != MATERIAL_DIRT) break;
                /* Pines: a narrow, tall canopy that reaches most of the way down
                   the trunk, which is what separates them from the broadleaf. */
                if (RngRange(&rng, 0, 999) < 30) {
                    FloraPlaceConifer(world, x, surface, &rng,
                                      RngRange(&rng, 18, 32), MATERIAL_LEAF);
                }
                break;
            case WORLD_BIOME_VOLCANIC:
                if (ground != MATERIAL_ROCK) break;
                /* Dead trunks with nothing on them. The ember wastes are what
                   the other biomes look like after they have burned. */
                if (RngRange(&rng, 0, 999) < 20) {
                    /* Bare: the same branching with nothing hanging on it. The
                       ember wastes are what the other biomes look like after
                       they have burned. */
                    FloraPlaceBroadleaf(world, x, surface, &rng,
                                        RngRange(&rng, 9, 17), 0,
                                        MATERIAL_EMPTY);
                }
                break;
            case WORLD_BIOME_COUNT:
                break;
        }
    }
}

void WorldGenerateBiomeTerrain(World *world)
{
    GenerateBaseTerrain(world);
    GenerateCaves(world);
    GenerateUndergroundFluids(world);
    GenerateSurfaceFeatures(world);
    /* Last, so that every plant grows on the surface as it finally is rather
       than on one a later feature was going to bury. */
    GenerateFlora(world);
}
