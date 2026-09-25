/* Deterministic biome composition for WorldGenerate.
 *
 * Every decision is derived from the world seed and coordinates. The current
 * store is still filled eagerly, but the generator does not depend on the
 * order in which distant features are visited; this is the contract a future
 * chunk generator will need.
 */
#include "world_internal.h"

#include <raymath.h>
#include <stdlib.h>

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
/* Ponds have their own, much denser grid than the landmark features do. Making
   the landmark grid denser instead would have multiplied the mounds as well,
   and a world with a hill every two hundred cells is a different world. */
#define POND_FEATURE_SPACING 224
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
    GENERATION_BIOME_WARP = 10,
    GENERATION_PONDS = 11,
    GENERATION_RANGES = 12,
    GENERATION_RIDGES = 13,
    GENERATION_PEAKS = 14,
    GENERATION_BASALT = 15,
    GENERATION_LENSES = 16,
    GENERATION_CAVERNS = 17,
    GENERATION_TUNNELS = 18,
    GENERATION_SNOW = 19
};

typedef struct BiomeSurfaceShape {
    float baseHeight;
    float continentAmplitude;
    float hillAmplitude;
    float detailAmplitude;
    /* How tall this biome's mountains stand where a range runs through it,
       as a fraction of the ground band. */
    float mountainAmplitude;
    /* Height of one step of a terraced landscape, as a fraction of the ground
       band; zero for none. The dunes rise in mesas: flat tops, steep sides. */
    float terrace;
} BiomeSurfaceShape;

typedef struct BiomeSample {
    WorldBiome first;
    WorldBiome second;
    float mix;
} BiomeSample;

/* Every field is a fraction of the world's height, which is what lets the same
   table describe a test world sixty cells tall and the production one.
 *
 * The base heights sit far lower down the world than they used to, and the
 * amplitudes are correspondingly smaller. Both changes are the same decision:
 * the world grew a long way upward, and if these numbers had been left alone
 * the whole gain would have gone into taller mountains and a surface still only
 * a few seconds of boost below space. Lowering the surface spends the new
 * height on sky; shrinking the amplitudes keeps a hill the size it was, in
 * cells, so the character — who is now smaller — is what makes it read as
 * bigger. */
/* The mountains are where the world is meant to be climbed and cut into: a
   range is a stretch of ridges, not a single cone, and it takes the soil off
   what it lifts — a peak is bare rock under snow, and the trees stop below
   it. */
