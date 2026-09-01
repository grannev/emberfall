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
#define BIOME_BLEND_WIDTH 192
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
    GENERATION_SURFACE_FEATURES = 9
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
    return BiomeForRegion(world, x / BIOME_REGION_WIDTH);
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

/* Nominal regions remain cheap to query, while terrain parameters blend for
   192 cells on either side of a boundary. */
static BiomeSample BiomeSampleAt(const World *world, int x)
{
    int lastRegion;
    int region;
    int localX;
    WorldBiome current;

    x = ClampInt(x, 0, world->width - 1);
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
                                   const BiomeSample *sample, int x, int y,
                                   uint64_t strata, int depth)
{
    WorldBiome biome = sample->first;

    if (sample->first != sample->second) {
        float patch = (float)(GenerationPatchHash(world->seed, x / 6, y / 6) >>
                              8) /
                      (float)(1u << 24);

        if (patch < sample->mix) biome = sample->second;
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
        int y;

        for (y = surfaceY; y < world->height; ++y) {
            WorldSetGeneratedCell(world, x, y,
                                  BaseMaterialAt(world, &sample, x, y,
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

void WorldGenerateBiomeTerrain(World *world)
{
    GenerateBaseTerrain(world);
    GenerateCaves(world);
    GenerateUndergroundFluids(world);
    GenerateSurfaceFeatures(world);
}