static const BiomeSurfaceShape BIOME_SURFACES[WORLD_BIOME_COUNT] = {
    [WORLD_BIOME_TEMPERATE] = {0.535f, 0.0252f, 0.0180f, 0.0048f, 0.140f, 0.0f},
    [WORLD_BIOME_DUNES] = {0.555f, 0.0180f, 0.0264f, 0.0072f, 0.070f, 0.021f},
    [WORLD_BIOME_FROST] = {0.495f, 0.0312f, 0.0216f, 0.0060f, 0.220f, 0.0f},
    [WORLD_BIOME_VOLCANIC] = {0.465f, 0.0360f, 0.0360f, 0.0108f, 0.160f, 0.0f},
    /* Well below WORLD_SEA_LEVEL, and with the gentlest relief of any biome: a
       sea floor is the one landscape the player looks at through a hundred
       cells of water, and every ridge on it is a ridge the light has to reach
       through them. The blend band either side does the coastline for free —
       the base height crosses the sea level somewhere inside it, and wherever
       it does is a shore. */
    [WORLD_BIOME_OCEAN] = {0.700f, 0.0120f, 0.0090f, 0.0030f, 0.0f, 0.0f},
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

/* Smooth noise along the world, periodic in its width: the world wraps, and
   a noise that did not would put a cliff at the seam. The lattice is the
   nearest whole number of wavelengths that fits round the world, so the
   wavelength a caller asks for is the one it gets to within half of one. */
static float ValueNoise1D(uint64_t seed, int x, int wavelength, int period,
                          enum GenerationChannel channel)
{
    int count;
    float span;
    float position;
    int lattice;
    float amount;
    float first;
    float second;

    if (wavelength <= 0 || period <= 0) return 0.0f;
    count = (int)((float)period / (float)wavelength + 0.5f);
    if (count < 1) count = 1;
    span = (float)period / (float)count;
    position = (float)PositiveModulo(x, period) / span;
    lattice = (int)floorf(position);
    amount = SmoothStep(position - (float)lattice);
    first = GenerationUnit(seed, PositiveModulo(lattice, count), 0,
                           (uint64_t)channel) * 2.0f - 1.0f;
    second = GenerationUnit(seed, PositiveModulo(lattice + 1, count), 0,
                            (uint64_t)channel) * 2.0f - 1.0f;
    return LerpFloat(first, second, amount);
}

/* How far `x` is from `to` the short way round the world. */
static int WrappedDistance(const World *world, int x, int to)
{
    int distance = PositiveModulo(x - to, world->width);

    return distance > world->width / 2 ? world->width - distance : distance;
}

/* Biome regions: the nearest whole number of BIOME_REGION_WIDTH that fits
   round the world, so the last region meets the first at the seam. */
static int BiomeRegionCount(const World *world)
{
    int count = (int)((float)world->width / (float)BIOME_REGION_WIDTH + 0.5f);

    return count < 1 ? 1 : count;
}

static float BiomeRegionWidth(const World *world)
{
    return (float)world->width / (float)BiomeRegionCount(world);
}

/* Bends a column sideways before it is turned into a region index, so that the
   seam between two biomes meanders instead of running straight down the map. */
static int BiomeWarpedX(const World *world, int x)
{
    float warp = ValueNoise1D(world->seed, x, BIOME_WARP_WAVELENGTH, world->width,
                              GENERATION_BIOME_WARP) *
                 (float)BIOME_BOUNDARY_WARP;

    return PositiveModulo(x + (int)warp, world->width);
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
    /* Rows of the ground band, not of the world: the sky above the band is
       never interleaved, and indexing the lattice from the top of the band
       keeps the ground the same ground however tall the sky is. */
    int rows = WorldGroundRows(world) / BIOME_BLEND_PATCH + 2;
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
    /* The ocean is last, and the rotation is drawn from the entries before it,
       so the region the player starts in is never open water. Everything else
       about the cycle is unchanged: whichever land biome the middle draws, the
       ocean is still four regions away in one direction and one in the
       other. */
    static const WorldBiome order[WORLD_BIOME_COUNT] = {
        WORLD_BIOME_TEMPERATE,
        WORLD_BIOME_DUNES,
        WORLD_BIOME_FROST,
        WORLD_BIOME_VOLCANIC,
        WORLD_BIOME_OCEAN,
    };
    uint64_t layout = GenerationHash(world->seed, 0, 0,
                                     GENERATION_BIOME_ORDER);
    int centerRegion = (int)((float)(world->width / 2) / BiomeRegionWidth(world));
    int relative = region - centerRegion;
    int direction = (layout & 4ull) != 0ull ? -1 : 1;
    int rotation = (int)(layout % (uint64_t)(WORLD_BIOME_COUNT - 1));
    int index = PositiveModulo(rotation + relative * direction,
                               WORLD_BIOME_COUNT);

    return order[index];
}

WorldBiome WorldBiomeAt(const World *world, int x)
{
    if (world == NULL || world->width <= 0) {
        return WORLD_BIOME_TEMPERATE;
    }
    {
        int region = (int)((float)BiomeWarpedX(world, x) / BiomeRegionWidth(world));

        return BiomeForRegion(world, ClampInt(region, 0, BiomeRegionCount(world) - 1));
    }
}

const char *WorldBiomeName(WorldBiome biome)
{
    static const char *const names[WORLD_BIOME_COUNT] = {
        [WORLD_BIOME_TEMPERATE] = "TEMPERATE BASIN",
        [WORLD_BIOME_DUNES] = "SHATTERED DUNES",
        [WORLD_BIOME_FROST] = "FROST SHELF",
        [WORLD_BIOME_VOLCANIC] = "EMBER WASTES",
        [WORLD_BIOME_OCEAN] = "SUNKEN SHELF",
    };

    if (biome < 0 || biome >= WORLD_BIOME_COUNT) return "UNKNOWN";
    return names[biome];
}

/* Regions remain cheap to query, while terrain parameters blend for 384 cells on
   either side of a boundary — half the width of a region, so a biome is a place
   the world gradually becomes rather than a place it switches to. */
static BiomeSample BiomeSampleAt(const World *world, int x)
{
    int count = BiomeRegionCount(world);
    float regionWidth = BiomeRegionWidth(world);
    float warped = (float)BiomeWarpedX(world, x);
    int region = ClampInt((int)(warped / regionWidth), 0, count - 1);
    float localX = warped - (float)region * regionWidth;
    WorldBiome current = BiomeForRegion(world, region);

    /* Every region has two neighbours: the world wraps, and the first region
       blends into the last across the seam like any two others. */
    if (count > 1 && localX < (float)BIOME_BLEND_WIDTH) {
        float amount = (localX + (float)BIOME_BLEND_WIDTH) /
                       (float)(BIOME_BLEND_WIDTH * 2);

        return (BiomeSample){BiomeForRegion(world, PositiveModulo(region - 1, count)),
                             current, SmoothStep(amount)};
    }
    if (count > 1 && localX >= regionWidth - (float)BIOME_BLEND_WIDTH) {
        float amount = (localX - (regionWidth - (float)BIOME_BLEND_WIDTH)) /
                       (float)(BIOME_BLEND_WIDTH * 2);

        return (BiomeSample){current,
                             BiomeForRegion(world, PositiveModulo(region + 1, count)),
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
        .mountainAmplitude =
            LerpFloat(first->mountainAmplitude, second->mountainAmplitude,
                      sample->mix),
        .terrace = LerpFloat(first->terrace, second->terrace, sample->mix),
    };
}

/* How much of a mountain range stands over column `x`, 0..1 of the biome's
   mountain amplitude. Ranges come and go along the world on a long
   wavelength; inside one, ridges and saddles alternate, and the sharpest
   ridges are the rarest. */
static float MountainRelief(const World *world, int x)
{
    float range = SmoothStep((ValueNoise1D(world->seed, x, 2600, world->width,
                                           GENERATION_RANGES) +
                              0.05f) /
                             0.55f);
    float ridge = 1.0f - fabsf(ValueNoise1D(world->seed, x, 420, world->width,
                                            GENERATION_RIDGES));
    float peaks = 0.65f + 0.35f * ValueNoise1D(world->seed, x, 140, world->width,
                                               GENERATION_PEAKS);

    return range * ridge * ridge * peaks;
}

/* The surface as a fraction of the ground band, before mountains: the plain a
   range rises from, which is what the soil depth is measured against. */
static float SurfaceBaseFraction(const World *world, int x,
                                 const BiomeSurfaceShape *shape)
{
    return shape->baseHeight +
           ValueNoise1D(world->seed, x, 1200, world->width, GENERATION_CONTINENT) *
               shape->continentAmplitude +
           ValueNoise1D(world->seed, x, 260, world->width, GENERATION_HILLS) *
               shape->hillAmplitude +
           ValueNoise1D(world->seed, x, 52, world->width, GENERATION_DETAIL) *
               shape->detailAmplitude;
}

static float SurfaceHeightRaw(const World *world, int x)
{
    BiomeSample sample = BiomeSampleAt(world, x);
    BiomeSurfaceShape shape = BlendedSurfaceShape(&sample);
    float fraction = SurfaceBaseFraction(world, x, &shape) -
                     MountainRelief(world, x) * shape.mountainAmplitude;

    /* Mesas: the ground rises in flat steps with steep sides, the step's rise
       squeezed into its last few cells. */
    if (shape.terrace > 0.0f) {
        float steps = fraction / shape.terrace;
        float step = floorf(steps);
        float rise = steps - step;

        rise = rise * rise * rise * rise * rise * rise;
        fraction = (step + rise) * shape.terrace;
    }
    return WorldGroundY(world, fraction);
}

/* Cells the column has been lifted above its plain by a mountain: what thins
   its soil and caps it with snow. */
static int SurfaceRelief(const World *world, int x)
{
    BiomeSample sample = BiomeSampleAt(world, x);
    BiomeSurfaceShape shape = BlendedSurfaceShape(&sample);

    return (int)(MountainRelief(world, x) * shape.mountainAmplitude *
                 (float)WorldGroundRows(world));
}

static int SurfaceHeightAt(const World *world, int x)
{
    int centerX = world->width / 2;
    int distance = WrappedDistance(world, x, centerX);
    float height = SurfaceHeightRaw(world, x);
    int minimumY;
    int maximumY;

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
                                    uint64_t strata, int depth, float soil)
{
    /* Strata are fractions of the ground band, never of the sky above it.
       `soil` thins every loose layer where a mountain has lifted the ground:
       a peak is bare rock. */
    int height = (int)((float)WorldGroundRows(world) * soil);

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
        case WORLD_BIOME_OCEAN: {
            /* Sand on top of the shelf, then the same soil and rock as
               anywhere else. What makes it read as a sea floor is that it is
               under water, not that it is made of something exotic. */
            int sandDepth = height / 40 + 8 + (int)(strata % 11ull);
            int siltDepth = sandDepth + height / 26 + 8;

            if (depth < sandDepth) return MATERIAL_SAND;
            return depth < siltDepth ? MATERIAL_DIRT : MATERIAL_ROCK;
        }
        case WORLD_BIOME_COUNT:
            break;
    }
    return MATERIAL_ROCK;
}

static CellMaterial BaseMaterialAt(const World *world,
                                   const BiomeSample *sample,
                                   const BiomeBlendColumn *blend, int x, int y,
                                   uint64_t strata, int depth, float soil,
                                   int basaltY)
{
    WorldBiome biome = sample->first;

    /* The deep band is basalt whatever the surface above it: the biomes are
       a skin, and a long way down every one of them is the same dark rock. */
    if (y >= basaltY) {
        return MATERIAL_BASALT;
    }

    if (sample->first != sample->second) {
        if (BiomeBlendAt(blend, world->seed, x, y - WorldSkyRows(world)) <
            sample->mix) {
            biome = sample->second;
        }
    }
    return BiomeMaterialAt(world, biome, strata, depth, soil);
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
    /* Columns are not clamped: the world wraps, and an ellipse across the
       seam is written into both sides of it. */
    firstX = centerX - radiusX;
    lastX = centerX + radiusX;
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
    int waterLine = centerY + 2;
    int lastY = centerY + radiusY;
    int x;

    WorldFillEllipse(world, centerX, centerY, radiusX + 4, radiusY + 4,
                     MATERIAL_ROCK);
    WorldFillEllipse(world, centerX, centerY, radiusX, radiusY,
                     MATERIAL_EMPTY);
    /* Filled to a level line rather than as a smaller ellipse inside the
       cavity. An ellipse of liquid has a curved underside, so its rim hangs
       over the empty floor beneath it — a lens of water in mid air, which the
       simulation then has to drop. A pool has a flat top and rests on the
       floor it is standing in. */
    for (x = centerX - radiusX; x <= centerX + radiusX; ++x) {
        float dx = (float)(x - centerX) / (float)radiusX;
        int y;

        for (y = waterLine; y <= lastY; ++y) {
            /* Inside the cavity, not merely inside the box around it. A cave
               dug earlier can lie right outside the pocket's rock lining, and
               a fill that only asked whether a cell was empty poured the
               pocket's water into it. */
            float dy = (float)(y - centerY) / (float)radiusY;

            if (dx * dx + dy * dy > 1.0f) continue;
            if (!WorldInBounds(world, x, y)) continue;
            if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) continue;
            WorldSetGeneratedCell(world, x, y, fill);
        }
    }
}

static void GenerateBaseTerrain(World *world)
{
    int x;

    for (x = 0; x < world->width; ++x) {
        BiomeSample sample = BiomeSampleAt(world, x);
        uint64_t strata = GenerationHash(world->seed, x, 0,
                                         GENERATION_STRATA);
        int surfaceY = SurfaceHeightAt(world, x);
        /* Soil thins over a mountain and is gone by the time it has risen a
           hundred cells; a little always clings on. */
        float soil = Clamp(1.0f - (float)SurfaceRelief(world, x) / 110.0f, 0.12f,
                           1.0f);
        int basaltY = (int)WorldGroundY(
            world, 0.80f + 0.035f * ValueNoise1D(world->seed, x, 300, world->width,
                                                 GENERATION_BASALT));
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
                                                 strata, y - surfaceY, soil,
                                                 basaltY));
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

        centerX = PositiveModulo(centerX, world->width);
        surfaceY = SurfaceHeightAt(world, centerX);
        minimumY = surfaceY + (WorldGroundRows(world) / 18 > 18
                                   ? WorldGroundRows(world) / 18
                                   : 18);
        maximumY = world->height - (WorldGroundRows(world) / 24 > 14
                                        ? WorldGroundRows(world) / 24
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
            centerX = PositiveModulo(centerX + stepX, world->width);
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

        centerX = PositiveModulo(centerX, world->width);
        surfaceY = SurfaceHeightAt(world, centerX);
        biome = WorldBiomeAt(world, centerX);
        liquid = biome == WORLD_BIOME_VOLCANIC ? MATERIAL_LAVA : MATERIAL_WATER;
        minimumY = surfaceY + WorldGroundRows(world) /
                                   (biome == WORLD_BIOME_DUNES ? 4 : 6);
        maximumY = world->height - 24;
        if (minimumY >= maximumY) continue;

        WorldPlacePocket(world, centerX, RngRange(&rng, minimumY, maximumY),
                         RngRange(&rng, 14, 29), RngRange(&rng, 7, 15), liquid);
    }
}

static bool IsNearSpawn(const World *world, int x)
{
    return WrappedDistance(world, x, world->width / 2) < SPAWN_FEATURE_CLEARANCE;
}

static void WorldPlaceMound(World *world, int centerX, int halfWidth,
                            int height, int foundationDepth,
                            CellMaterial material)
{
    int firstX = centerX - halfWidth;
    int lastX = centerX + halfWidth;
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

/* Whether the ground the basin is about to be cut into can hold water.
 *
 * Caves are dug before surface features are placed, and they reach close enough
 * to the surface to undercut a lake. A basin carved over one is a lake with a
 * hole in the bottom: it drains into the cavern the moment the simulation
 * starts, and what the player finds is a bowl-shaped scar with water running
 * out of it. Checking first is cheaper than sealing afterwards, and a lake that
 * is simply not there is invisible where a draining one is not. */
static bool BasinBedIsSolid(const World *world, int firstX, int lastX,
                            int waterLine, int depth)
{
    int x;

    for (x = firstX; x <= lastX; ++x) {
        int surface = SurfaceHeightAt(world, x);
        int y;

        for (y = surface; y <= waterLine + depth + 2; ++y) {
            if (!WorldInBounds(world, x, y)) return false;
            if (!MaterialIsSolid(WorldMaterialAt(world, x, y))) return false;
        }
    }
    return true;
}

/* Smallest lake worth digging. Anything narrower than this reads as a puddle in
   a scar rather than as water, and the excavation is more visible than the
   liquid in it. */
#define BASIN_MINIMUM_WIDTH 14

static void WorldPlaceSurfaceBasin(World *world, int centerX, int radiusX,
                                   int depth, CellMaterial liquid,
                                   bool frozen)
{
    int centerSurface = SurfaceHeightAt(world, centerX);
    int waterLine = centerSurface + 4;
    int firstX = centerX;
    int lastX = centerX;
    int x;

    if (radiusX <= 0 || depth <= 0) return;

    /* The shore is where the ground stops being able to hold the water.
     *
     * Filling to a fixed level across a fixed radius said nothing about the
     * terrain at the edges of that radius: wherever the natural surface ran
     * lower than the water line, the fill began above the ground and left a
     * slab of water — or, in the frost, a lid of ice — hanging in open air over
     * the slope beyond. A lake spills at its lowest rim, so the span is walked
     * outward from the centre and stopped at the first column that cannot hold
     * the level. */
    while (firstX > centerX - radiusX &&
           SurfaceHeightAt(world, firstX - 1) <= waterLine) {
        --firstX;
    }
    while (lastX < centerX + radiusX &&
           SurfaceHeightAt(world, lastX + 1) <= waterLine) {
        ++lastX;
    }
    /* The outermost column of the span is the rim, and is left untouched: it is
       ground that reaches at least to the water line, and the water needs a
       wall there or it simply runs out of the end of the basin. */
    ++firstX;
    --lastX;
    if (lastX - firstX + 1 < BASIN_MINIMUM_WIDTH) return;
    if (!BasinBedIsSolid(world, firstX, lastX, waterLine, depth)) return;

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

        centerX = PositiveModulo(centerX, world->width);
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
            case WORLD_BIOME_OCEAN:
                /* No landmarks on the shelf. A mound under a hundred cells of
                   water is an island nobody asked for, and a basin cut into a
                   sea floor is a hole in the bottom of the sea. */
                break;
            case WORLD_BIOME_COUNT:
                break;
        }
    }
}

/* Small ponds, on their own much denser grid than the landmarks.
 *
 * Standing water is what a landscape looks lived-in for, and one lake every
 * thousand cells is not standing water, it is a landmark. These are small
 * enough to sit in an ordinary dip and frequent enough that a screen of
 * temperate ground usually holds one. Everything about where they may go is
 * WorldPlaceSurfaceBasin's own decision — a pond that cannot be held by the
 * ground it is offered is simply not placed. */
static void GenerateSurfacePonds(World *world)
{
    int featureCount = (world->width + POND_FEATURE_SPACING - 1) /
                       POND_FEATURE_SPACING;
    int feature;

    if (world->height < 96 || world->width < 26) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature, GENERATION_PONDS);
        int centerX = feature * POND_FEATURE_SPACING +
                      POND_FEATURE_SPACING / 2 + RngRange(&rng, -60, 60);
        WorldBiome biome;

        centerX = PositiveModulo(centerX, world->width);
        if (IsNearSpawn(world, centerX)) continue;
        biome = WorldBiomeAt(world, centerX);
        if (biome == WORLD_BIOME_OCEAN) continue;
        if (RngRange(&rng, 0, 99) >= 64) continue;

        WorldPlaceSurfaceBasin(world, centerX, RngRange(&rng, 15, 31),
                               RngRange(&rng, 7, 14),
                               biome == WORLD_BIOME_VOLCANIC ? MATERIAL_LAVA
                                                             : MATERIAL_WATER,
                               biome == WORLD_BIOME_FROST);
    }
}

/* The sea.
 *
 * One level for the whole map, poured after every feature that could change the
 * shape of the ground and before anything grows on it. Each column is filled
 * from the sea level down until it meets something, so land columns take
 * nothing at all and a cave under the shelf stays dry until the player opens
 * it — the fill follows the open water, it does not flood the map. */
static void GenerateSea(World *world)
{
    int seaLevel = (int)WorldSeaLevelY(world);
    int x;

    if (world->height < 96) return;
    for (x = 0; x < world->width; ++x) {
        int y;

        for (y = seaLevel; y < world->height; ++y) {
            if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) break;
            WorldSetGeneratedCell(world, x, y, MATERIAL_WATER);
        }
    }

    /* Then the bed it stands in.
     *
     * Caves are dug long before the sea is poured and they reach close under
     * the shelf, so a cave mouth in the sea floor is a hole in the bottom of
     * the ocean: the whole sea drains into the cave system on the first tick,
     * which on a map this size means most of the water in the world going
     * somewhere the player will never look. Every empty cell touching the sea
     * from below or from the side becomes rock, exactly the way an underground
     * pocket is lined. Nothing above the water line is touched, so the surface
     * is still open sky.
     *
     * A second pass rather than part of the fill: the column to the right has
     * not been poured yet while the first pass is walking left to right, and
     * sealing it would wall off the sea from its own next column. */
    for (x = 0; x < world->width; ++x) {
        int y;

        for (y = seaLevel; y < world->height; ++y) {
            if (WorldMaterialAt(world, x, y) != MATERIAL_WATER) break;
            if (WorldInBounds(world, x - 1, y) &&
                WorldMaterialAt(world, x - 1, y) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, x - 1, y, MATERIAL_ROCK);
            }
            if (WorldInBounds(world, x + 1, y) &&
                WorldMaterialAt(world, x + 1, y) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, x + 1, y, MATERIAL_ROCK);
            }
            if (WorldInBounds(world, x, y + 1) &&
                WorldMaterialAt(world, x, y + 1) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, x, y + 1, MATERIAL_ROCK);
            }
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

    /* From the top of the ground band, not of the world: the generator puts
       nothing in the sky above it but the islands, which are made last, and
       reading three thousand rows of untouched sky per column was most of
       the time it took to make a world. */
    for (y = WorldSkyRows(world); y < world->height; ++y) {
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

/* The ragged edge of a canopy is drawn by a hash, and a hash left to itself
   strands single cells in mid air: a leaf with nothing touching it is not
   foliage, it is a dead pixel. Sweeping the box afterwards is the cheapest
   honest fix — the erosion stays random, and nothing survives it alone. */
static void FloraSweepStranded(World *world, int firstX, int firstY, int lastX,
                               int lastY, CellMaterial material)
{
    int y;

    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
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

    FloraSweepStranded(world, centerX - radiusX, centerY - radiusY,
                       centerX + radiusX, centerY + radiusY, material);
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
           leaning away from, which is where a real one carries its weight. The
           taper runs along the limb as well as between levels: a trunk that is
           one thickness from root to fork is a post with a shape on top. */
        int here = thickness - (thickness - 1) * step / steps;

        for (side = 1; side < here; ++side) {
            int offsetX = cellX + (angle > -1.5708f ? -side : side);

            if (WorldInBounds(world, offsetX, cellY) &&
                WorldMaterialAt(world, offsetX, cellY) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, offsetX, cellY, MATERIAL_WOOD);
            }
        }
        x += cosf(angle);
        y += sinf(angle);
        /* Crooked, not curved: the drift is redrawn every step. On top of it a
           slow pull back toward the sky, because a limb that only wanders
           drifts flat — every branch ends up horizontal, every crown ends up a
           pad balanced on a pole, and every tree in the forest is the same
           tree. Growing back toward the light is what gives a crown its
           height. */
        angle += ((float)RngRange(rng, -100, 100) / 100.0f) * 0.10f +
                 (-1.5708f - angle) * 0.045f;
    }

    /* Foliage on the last three levels rather than only on the tips. Hung on
       the tips alone it forms a shell at one distance from the root and the
       tree reads as an umbrella; hung on every level down to the fork it fills
       the crown with clumps at three sizes, which is what gives it depth
       instead of an outline. Each level inward carries a smaller clump, so the
       crown still thins outward. */
    if (depth <= 2 && canopy != MATERIAL_EMPTY && canopyRadius > 0) {
        static const float shrink[3] = {1.0f, 0.70f, 0.45f};
        int radius = (int)((float)canopyRadius * shrink[depth]);

        if (radius > 0) {
            FloraFillDisc(world, (int)floorf(x), (int)floorf(y), radius, radius,
                          canopy, world->seed, RngRange(rng, 0, 255));
        }
    }
    if (depth == 0) {
        return;
    }

    {
        int forks = RngRange(rng, 2, 3);
        int fork;

        for (fork = 0; fork < forks; ++fork) {
            /* Evenly spaced and then knocked off it. A perfectly symmetric
               split at every node is what makes a procedural tree look
               procedural: the eye reads the rule before it reads the tree. */
            float spread = ((float)fork / (float)(forks - 1)) - 0.5f +
                           (float)RngRange(rng, -20, 20) * 0.01f;
            float turn = spread * (0.9f + (float)RngRange(rng, 0, 40) * 0.01f);
            float shorter = length * (0.52f + (float)RngRange(rng, 0, 22) * 0.01f);

            FloraGrowLimb(world, x, y, angle + turn, shorter, depth - 1,
                          thickness > 1 ? thickness - 1 : 1, rng, canopy,
                          canopyRadius);
        }
    }
}

/* A tree that spreads: a leaning trunk that divides four times, with foliage
   only where the limbs end.
 *
 * Four divisions rather than three, and a stem no longer than it was. Reaching
 * the new height by lengthening the first limb instead produced a bare pole
 * with a tuft on top — an umbrella, not a broadleaf. Height belongs to the
 * branching: the crown is where a tree keeps its size. */
static void FloraPlaceBroadleaf(World *world, int x, int groundY, Rng *rng,
                                int trunkHeight, int canopyRadius,
                                CellMaterial canopy)
{
    float lean = (float)RngRange(rng, -22, 22) * 0.01f;
    /* A bare trunk divides once less: without foliage the extra level is a
       thicket of twigs nobody can read, and a dead tree is a silhouette. */
    /* Three levels or four, decided per tree: a stand where every trunk
       divides the same number of times is a stand of one tree repeated. */
    int depth = canopy == MATERIAL_EMPTY ? 3 : RngRange(rng, 3, 4);

    if (!FloraSpaceIsClear(world, x, groundY - 1, 1, trunkHeight / 2)) {
        return;
    }
    /* The first limb is a fraction of the tree's height, not the whole of it.
       Given the full height it produced a bare pole with everything happening
       at the top; the crown is supposed to start where the trunk first
       divides, and the rest of the height comes from the divisions. */
    FloraGrowLimb(world, (float)x + 0.5f, (float)groundY - 0.5f,
                  -1.5708f + lean, (float)trunkHeight * 0.62f, depth, 5, rng,
                  canopy, canopyRadius);
}

/* A conifer: a spine with tiers of needles hung on it.
 *
 * It used to be a bare pole with short horizontal wooden limbs and a small
 * clump of leaves on the end of each, and on screen that is a ladder rather
 * than a tree: the trunk was the most visible thing about it and the foliage
 * read as beads threaded onto it. What makes a pine is the opposite — the
 * needles are the silhouette, and the trunk is barely visible through them.
 *
 * So the crown is drawn as rows rather than as limbs. The half-width grows from
 * the tip toward the base, and inside each tier it starts narrow and widens
 * before dropping back at the next tier: that step is the drooping layer a pine
 * is made of, and it is what keeps the edge a saw instead of a straight cone.
 * The edge is eaten by the same hash the canopies and the beams use, so no two
 * pines on a slope are the same pine. */
static void FloraPlaceConifer(World *world, int x, int groundY, Rng *rng,
                              int trunkHeight, CellMaterial canopy)
{
    int top = groundY - trunkHeight;
    /* A bare bole under the crown, so the tree stands on something rather than
       sitting on the ground like a bush. */
    int bole = trunkHeight / 7 + 2;
    int crownBottom = groundY - 1 - bole;
    int tierHeight = 3 + trunkHeight / 20;
    float maxReach = 2.0f + (float)trunkHeight * 0.20f;
    int salt = RngRange(rng, 0, 255);
    int y;

    if (!FloraSpaceIsClear(world, x, groundY - 1, 1, trunkHeight / 2)) {
        return;
    }

    /* The bole first, two cells thick, and it stops at whatever it meets: a
       trunk does not grow through a cliff. */
    for (y = groundY - 1; y > crownBottom; --y) {
        int offset;

        if (!WorldInBounds(world, x, y)) return;
        if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) break;
        for (offset = 0; offset < 2; ++offset) {
            if (!WorldInBounds(world, x + offset, y)) continue;
            if (WorldMaterialAt(world, x + offset, y) != MATERIAL_EMPTY) {
                continue;
            }
            WorldSetGeneratedCell(world, x + offset, y, MATERIAL_WOOD);
        }
    }

    if (canopy == MATERIAL_EMPTY || crownBottom <= top + 2) {
        /* A bare spine is all a dead conifer is, so it is drawn and that is
           the whole tree. */
        for (y = crownBottom; y >= top; --y) {
            if (!WorldInBounds(world, x, y)) return;
            if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) break;
            WorldSetGeneratedCell(world, x, y, MATERIAL_WOOD);
        }
        return;
    }

    for (y = top; y <= crownBottom; ++y) {
        float along = (float)(y - top) / (float)(crownBottom - top);
        int tier = (crownBottom - y) % tierHeight;
        float within = 1.0f - (float)tier / (float)tierHeight;
        float half = (0.8f + along * maxReach) * (0.50f + 0.50f * within);
        int reach = (int)half;
        int offset;

        for (offset = -reach; offset <= reach; ++offset) {
            int cellX = x + offset;
            float edge = half > 0.001f
                             ? (float)(offset < 0 ? -offset : offset) / half
                             : 1.0f;

            if (!WorldInBounds(world, cellX, y)) continue;
            if (WorldMaterialAt(world, cellX, y) != MATERIAL_EMPTY) continue;
            if (edge > 0.5f &&
                PatchUnit(world->seed, cellX * 3 + salt, y * 3) <
                    (edge - 0.5f) * 1.5f) {
                continue;
            }
            WorldSetGeneratedCell(world, cellX, y, canopy);
        }
    }

    FloraSweepStranded(world, x - (int)maxReach - 2, top,
                       x + (int)maxReach + 2, crownBottom, canopy);

    /* The spine inside the crown, drawn last. Drawn first it was a solid brown
       line running the whole height of the tree, which is the one thing a pine
       never shows; drawn only into what the needles left empty it disappeared
       altogether, which is the other. So it takes the empty cells, and also the
       one row of each tier where the foliage is at its thinnest — the gap
       between two layers is exactly where a real trunk is visible. */
    for (y = crownBottom; y >= top; --y) {
        bool gap = (crownBottom - y) % tierHeight == tierHeight - 1;

        if (!WorldInBounds(world, x, y)) break;
        if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY &&
            !(gap && WorldMaterialAt(world, x, y) == canopy)) {
            continue;
        }
        WorldSetGeneratedCell(world, x, y, MATERIAL_WOOD);
    }
}

/* One vertical run of a cactus, `width` cells across, from `topY` down to
   `bottomY`. Nothing here overwrites: a limb meeting the trunk stops rather
   than carving into it. */
static void FloraCactusColumn(World *world, int x, int topY, int bottomY,
                              int width)
{
    int y;

    for (y = topY; y <= bottomY; ++y) {
        int offset;

        for (offset = 0; offset < width; ++offset) {
            if (!WorldInBounds(world, x + offset, y)) continue;
            if (WorldMaterialAt(world, x + offset, y) != MATERIAL_EMPTY) {
                continue;
            }
            WorldSetGeneratedCell(world, x + offset, y, MATERIAL_CACTUS);
        }
    }
    /* Rounded, not sawn off: the top cell of the outermost rib is dropped, so a
       trunk and an arm both end in a dome rather than in a flat lid. */
    if (width > 2 && WorldInBounds(world, x + width - 1, topY) &&
        WorldMaterialAt(world, x + width - 1, topY) == MATERIAL_CACTUS) {
        WorldSetGeneratedCell(world, x + width - 1, topY, MATERIAL_EMPTY);
    }
}

/* A saguaro: a thick ribbed column with one or two elbowed arms.
 *
 * It used to be a single cell wide with a hook on it, and at that width a
 * cactus is not a plant, it is a green line — the desert's one landmark read as
 * a scratch on the screen. Everything here is at least two cells thick, and the
 * arms turn a corner rather than sprouting sideways, because the corner is the
 * whole silhouette: it is what the eye names a cactus by.
 *
 * The ribs are the same trick as the broken edge of a beam and the eaten edge
 * of a canopy — a hash decides them rather than a drawing — so no two cacti in
 * a dune field are the same cactus. */
static void FloraPlaceCactus(World *world, int x, int groundY, Rng *rng,
                             int height)
{
    int width = RngRange(rng, 0, 99) < 55 ? 4 : 3;
    int top = groundY - height;
    int arms = RngRange(rng, 0, 99) < 68 ? 2 : 1;
    int arm;
    int side = RngRange(rng, 0, 1) == 0 ? -1 : 1;
    int y;

    /* Trunk-width clearance only. Demanding room for the arms as well is what
       once made trees vanish from every slope: any hillside violates a box as
       wide as the plant, and the limbs already stop at whatever they meet. */
    if (!FloraSpaceIsClear(world, x, groundY - 1, width / 2, height)) {
        return;
    }
    FloraCactusColumn(world, x, top, groundY - 1, width);

    for (arm = 0; arm < arms; ++arm) {
        /* Arms are hung on the lower half of the trunk and never at the same
           height, so a two-armed cactus is lopsided the way a real one is. */
        int elbowY = groundY - height / 2 + RngRange(rng, -2, 4) - arm * 3;
        /* How far out the elbow sits, then how far up the arm climbs from it.
           The climb is measured against what is left of the trunk above the
           elbow, so an arm never overtops its own plant. */
        int reach = RngRange(rng, 4, 8);
        int rise = RngRange(rng, 7, 15);
        int armWidth = width > 3 ? 3 : 2;
        int armX = side > 0 ? x + width - 1 + reach : x - reach - armWidth + 1;
        int armTop = elbowY - rise;

        if (armTop < top + 2) armTop = top + 2;
        if (elbowY >= groundY - 2 || armTop >= elbowY - 1) continue;

        /* The horizontal run out to the elbow, two cells deep so the corner has
           a thickness rather than being a single line of pixels. */
        for (y = elbowY; y < elbowY + armWidth; ++y) {
            int step;
            int from = side > 0 ? x + width : armX;
            int to = side > 0 ? armX + armWidth - 1 : x - 1;

            for (step = from; step <= to; ++step) {
                if (!WorldInBounds(world, step, y)) continue;
                if (WorldMaterialAt(world, step, y) != MATERIAL_EMPTY) continue;
                WorldSetGeneratedCell(world, step, y, MATERIAL_CACTUS);
            }
        }
        FloraCactusColumn(world, armX, armTop, elbowY + armWidth - 1, armWidth);
        side = -side;
    }

    /* Ribs: a shallow notch bitten out of the sides, never out of the middle,
       so the column keeps its spine and gains a surface. */
    for (y = top + 1; y < groundY - 1; ++y) {
        int edge;

        for (edge = 0; edge < 2; ++edge) {
            int cellX = edge == 0 ? x : x + width - 1;

            if (width < 3) break;
            if (!WorldInBounds(world, cellX, y)) continue;
            if (WorldMaterialAt(world, cellX, y) != MATERIAL_CACTUS) continue;
            if (PatchUnit(world->seed, cellX * 5, y * 3 + edge) < 0.24f) {
                WorldSetGeneratedCell(world, cellX, y, MATERIAL_EMPTY);
            }
        }
    }
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
                                        RngRange(&rng, 12, 21),
                                        RngRange(&rng, 4, 8), MATERIAL_LEAF);
                }
                /* Grass on almost every exposed cell of soil: it is the
                   cheapest thing that makes ground read as living. */
                if (RngRange(&rng, 0, 99) < 86 &&
                    WorldMaterialAt(world, x, surface - 1) == MATERIAL_EMPTY) {
                    int tuft = RngRange(&rng, 0, 99) < 34 ? 3 : 2;
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
                /* Sparse, but not so sparse that a screen of desert holds
                   none: a cactus is the only landmark a dune field has. */
                if (RngRange(&rng, 0, 999) < 28) {
                    FloraPlaceCactus(world, x, surface, &rng,
                                     RngRange(&rng, 17, 32));
                }
                break;
            case WORLD_BIOME_FROST:
                if (ground != MATERIAL_ICE && ground != MATERIAL_DIRT &&
                    ground != MATERIAL_SNOW) {
                    break;
                }
                /* Pines: a narrow, tall canopy that reaches most of the way down
                   the trunk, which is what separates them from the broadleaf. */
                if (RngRange(&rng, 0, 999) < 30) {
                    FloraPlaceConifer(world, x, surface, &rng,
                                      RngRange(&rng, 30, 54), MATERIAL_LEAF);
                }
                break;
            case WORLD_BIOME_VOLCANIC:
                if (ground != MATERIAL_ROCK) break;
                /* Dead trunks: the same branching with nothing hanging on
                   it. The ember wastes are what the other biomes look like
                   after they have burned. */
                if (RngRange(&rng, 0, 999) < 30) {
                    FloraPlaceBroadleaf(world, x, surface, &rng,
                                        RngRange(&rng, 13, 24), 0,
                                        MATERIAL_EMPTY);
                }
                break;
            case WORLD_BIOME_OCEAN:
                /* Nothing grows on the shelf. Every plant here is placed into
                   empty air above the ground it stands on, and there is no
                   empty air under the sea — the clearance test rejects the
                   water, so this is a statement of intent rather than a
                   guard. */
                break;
            case WORLD_BIOME_COUNT:
                break;
        }
    }
}


/* ---- the deep world ------------------------------------------------------
 *
 * Below the soil the world used to be one rock with holes in it. Now it has a
 * geology the player can read as they dig: loose lenses of sand and gravel
 * that pour out when opened, great caverns with something in each of them —
 * a lake, a grove of glowing mushrooms, a grotto of crystal, a hall of lava —
 * and tunnels that wind down from the surface to find them.
 */

/* Replaces the static solid cells of an ellipse — never air, never liquid,
   never anything loose — with `fill`: a lens is a pocket inside the rock, not
   a blob pasted over a cave. */
static void WorldReplaceEllipse(World *world, int centerX, int centerY, int radiusX,
                                int radiusY, CellMaterial fill)
{
    int y;

    if (radiusX <= 0 || radiusY <= 0) return;
    for (y = centerY - radiusY; y <= centerY + radiusY; ++y) {
        int x;

        if (!WorldInBounds(world, centerX, y)) continue;
        for (x = centerX - radiusX; x <= centerX + radiusX; ++x) {
            float dx = (float)(x - centerX) / (float)radiusX;
            float dy = (float)(y - centerY) / (float)radiusY;
            CellMaterial there;

            if (dx * dx + dy * dy > 1.0f) continue;
            there = WorldMaterialAt(world, x, y);
            if (!MaterialIsSolid(there) || MaterialIsDynamic(there) ||
                there == MATERIAL_BASALT) {
                continue;
            }
            WorldSetGeneratedCell(world, x, y, fill);
        }
    }
}

#define LENS_FEATURE_SPACING 150

static void GenerateLenses(World *world)
{
    int featureCount = (world->width + LENS_FEATURE_SPACING - 1) / LENS_FEATURE_SPACING;
    int feature;

    if (world->height < 160 || world->width < 64) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature, GENERATION_LENSES);
        int centerX = PositiveModulo(feature * LENS_FEATURE_SPACING +
                                         RngRange(&rng, 0, LENS_FEATURE_SPACING - 1),
                                     world->width);
        int surfaceY = SurfaceHeightAt(world, centerX);
        int deepest = (int)WorldGroundY(world, 0.80f);
        WorldBiome biome = WorldBiomeAt(world, centerX);
        CellMaterial fill;

        if (surfaceY + 24 >= deepest) continue;
        /* Sand where the land is sandy, gravel — rubble — everywhere else. */
        fill = biome == WORLD_BIOME_DUNES || biome == WORLD_BIOME_OCEAN ||
                       RngRange(&rng, 0, 99) < 30
                   ? MATERIAL_SAND
                   : MATERIAL_RUBBLE;
        WorldReplaceEllipse(world, centerX, RngRange(&rng, surfaceY + 24, deepest),
                            RngRange(&rng, 6, 18), RngRange(&rng, 3, 8), fill);
    }
}

typedef enum CavernTheme {
    CAVERN_PLAIN = 0,
    CAVERN_LAKE,
    CAVERN_GROVE,
    CAVERN_CRYSTAL,
    CAVERN_LAVA,
} CavernTheme;

#define CAVERN_FEATURE_SPACING 900

/* A spike of `material` hanging from (x, y) when `direction` is 1, or rising
   from it when -1: two cells wide at its root, tapering to one. */
static void WorldPlaceSpike(World *world, int x, int y, int length, int direction,
                            CellMaterial material)
{
    int step;

    for (step = 0; step < length; ++step) {
        int row = y + step * direction;
        int half = (length - step) * 2 / length;
        int column;

        for (column = x - half; column <= x + half; ++column) {
            if (!WorldInBounds(world, column, row)) continue;
            if (WorldMaterialAt(world, column, row) != MATERIAL_EMPTY) continue;
            WorldSetGeneratedCell(world, column, row, material);
        }
    }
}

/* A mushroom: a pale stalk and a glowing dome of cap. */
static void WorldPlaceMushroom(World *world, int x, int floorY, int height, int capRadius)
{
    int y;
    int top = floorY - height;

    for (y = floorY - 1; y > top; --y) {
        if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) return;
    }
    for (y = floorY - 1; y > top; --y) {
        WorldSetGeneratedCell(world, x, y, MATERIAL_WOOD);
        if (height > 10) {
            WorldSetGeneratedCell(world, x + 1, y, MATERIAL_WOOD);
        }
    }
    for (y = top - capRadius / 2; y <= top; ++y) {
        int column;
        float across = (float)(top - y) / (float)(capRadius / 2 + 1);
        int half = (int)((float)capRadius * sqrtf(1.0f - across * across));

        for (column = x - half; column <= x + half + (height > 10 ? 1 : 0); ++column) {
            if (!WorldInBounds(world, column, y)) continue;
            if (WorldMaterialAt(world, column, y) != MATERIAL_EMPTY) continue;
            WorldSetGeneratedCell(world, column, y, MATERIAL_FUNGUS);
        }
    }
}

static void GenerateCaverns(World *world)
{
    int featureCount =
        (world->width + CAVERN_FEATURE_SPACING - 1) / CAVERN_FEATURE_SPACING;
    int feature;

    if (world->height < 400 || world->width < 256) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature, GENERATION_CAVERNS);
        int centerX = PositiveModulo(feature * CAVERN_FEATURE_SPACING +
                                         CAVERN_FEATURE_SPACING / 2 +
                                         RngRange(&rng, -200, 200),
                                     world->width);
        int radiusX = RngRange(&rng, 70, 150);
        int radiusY = RngRange(&rng, 28, 58);
        int top = SurfaceHeightAt(world, centerX) + radiusY + 60;
        int bottom = (int)WorldGroundY(world, 0.95f) - radiusY;
        int centerY;
        bool deep;
        CavernTheme theme;
        CellMaterial host;
        int floorLevel;
        int dx;

        if (IsNearSpawn(world, centerX) || top >= bottom) continue;
        centerY = RngRange(&rng, top, bottom);
        deep = centerY > (int)WorldGroundY(world, 0.80f);
        host = deep ? MATERIAL_BASALT : MATERIAL_ROCK;
        if (deep) {
            theme = RngRange(&rng, 0, 99) < 55 ? CAVERN_CRYSTAL : CAVERN_LAVA;
        } else {
            int roll = RngRange(&rng, 0, 99);

            theme = roll < 35 ? CAVERN_LAKE : (roll < 75 ? CAVERN_GROVE : CAVERN_PLAIN);
        }

        /* A lake or a lava hall is lined first, like a pocket, so what it
           holds stays in it when a tunnel or a cave comes near. */
        if (theme == CAVERN_LAKE || theme == CAVERN_LAVA) {
            for (dx = -radiusX - 4; dx <= radiusX + 4; ++dx) {
                float t = (float)dx / (float)(radiusX + 4);
                int depthBelow = (int)((float)(radiusY / 2 + 6) * sqrtf(fmaxf(0.0f, 1.0f - t * t)));
                int y;

                for (y = centerY; y <= centerY + depthBelow; ++y) {
                    if (WorldInBounds(world, centerX + dx, y)) {
                        WorldSetGeneratedCell(world, centerX + dx, y, host);
                    }
                }
            }
        }

        /* The hall: a dome over a flatter floor, its outline broken by noise
           so it is a cave and not an ellipse. */
        floorLevel = centerY + radiusY / 2;
        for (dx = -radiusX; dx <= radiusX; ++dx) {
            float t = (float)dx / (float)radiusX;
            float shape = sqrtf(fmaxf(0.0f, 1.0f - t * t));
            float rough = GenerationUnit(world->seed, centerX + dx / 6, feature,
                                         GENERATION_CAVERNS);
            int ceiling = centerY - (int)((float)radiusY * shape * (0.78f + 0.22f * rough));
            int floorY = centerY + (int)((float)(radiusY / 2) * shape);
            int y;

            for (y = ceiling; y <= floorY; ++y) {
                if (WorldInBounds(world, centerX + dx, y)) {
                    WorldSetGeneratedCell(world, centerX + dx, y, MATERIAL_EMPTY);
                }
            }
            if (dx % 9 == 0 && shape > 0.3f && RngRange(&rng, 0, 99) < 55) {
                WorldPlaceSpike(world, centerX + dx, ceiling - 1,
                                RngRange(&rng, 3, 4 + (int)(10.0f * shape)), 1,
                                theme == CAVERN_CRYSTAL ? MATERIAL_CRYSTAL : host);
            }
        }

        switch (theme) {
        case CAVERN_LAKE:
        case CAVERN_LAVA: {
            /* Filled from the floor to a level line below the centre. */
            int level = centerY + radiusY / 6;
            CellMaterial liquid = theme == CAVERN_LAKE ? MATERIAL_WATER : MATERIAL_LAVA;

            for (dx = -radiusX; dx <= radiusX; ++dx) {
                int y;

                for (y = level; y <= floorLevel + 2; ++y) {
                    if (WorldInBounds(world, centerX + dx, y) &&
                        WorldMaterialAt(world, centerX + dx, y) == MATERIAL_EMPTY) {
                        WorldSetGeneratedCell(world, centerX + dx, y, liquid);
                    }
                }
            }
            break;
        }
        case CAVERN_GROVE:
            for (dx = -radiusX + 8; dx <= radiusX - 8; dx += RngRange(&rng, 7, 16)) {
                int x = centerX + dx;
                int y = centerY;

                while (y < centerY + radiusY &&
                       WorldMaterialAt(world, x, y + 1) == MATERIAL_EMPTY) {
                    ++y;
                }
                if (y >= centerY + radiusY) continue;
                WorldPlaceMushroom(world, x, y + 1, RngRange(&rng, 5, 18),
                                   RngRange(&rng, 3, 8));
            }
            break;
        case CAVERN_CRYSTAL:
            for (dx = -radiusX + 6; dx <= radiusX - 6; dx += RngRange(&rng, 5, 13)) {
                int x = centerX + dx;
                int y = centerY;

                while (y < centerY + radiusY &&
                       WorldMaterialAt(world, x, y + 1) == MATERIAL_EMPTY) {
                    ++y;
                }
                if (y >= centerY + radiusY) continue;
                WorldPlaceSpike(world, x, y, RngRange(&rng, 5, 22), -1,
                                MATERIAL_CRYSTAL);
            }
            break;
        case CAVERN_PLAIN:
        default:
            break;
        }
    }
}

#define TUNNEL_FEATURE_SPACING 1300

/* Tunnels that wind down from a mouth on the surface: a walk that leans
   downward and wanders, its bore swelling and narrowing as it goes. What
   makes a cave system read as one is that the caves are joined, and this is
   what joins the surface to them. */
static void GenerateTunnels(World *world)
{
    int featureCount =
        (world->width + TUNNEL_FEATURE_SPACING - 1) / TUNNEL_FEATURE_SPACING;
    int feature;

    if (world->height < 400 || world->width < 256) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature, GENERATION_TUNNELS);
        float x = (float)PositiveModulo(feature * TUNNEL_FEATURE_SPACING +
                                            RngRange(&rng, 0, TUNNEL_FEATURE_SPACING - 1),
                                        world->width);
        float y;
        float angle = 1.2f + (float)RngRange(&rng, 0, 70) / 100.0f;
        float deepest = WorldGroundY(world, 0.86f);
        int steps = RngRange(&rng, 260, 620);
        int step;

        if (IsNearSpawn(world, (int)x) ||
            WorldBiomeAt(world, (int)x) == WORLD_BIOME_OCEAN) {
            continue;
        }
        y = (float)SurfaceHeightAt(world, (int)x) - 2.0f;
        if (RngRange(&rng, 0, 1) == 0) {
            angle = PI - angle;
        }
        for (step = 0; step < steps; ++step) {
            float radius = 3.0f + 2.5f * (0.5f + 0.5f * sinf((float)step * 0.07f +
                                                         (float)feature));
            int cx = (int)x;
            int cy = (int)y;
            int r = (int)ceilf(radius);
            int oy;

            for (oy = -r; oy <= r; ++oy) {
                int ox;

                for (ox = -r; ox <= r; ++ox) {
                    if ((float)(ox * ox + oy * oy) > radius * radius) continue;
                    if (!WorldInBounds(world, cx + ox, cy + oy)) continue;
                    if (MaterialIsLiquid(WorldMaterialAt(world, cx + ox, cy + oy))) continue;
                    WorldSetGeneratedCell(world, cx + ox, cy + oy, MATERIAL_EMPTY);
                }
            }
            /* Wanders, leans back toward going down, and levels out near the
               deep band so it runs along it instead of into it. */
            angle += (float)RngRange(&rng, -18, 18) / 100.0f;
            if (y > deepest) {
                angle += (angle < PI * 0.5f ? -0.08f : 0.08f);
            } else {
                angle += (PI * 0.5f - angle) * 0.02f;
            }
            x += cosf(angle) * 2.0f;
            y += sinf(angle) * 2.0f;
            if (y >= (float)world->height - 20.0f) break;
        }
    }
}

/* Every underground pool held on every side it could leak from.
 *
 * The caverns are lined when they are dug, but everything dug after them —
 * the tunnels, the dungeons, the mine workings, the pockets — may pass a
 * cell away from their water, and a lake that meets a tunnel drains into it
 * on the first tick: what the player finds is a dry cavern and a flooded
 * corridor. The sea seals its own bed the same way. Only the chunks that
 * hold any liquid are looked at, which the per-chunk counts already know. */
static void SealUndergroundLiquids(World *world)
{
    int surfaceBand = (int)WorldGroundY(world, 0.0f);
    int *tops = malloc((size_t)world->width * sizeof(*tops));
    int chunkY;
    int column;

    /* The top of every column once: asked per liquid cell, the search from
       the top of the world costs more than the rest of generation. */
    if (tops == NULL) return;
    for (column = 0; column < world->width; ++column) {
        tops[column] = SurfaceSolidY(world, column);
    }

    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunk = WorldChunkIndex(world, chunkX, chunkY);
            int firstY = chunkY * WORLD_CHUNK_SIZE;
            int y;

            if (world->chunkWater[chunk] == 0u && world->chunkLava[chunk] == 0u) {
                continue;
            }
            for (y = firstY; y < firstY + WORLD_CHUNK_SIZE && y < world->height; ++y) {
                int x;

                if (y < surfaceBand) continue;
                for (x = chunkX * WORLD_CHUNK_SIZE;
                     x < (chunkX + 1) * WORLD_CHUNK_SIZE && x < world->width; ++x) {
                    CellMaterial liquid = WorldMaterialAt(world, x, y);
                    CellMaterial host;

                    if (!MaterialIsLiquid(liquid)) continue;
                    /* A pool open to the sky is a surface pool, held by its
                       own basin; the sealing is for the ones under a roof. */
                    if (y < tops[x]) continue;
                    host = y >= (int)WorldGroundY(world, 0.80f) ? MATERIAL_BASALT
                                                                : MATERIAL_ROCK;
                    if (WorldMaterialAt(world, x - 1, y) == MATERIAL_EMPTY) {
                        WorldSetGeneratedCell(world, x - 1, y, host);
                    }
                    if (WorldMaterialAt(world, x + 1, y) == MATERIAL_EMPTY) {
                        WorldSetGeneratedCell(world, x + 1, y, host);
                    }
                    if (WorldInBounds(world, x, y + 1) &&
                        WorldMaterialAt(world, x, y + 1) == MATERIAL_EMPTY) {
                        WorldSetGeneratedCell(world, x, y + 1, host);
                    }
                }
            }
        }
    }
    free(tops);
}

/* Snow on everything high enough, and on the whole of the frost: a few cells
   deep, laid on top of the ground as it finally is — over the ice of a frost
   lake, over the bare rock of a peak. The snowline is a height, not a biome:
   a range in the temperate lands is white at the top. */
static void GenerateSnow(World *world)
{
    int snowline = (int)WorldGroundY(world, 0.455f);
    int x;

    if (world->height < 160) return;
    for (x = 0; x < world->width; ++x) {
        int surface = SurfaceSolidY(world, x);
        WorldBiome biome;
        CellMaterial ground;
        int depth;
        int y;

        if (surface <= 8) continue;
        ground = WorldMaterialAt(world, x, surface);
        if (!MaterialIsSolid(ground) || MaterialIsFlora(ground)) continue;
        biome = WorldBiomeAt(world, x);
        if (surface < snowline) {
            depth = 2 + (snowline - surface) / 16;
        } else if (biome == WORLD_BIOME_FROST) {
            depth = 2 + (int)(GenerationUnit(world->seed, x / 5, 0, GENERATION_SNOW) * 3.0f);
        } else {
            continue;
        }
        if (depth > 8) depth = 8;
        for (y = surface - 1; y >= surface - depth; --y) {
            if (!WorldInBounds(world, x, y) || WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) {
                break;
            }
            WorldSetGeneratedCell(world, x, y, MATERIAL_SNOW);
        }
    }
}

int WorldGenSurfaceY(const World *world, int x)
{
    return SurfaceHeightAt(world, x);
}

int WorldGenSolidY(const World *world, int x)
{
    return SurfaceSolidY(world, x);
}

uint64_t WorldGenHash(uint64_t seed, int x, int y, uint64_t channel)
{
    return GenerationHash(seed, x, y, channel);
}

float WorldGenUnit(uint64_t seed, int x, int y, uint64_t channel)
{
    return GenerationUnit(seed, x, y, channel);
}

Rng WorldGenFeatureRng(uint64_t seed, int feature, uint64_t channel)
{
    Rng rng;

    RngSeed(&rng, GenerationHash(seed, feature, 0, channel));
    return rng;
}

bool WorldGenNearSpawn(const World *world, int x)
{
    return IsNearSpawn(world, x);
}

void WorldGenPlaceTree(World *world, int x, int groundY, Rng *rng)
{
    FloraPlaceBroadleaf(world, x, groundY, rng, RngRange(rng, 12, 21),
                        RngRange(rng, 4, 8), MATERIAL_LEAF);
}

void WorldGenerateBiomeTerrain(World *world)
{
    GenerateBaseTerrain(world);
    GenerateCaves(world);
    GenerateCaverns(world);
    GenerateTunnels(world);
    GenerateLenses(world);
    GenerateUndergroundFluids(world);
    GenerateSurfaceFeatures(world);
    GenerateSurfacePonds(world);
    /* Ruins and dungeons after the ground has its final shape, and before
       the sea is poured: a sunken temple fills with the sea around it. */
    WorldGenerateUnderground(world);
    WorldGenerateRuins(world);
    /* After every feature that could change the shape of the ground, so the
       coastline is the coastline the world actually ended up with. */
    GenerateSea(world);
    SealUndergroundLiquids(world);
    /* Before the plants, so a pine stands in the snow and the treeline is
       where the snow begins. */
    GenerateSnow(world);
    /* Last, so that every plant grows on the surface as it finally is rather
       than on one a later feature was going to bury. */
    GenerateFlora(world);
    /* The islands in the sky last: they carry their own soil, grass and
       trees, and nothing below should grow up into them. */
    WorldGenerateIslands(world);
}
