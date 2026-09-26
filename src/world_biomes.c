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
#include <string.h>

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
#define CAVE_FEATURE_SPACING 240
#define HYDROLOGY_FEATURE_SPACING 256
#define SURFACE_FEATURE_SPACING 900
/* Ponds have their own, much denser grid than the landmark features do. Making
   the landmark grid denser instead would have multiplied the mounds as well,
   and a world with a hill every two hundred cells is a different world. */
#define POND_FEATURE_SPACING 420
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
    [WORLD_BIOME_TEMPERATE] = {0.535f, 0.0400f, 0.0440f, 0.0100f, 0.220f, 0.0f},
    [WORLD_BIOME_DUNES] = {0.555f, 0.0300f, 0.0620f, 0.0140f, 0.110f, 0.052f},
    [WORLD_BIOME_FROST] = {0.500f, 0.0480f, 0.0520f, 0.0120f, 0.300f, 0.0f},
    [WORLD_BIOME_VOLCANIC] = {0.475f, 0.0520f, 0.0800f, 0.0200f, 0.240f, 0.0f},
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
    float ridge = 1.0f - fabsf(ValueNoise1D(world->seed, x, 900, world->width,
                                            GENERATION_RIDGES));
    float peaks = 0.65f + 0.35f * ValueNoise1D(world->seed, x, 320, world->width,
                                               GENERATION_PEAKS);

    return range * ridge * ridge * peaks;
}

/* The surface as a fraction of the ground band, before mountains: the plain a
   range rises from, which is what the soil depth is measured against. */
static float SurfaceBaseFraction(const World *world, int x,
                                 const BiomeSurfaceShape *shape)
{
    return shape->baseHeight +
           ValueNoise1D(world->seed, x, 2400, world->width, GENERATION_CONTINENT) *
               shape->continentAmplitude +
           ValueNoise1D(world->seed, x, 620, world->width, GENERATION_HILLS) *
               shape->hillAmplitude +
           ValueNoise1D(world->seed, x, 120, world->width, GENERATION_DETAIL) *
               shape->detailAmplitude;
}

static float SurfaceHeightRaw(const World *world, int x)
{
    BiomeSample sample = BiomeSampleAt(world, x);
    BiomeSurfaceShape shape = BlendedSurfaceShape(&sample);
    float fraction = SurfaceBaseFraction(world, x, &shape) -
                     MountainRelief(world, x) * shape.mountainAmplitude;

    /* Mesas: the ground rises in flat steps with steep sides, the step's rise
       squeezed into its last few cells. The steps are always the full size
       of the steppe's own and it is how much of them shows that blends
       across a border: blending the step size instead shrank it toward the
       border until the mesas became a saw of tiny teeth, a band of the
       desert oscillating like a sine wave. */
    if (shape.terrace > 0.0f) {
        const float full = BIOME_SURFACES[WORLD_BIOME_DUNES].terrace;
        float steps = fraction / full;
        float step = floorf(steps);
        float rise = steps - step;
        float amount = shape.terrace / full;

        rise = rise * rise * rise * rise * rise * rise;
        fraction = LerpFloat(fraction, (step + rise) * full, SmoothStep(amount));
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

        /* Above the sea, whatever the hills did there: a spawn is a place
           to stand. */
        float plateau = fminf(SurfaceHeightRaw(world, centerX),
                              WorldSeaLevelY(world) - 24.0f);

        height = LerpFloat(plateau, height, SmoothStep(amount));
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

static void WorldReplaceEllipse(World *world, int centerX, int centerY, int radiusX,
                                int radiusY, CellMaterial fill);

static void WorldPlacePocket(World *world, int centerX, int centerY,
                             int radiusX, int radiusY, CellMaterial fill)
{
    int waterLine = centerY + 2;
    int lastY = centerY + radiusY;
    int x;

    /* The rim is rock where there is ground to make it of; a cave that
       already runs past is left open, never walled off. */
    WorldReplaceEllipse(world, centerX, centerY, radiusX + 4, radiusY + 4,
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

static bool IsNearSpawn(const World *world, int x);

static void GenerateCaves(World *world)
{
    int featureCount =
        (world->width + CAVE_FEATURE_SPACING - 1) / CAVE_FEATURE_SPACING;
    int feature;

    if (world->height < 48 || world->width < 10) return;
    for (feature = 0; feature < featureCount; ++feature) {
        Rng rng = GenerationFeatureRng(world->seed, feature, GENERATION_CAVES);
        int centerX = feature * CAVE_FEATURE_SPACING +
                      CAVE_FEATURE_SPACING / 2 + RngRange(&rng, -40, 40);
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
                    radiusX = RngRange(&rng, 36, 100);
                    radiusY = RngRange(&rng, 28, 72);
                    stepX = RngRange(&rng, -72, 72);
                    stepY = RngRange(&rng, -36, 36);
                    break;
                case WORLD_BIOME_FROST:
                    radiusX = RngRange(&rng, 26, 60);
                    radiusY = RngRange(&rng, 44, 96);
                    stepX = RngRange(&rng, -36, 36);
                    stepY = RngRange(&rng, -64, 64);
                    break;
                case WORLD_BIOME_VOLCANIC:
                    radiusX = RngRange(&rng, 60, 136);
                    radiusY = RngRange(&rng, 26, 50);
                    stepX = RngRange(&rng, -96, 96);
                    stepY = RngRange(&rng, -24, 24);
                    break;
                case WORLD_BIOME_TEMPERATE:
                case WORLD_BIOME_COUNT:
                default:
                    radiusX = RngRange(&rng, 50, 120);
                    radiusY = RngRange(&rng, 28, 64);
                    stepX = RngRange(&rng, -82, 82);
                    stepY = RngRange(&rng, -36, 36);
                    break;
            }

            /* Under the surface by more than the lobe's own height: a lobe
               this size breaking out of the ground is a crater, not a cave.
               The tunnels are what open the caves to the sky. */
            {
                int under = SurfaceHeightAt(world, centerX) + radiusY + 16;

                if (centerY < under) centerY = under;
                if (centerY + radiusY >= world->height - 8 ||
                    IsNearSpawn(world, centerX)) {
                    centerX = PositiveModulo(centerX + stepX, world->width);
                    continue;
                }
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
        int radiusX;
        int radiusY;

        centerX = PositiveModulo(centerX, world->width);
        surfaceY = SurfaceHeightAt(world, centerX);
        biome = WorldBiomeAt(world, centerX);
        liquid = biome == WORLD_BIOME_VOLCANIC ? MATERIAL_LAVA : MATERIAL_WATER;
        radiusX = RngRange(&rng, 50, 96);
        radiusY = RngRange(&rng, 24, 48);
        /* Deep enough that the pocket and its lining stay under the ground
           however big the pocket is. */
        minimumY = surfaceY + WorldGroundRows(world) /
                                   (biome == WORLD_BIOME_DUNES ? 4 : 6) +
                   radiusY + 12;
        maximumY = world->height - 24 - radiusY;
        if (minimumY >= maximumY) continue;

        WorldPlacePocket(world, centerX, RngRange(&rng, minimumY, maximumY), radiusX,
                         radiusY, liquid);
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
                                           RngRange(&rng, 140, 250),
                                           RngRange(&rng, 44, 80),
                                           MATERIAL_WATER, false);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 76, 144),
                                    RngRange(&rng, 38, 86), 18,
                                    MATERIAL_DIRT);
                }
                break;
            case WORLD_BIOME_DUNES:
                if (feature % 4 == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 90, 154),
                                           RngRange(&rng, 28, 48),
                                           MATERIAL_WATER, false);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 134, 244),
                                    RngRange(&rng, 44, 100), 12,
                                    MATERIAL_SAND);
                }
                break;
            case WORLD_BIOME_FROST:
                if ((feature & 1) == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 134, 224),
                                           RngRange(&rng, 42, 74),
                                           MATERIAL_WATER, true);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 58, 108),
                                    RngRange(&rng, 58, 122), 10,
                                    MATERIAL_ICE);
                }
                break;
            case WORLD_BIOME_VOLCANIC:
                if ((feature & 1) == 0) {
                    WorldPlaceSurfaceBasin(world, centerX,
                                           RngRange(&rng, 100, 186),
                                           RngRange(&rng, 36, 64),
                                           MATERIAL_LAVA, false);
                } else {
                    WorldPlaceMound(world, centerX, RngRange(&rng, 70, 138),
                                    RngRange(&rng, 64, 140), 20,
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

        WorldPlaceSurfaceBasin(world, centerX, RngRange(&rng, 43, 90),
                               RngRange(&rng, 20, 40),
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
    int *queue;
    size_t capacity = 1u << 20;
    size_t head = 0;
    size_t tail = 0;
    int x;

    if (world->height < 96) return;
    queue = malloc(capacity * sizeof(*queue));
    if (queue == NULL) return;
    /* Poured from the open sea down into everything under the sea level
       that it can reach — the hollow of a wreck, the gap under a gateway's
       key-stone, a cave that opens in the sea floor — so that nothing the
       water touches is left to fill when the chunk is first streamed in.
       Nothing is walled off to keep it out. */
    for (x = 0; x < world->width; ++x) {
        int y;
        bool open = true;

        for (y = WorldSkyRows(world); y < seaLevel; ++y) {
            if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) {
                open = false;
                break;
            }
        }
        if (!open || WorldMaterialAt(world, x, seaLevel) != MATERIAL_EMPTY) continue;
        WorldSetGeneratedCell(world, x, seaLevel, MATERIAL_WATER);
        queue[tail++] = seaLevel * world->width + x;
    }
    while (head < tail) {
        int cell = queue[head++];
        int cellX = cell % world->width;
        int cellY = cell / world->width;
        static const int stepX[3] = {1, -1, 0};
        static const int stepY[3] = {0, 0, 1};
        int direction;

        for (direction = 0; direction < 3; ++direction) {
            int nextX = WorldWrapColumn(cellX + stepX[direction], world->width);
            int nextY = cellY + stepY[direction];

            if (nextY >= world->height ||
                WorldMaterialAt(world, nextX, nextY) != MATERIAL_EMPTY) {
                continue;
            }
            WorldSetGeneratedCell(world, nextX, nextY, MATERIAL_WATER);
            if (tail == capacity) {
                /* Compact what has been read, then grow if that was not
                   enough. */
                memmove(queue, queue + head, (tail - head) * sizeof(*queue));
                tail -= head;
                head = 0;
                if (tail == capacity) {
                    int *grown = realloc(queue, capacity * 2u * sizeof(*queue));

                    if (grown == NULL) {
                        free(queue);
                        return;
                    }
                    queue = grown;
                    capacity *= 2u;
                }
            }
            queue[tail++] = nextY * world->width + nextX;
        }
    }
    free(queue);
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
       nothing in the sky above it, and reading three thousand rows of untouched sky per column was most of
       the time it took to make a world. */
    for (y = WorldSkyRows(world); y < world->height; ++y) {
        CellMaterial material = WorldMaterialAt(world, x, y);

        /* A plant is never the ground: a blade leaning over from the next
           column is not where this column's soil is. */
        if (MaterialIsSolid(material) && !MaterialIsBackdrop(material)) return y;
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
        CellMaterial ahead;
        /* Thicker near the base, tapering along the limb as well as between
           levels: a trunk one thickness from root to fork is a post with a
           shape on top. */
        float here = (float)thickness -
                     ((float)thickness * 0.45f) * (float)step / (float)steps;
        float half = here * 0.5f;
        float across;

        if (!WorldInBounds(world, cellX, cellY)) return;
        ahead = WorldMaterialAt(world, cellX, cellY);
        /* A limb stops where it meets anything but the tree: it does not
           bore through a cliff. The tree itself is no obstacle — a thick limb
           that leans lays the cells of its next step while drawing this one,
           and a branch grows on through the leaves of the one before it;
           stopping at either cut every tree off at its first fork. */
        if (ahead != MATERIAL_EMPTY && !MaterialIsFlora(ahead) && step > 1) {
            break;
        }
        /* The cross-section, square to the limb and centred on it, so a
           trunk is round rather than a stair of cells hung off one side. */
        for (across = -half; across <= half + 0.001f; across += 0.5f) {
            int sideX = (int)floorf(x - sinf(angle) * across);
            int sideY = (int)floorf(y + cosf(angle) * across);

            if (WorldInBounds(world, sideX, sideY) &&
                WorldMaterialAt(world, sideX, sideY) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, sideX, sideY, MATERIAL_WOOD);
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

    /* Foliage on the last four levels rather than only on the tips. Hung on
       the tips alone it forms a shell at one distance from the root and the
       tree reads as an umbrella; hung on every level down to the fork it fills
       the crown with clumps at three sizes, which is what gives it depth
       instead of an outline. Each level inward carries a smaller clump, so the
       crown still thins outward. */
    if (depth <= 3 && canopy != MATERIAL_EMPTY && canopyRadius > 0) {
        static const float shrink[4] = {1.0f, 0.78f, 0.58f, 0.42f};
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

            /* A branch carries a little over half its parent's girth. */
            int girth = (int)((float)thickness * 0.6f + 0.5f);

            FloraGrowLimb(world, x, y, angle + turn, shorter, depth - 1,
                          girth > 1 ? girth : 1, rng, canopy, canopyRadius);
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
static void FloraPlaceBroadleafBody(World *world, int x, int groundY, Rng *rng,
                                int trunkHeight, int canopyRadius,
                                CellMaterial canopy)
{
    float lean = (float)RngRange(rng, -22, 22) * 0.01f;
    /* A bare trunk divides once less: without foliage the extra level is a
       thicket of twigs nobody can read, and a dead tree is a silhouette. */
    /* Three levels or four, decided per tree: a stand where every trunk
       divides the same number of times is a stand of one tree repeated. */
    int depth = canopy == MATERIAL_EMPTY ? 4 : RngRange(rng, 4, 5);

    if (!FloraSpaceIsClear(world, x, groundY - 1, 1, trunkHeight / 2)) {
        return;
    }
    /* The first limb is a fraction of the tree's height, not the whole of it.
       Given the full height it produced a bare pole with everything happening
       at the top; the crown is supposed to start where the trunk first
       divides, and the rest of the height comes from the divisions. */
    FloraGrowLimb(world, (float)x + 0.5f, (float)groundY - 0.5f,
                  -1.5708f + lean, (float)trunkHeight * 0.62f, depth, 11, rng,
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
/* Half the width of a pine's trunk `along` (0 at the foot, 1 at the tip)
   of a tree `height` tall: about a twentieth of the height at the foot,
   narrowing to a single cell at the top. */
static int ConiferGirth(int height, float along)
{
    float foot = (float)height / 40.0f + 1.0f;
    float half = foot * (1.0f - 0.85f * along);

    return half < 0.5f ? 0 : (int)(half + 0.5f);
}

static void FloraPlaceConiferBody(World *world, int x, int groundY, Rng *rng,
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
    /* Rooted through the snow into the ground under it. Standing on the
       snow itself, a pine was held by nothing — loose grains hold nothing
       up — and the first blast nearby carried it off. */
    {
        int root = groundY;
        int half = ConiferGirth(trunkHeight, 0.0f);
        int offset;

        while (root < world->height && root < groundY + 24 &&
               MaterialIsDynamic(WorldMaterialAt(world, x, root))) {
            for (offset = -half; offset <= half; ++offset) {
                if (MaterialIsDynamic(WorldMaterialAt(world, x + offset, root))) {
                    WorldSetGeneratedCell(world, x + offset, root, MATERIAL_WOOD);
                }
            }
            ++root;
        }
    }

    /* The bole first, and it stops at whatever it meets: a trunk does not
       grow through a cliff. Its girth follows the tree's height — a pine
       two hundred cells tall on a stem two cells wide was a feather duster
       — and narrows toward the top. */
    for (y = groundY - 1; y > crownBottom; --y) {
        int half;
        int offset;

        if (!WorldInBounds(world, x, y)) return;
        if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY &&
            !MaterialIsFlora(WorldMaterialAt(world, x, y))) {
            break;
        }
        half = ConiferGirth(trunkHeight, (float)(groundY - y) / (float)trunkHeight);
        for (offset = -half; offset <= half; ++offset) {
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
        int half = ConiferGirth(trunkHeight, (float)(groundY - y) / (float)trunkHeight);
        int offset;

        if (!WorldInBounds(world, x, y)) break;
        for (offset = -half; offset <= half; ++offset) {
            CellMaterial here = WorldMaterialAt(world, x + offset, y);

            if (here != MATERIAL_EMPTY && !(gap && here == canopy)) {
                continue;
            }
            WorldSetGeneratedCell(world, x + offset, y, MATERIAL_WOOD);
        }
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
static void FloraPlaceCactusBody(World *world, int x, int groundY, Rng *rng,
                             int height)
{
    int width = RngRange(rng, 0, 99) < 55 ? 15 : 13;
    int top = groundY - height;
    int arms = RngRange(rng, 0, 99) < 68 ? 2 : 1;
    int arm;
    int side = RngRange(rng, 0, 1) == 0 ? -1 : 1;
    int y;

    /* Trunk-width clearance only. Demanding room for the arms as well is what
       once made trees vanish from every slope: any hillside violates a box as
       wide as the plant, and the limbs already stop at whatever they meet. */
    if (!FloraSpaceIsClear(world, x + width / 2, groundY - 1, 1, height * 2 / 3)) {
        return;
    }
    FloraCactusColumn(world, x, top, groundY - 1, width);

    for (arm = 0; arm < arms; ++arm) {
        /* Arms are hung on the lower half of the trunk and never at the same
           height, so a two-armed cactus is lopsided the way a real one is. */
        int elbowY = groundY - height / 2 + RngRange(rng, -8, 16) - arm * 12;
        /* How far out the elbow sits, then how far up the arm climbs from it.
           The climb is measured against what is left of the trunk above the
           elbow, so an arm never overtops its own plant. */
        int reach = RngRange(rng, 16, 30);
        int rise = RngRange(rng, 30, 60);
        int armWidth = width > 13 ? 11 : 9;
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

/* A blade of grass, or none, from the soil at (x, groundY).
 *
 * A row of identical columns reads as a strip of plastic laid on the
 * ground. A meadow is patches — thick and tall in one place, short and thin
 * in the next, bare soil between — and every blade is its own height and
 * leans its own way, dark at the foot and bright at the tip, and now and
 * then carries a flower. The patches are noise along the ground, so they
 * are the same wherever the column is generated from; the blade's own
 * height, lean and flower come from the column's stream. The soil itself
 * stays soil: the blade stands on it. */
static void FloraGrowGrassBody(World *world, int x, int groundY, Rng *rng)
{
    float meadow = ValueNoise1D(world->seed, x, 46, world->width, GENERATION_DETAIL) * 0.5f +
                   0.5f;
    float lushness = meadow * (0.4f + 0.6f * meadow);
    int height;
    float lean;
    float drift = 0.0f;
    bool flower;
    int row;

    if (RngRange(rng, 0, 999) > (int)(420.0f + 580.0f * meadow)) {
        return;
    }
    height = 3 + (int)((float)RngRange(rng, 25, 100) * 0.01f * (5.0f + 17.0f * lushness));
    lean = (float)RngRange(rng, -45, 45) * 0.01f;
    flower = height > 6 && RngRange(rng, 0, 99) < 5;
    for (row = 1; row <= height; ++row) {
        int cellX = x + (int)floorf(drift + 0.5f);
        int cellY = groundY - row;
        float along = (float)row / (float)height;
        uint8_t shade;

        if (!WorldInBounds(world, cellX, cellY) ||
            WorldMaterialAt(world, cellX, cellY) != MATERIAL_EMPTY) {
            break;
        }
        WorldSetGeneratedCell(world, cellX, cellY, MATERIAL_GRASS);
        shade = (uint8_t)(6.0f + 54.0f * along * along + (float)RngRange(rng, 0, 4));
        if (flower && row == height) {
            shade = (uint8_t)RngRange(rng, 0, 1);
        }
        WorldSetShade(world, cellX, cellY, shade);
        /* The lean grows toward the tip: a blade bends, it is not planted
           at an angle. */
        drift += lean * along;
    }
}

/* A plant's identity: a hash of where it stands, never zero, so two trees
   whose crowns touch are still two trees. */
static uint16_t PlantIdAt(int x, int y)
{
    uint32_t value = (uint32_t)x * 0x9e3779b1u ^ (uint32_t)y * 0x85ebca77u;

    value ^= value >> 15;
    value *= 0x2c1b3c6du;
    value ^= value >> 12;
    return (uint16_t)((value & 0xfffeu) | 1u);
}

static void FloraPlaceBroadleaf(World *world, int x, int groundY, Rng *rng,
                                int trunkHeight, int canopyRadius,
                                CellMaterial canopy)
{
    world->generationPlant = PlantIdAt(x, groundY);
    FloraPlaceBroadleafBody(world, x, groundY, rng, trunkHeight, canopyRadius, canopy);
    world->generationPlant = 0u;
}

static void FloraPlaceConifer(World *world, int x, int groundY, Rng *rng,
                              int trunkHeight, CellMaterial canopy)
{
    world->generationPlant = PlantIdAt(x, groundY);
    FloraPlaceConiferBody(world, x, groundY, rng, trunkHeight, canopy);
    world->generationPlant = 0u;
}

static void FloraPlaceCactus(World *world, int x, int groundY, Rng *rng, int height)
{
    world->generationPlant = PlantIdAt(x, groundY);
    FloraPlaceCactusBody(world, x, groundY, rng, height);
    world->generationPlant = 0u;
}

static void FloraGrowGrass(World *world, int x, int groundY, Rng *rng)
{
    world->generationPlant = PlantIdAt(x, groundY);
    FloraGrowGrassBody(world, x, groundY, rng);
    world->generationPlant = 0u;
}

/* ---- the undergrowth -------------------------------------------------------

   What grows between the trees, each one a plant of its own (see PlantIdAt):
   bushes in the temperate lands, dry brush and the tumbleweeds the wind
   rolls on the dunes, low needle-bushes on the frost, ember blooms among the
   rocks of the wastes, kelp on the sea floor, vines hanging in the caves and
   glowing moss on their floors. */

/* Sets a cell to `material` when it is empty (or, for kelp, water). */
static void FloraPut(World *world, int x, int y, CellMaterial material)
{
    CellMaterial there;

    if (!WorldInBounds(world, x, y)) return;
    there = WorldMaterialAt(world, x, y);
    if (there == MATERIAL_EMPTY || (material == MATERIAL_KELP && there == MATERIAL_WATER)) {
        WorldSetGeneratedCell(world, x, y, material);
    }
}

/* A bush: a few short stems from one root and leaves heaped round them,
   low and wider than it is tall. */
static void FloraPlaceBush(World *world, int x, int groundY, Rng *rng, CellMaterial leaves)
{
    int stems = RngRange(rng, 2, 4);
    int stem;

    world->generationPlant = PlantIdAt(x, groundY);
    for (stem = 0; stem < stems; ++stem) {
        int height = RngRange(rng, 6, 16);
        int lean = RngRange(rng, -8, 8);
        int step;
        int tipX = x;
        int tipY = groundY - 1;

        for (step = 0; step < height; ++step) {
            tipX = x + lean * step / height;
            tipY = groundY - 1 - step;
            FloraPut(world, tipX, tipY, MATERIAL_WOOD);
        }
        FloraFillDisc(world, tipX, tipY, RngRange(rng, 6, 11), RngRange(rng, 4, 8), leaves,
                      world->seed, RngRange(rng, 0, 255));
    }
    world->generationPlant = 0u;
}

/* Dry brush: a tangle of thin twigs from one root. */
static void FloraPlaceDryShrub(World *world, int x, int groundY, Rng *rng)
{
    int twigs = RngRange(rng, 5, 9);
    int twig;

    world->generationPlant = PlantIdAt(x, groundY);
    for (twig = 0; twig < twigs; ++twig) {
        float angle = -1.5708f + (float)RngRange(rng, -80, 80) * 0.01f;
        float length = (float)RngRange(rng, 8, 20);
        float step;

        for (step = 0.0f; step < length; step += 0.7f) {
            float bend = sinf(step * 0.3f + (float)twig) * 1.5f;

            FloraPut(world, x + (int)(cosf(angle) * step + bend), groundY - 1 + (int)(sinf(angle) * step),
                     MATERIAL_DRYBRUSH);
        }
    }
    world->generationPlant = 0u;
}

/* A tumbleweed: a loose ball of brush sitting on the sand, remembered so the
   wind can take it. */
static void FloraPlaceTumbleweed(World *world, int x, int groundY, Rng *rng)
{
    int radius = RngRange(rng, 5, 8);
    int centreY = groundY - radius;
    int dy;

    if (world->tumbleweedCount >= WORLD_MAX_TUMBLEWEEDS) return;
    world->generationPlant = PlantIdAt(x, groundY);
    for (dy = -radius; dy <= radius; ++dy) {
        int dx;

        for (dx = -radius; dx <= radius; ++dx) {
            int distance = dx * dx + dy * dy;

            /* A shell of twigs round a hollow, with gaps in it. */
            if (distance > radius * radius || distance < (radius - 3) * (radius - 3)) continue;
            if (PatchUnit(world->seed, (x + dx) * 5, (centreY + dy) * 5) < 0.3f) continue;
            FloraPut(world, x + dx, centreY + dy, MATERIAL_DRYBRUSH);
        }
    }
    FloraPut(world, x, groundY - 1, MATERIAL_DRYBRUSH);
    world->generationPlant = 0u;
    world->tumbleweeds[world->tumbleweedCount++] =
        (WorldTumbleweed){(int16_t)radius, false, x, centreY};
}

/* An ember bloom: a charred stem and a glowing head, in clusters. */
static void FloraPlaceEmberBlooms(World *world, int x, int groundY, Rng *rng)
{
    int blooms = RngRange(rng, 1, 4);
    int bloom;

    for (bloom = 0; bloom < blooms; ++bloom) {
        int bx = x + RngRange(rng, -6, 6);
        int top = groundY - 1 - RngRange(rng, 4, 9);
        int y;

        world->generationPlant = PlantIdAt(bx, groundY + bloom);
        for (y = groundY - 1; y > top; --y) {
            FloraPut(world, bx, y, MATERIAL_WOOD);
        }
        FloraPut(world, bx, top, MATERIAL_EMBERBLOOM);
        FloraPut(world, bx - 1, top, MATERIAL_EMBERBLOOM);
        FloraPut(world, bx + 1, top, MATERIAL_EMBERBLOOM);
        FloraPut(world, bx, top - 1, MATERIAL_EMBERBLOOM);
    }
    world->generationPlant = 0u;
}

/* Kelp: strands up from the sea floor through the water, waving, with a
   blade off each side now and then. */
static void FloraPlaceKelp(World *world, int x, int floorY, Rng *rng)
{
    int height = RngRange(rng, 30, 110);
    float phase = (float)RngRange(rng, 0, 628) * 0.01f;
    int row;

    world->generationPlant = PlantIdAt(x, floorY);
    for (row = 1; row <= height; ++row) {
        int y = floorY - row;
        int cx = x + (int)(sinf((float)row * 0.12f + phase) * 3.0f);

        if (WorldMaterialAt(world, cx, y) != MATERIAL_WATER) break;
        FloraPut(world, cx, y, MATERIAL_KELP);
        FloraPut(world, cx + 1, y, MATERIAL_KELP);
        if (row % 7 == 3) {
            FloraPut(world, cx + 2, y - 1, MATERIAL_KELP);
            FloraPut(world, cx + 3, y - 2, MATERIAL_KELP);
        } else if (row % 7 == 6) {
            FloraPut(world, cx - 1, y - 1, MATERIAL_KELP);
            FloraPut(world, cx - 2, y - 2, MATERIAL_KELP);
        }
    }
    world->generationPlant = 0u;
}

/* A fallen log along the ground. */
static void FloraPlaceLog(World *world, int x, int groundY, Rng *rng)
{
    int length = RngRange(rng, 26, 56);
    int thick = RngRange(rng, 4, 6);
    int column;

    world->generationPlant = PlantIdAt(x, groundY);
    for (column = 0; column < length; ++column) {
        int top = WorldGenSolidY(world, x + column);
        int row;

        if (top < 0 || top < groundY - 6 || top > groundY + 6) break;
        for (row = 1; row <= thick; ++row) {
            FloraPut(world, x + column, top - row, MATERIAL_WOOD);
        }
    }
    world->generationPlant = 0u;
}

/* Vines from cave ceilings and moss on cave floors, wherever the ground is
   hollow: the one pass that walks the underground, at generation only. */
static void GenerateCaveGrowth(World *world)
{
    int x;
    int top = (int)WorldGroundY(world, 0.05f);

    for (x = 0; x < world->width; x += 3) {
        int y;

        if (IsNearSpawn(world, x)) continue;
        for (y = top; y < world->height - 1; ++y) {
            CellMaterial here = WorldMaterialAt(world, x, y);
            CellMaterial above;
            CellMaterial below;
            uint32_t roll;

            if (here != MATERIAL_EMPTY ||
                WorldBackWallAt(world, x, y) == MATERIAL_EMPTY) {
                continue;
            }
            above = WorldMaterialAt(world, x, y - 1);
            below = WorldMaterialAt(world, x, y + 1);
            roll = (uint32_t)(GenerationUnit(world->seed, x, y, GENERATION_SURFACE_FEATURES + 77u) *
                              1000.0f);
            /* Nothing live hangs in the caves of the ember wastes. */
            if (MaterialIsSolid(above) && !MaterialIsBackdrop(above) &&
                !MaterialIsDynamic(above) && roll < 45 &&
                WorldBiomeAt(world, x) != WORLD_BIOME_VOLCANIC) {
                /* A vine: a strand hanging, with leaves along it. */
                int length = 8 + (int)(roll % 40u);
                int step;

                world->generationPlant = PlantIdAt(x, y);
                for (step = 0; step < length; ++step) {
                    int vx = x + (int)(sinf((float)step * 0.35f) * 1.2f);

                    if (WorldMaterialAt(world, vx, y + step) != MATERIAL_EMPTY) break;
                    FloraPut(world, vx, y + step, MATERIAL_LEAF);
                }
                world->generationPlant = 0u;
            } else if (MaterialIsSolid(below) && !MaterialIsBackdrop(below) &&
                       !MaterialIsDynamic(below) && roll > 985) {
                /* A patch of glowing moss along the floor. */
                int length = 3 + (int)(roll % 7u);
                int step;

                world->generationPlant = PlantIdAt(x, y);
                for (step = 0; step < length; ++step) {
                    if (WorldMaterialAt(world, x + step, y) == MATERIAL_EMPTY &&
                        MaterialIsSolid(WorldMaterialAt(world, x + step, y + 1))) {
                        FloraPut(world, x + step, y, MATERIAL_FUNGUS);
                    }
                }
                world->generationPlant = 0u;
            }
        }
    }
}

/* Whether the ground at (x, y) is something built — a roof, a plinth, a
   hull — under at most a drift of snow. Nothing grows on a roof: a pine on
   the snow on an outpost's roof was a pine standing in the air. */
static bool GroundIsBuilt(const World *world, int x, int y)
{
    int depth;

    for (depth = 0; depth < 16; ++depth) {
        CellMaterial material = WorldMaterialAt(world, x, y + depth);

        if (material == MATERIAL_SNOW) continue;
        return material == MATERIAL_METAL || material == MATERIAL_RELIC ||
               material == MATERIAL_BRICK || material == MATERIAL_BASALT ||
               material == MATERIAL_LUMEN || MaterialIsBackdrop(material);
    }
    return false;
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

        if (surface <= 8 || IsNearSpawn(world, x) || GroundIsBuilt(world, x, surface)) {
            continue;
        }
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
                if (RngRange(&rng, 0, 999) < 16) {
                    FloraPlaceBroadleaf(world, x, surface, &rng,
                                        RngRange(&rng, 96, 150),
                                        RngRange(&rng, 12, 18), MATERIAL_LEAF);
                }
                /* The undergrowth: bushes, now and then a fallen log. */
                if (RngRange(&rng, 0, 999) < 22) {
                    FloraPlaceBush(world, x, surface, &rng, MATERIAL_LEAF);
                } else if (RngRange(&rng, 0, 999) < 4) {
                    FloraPlaceLog(world, x, surface, &rng);
                }
                /* Grass on almost every exposed cell of soil: it is the
                   cheapest thing that makes ground read as living. */
                FloraGrowGrass(world, x, surface, &rng);
                break;
            case WORLD_BIOME_DUNES:
                if (ground != MATERIAL_SAND) break;
                /* Sparse, but not so sparse that a screen of desert holds
                   none: a cactus is the only landmark a dune field has. */
                if (RngRange(&rng, 0, 999) < 22) {
                    FloraPlaceCactus(world, x, surface, &rng,
                                     RngRange(&rng, 76, 130));
                } else if (RngRange(&rng, 0, 999) < 16) {
                    FloraPlaceDryShrub(world, x, surface, &rng);
                } else if (RngRange(&rng, 0, 999) < 5) {
                    FloraPlaceTumbleweed(world, x, surface, &rng);
                }
                break;
            case WORLD_BIOME_FROST:
                if (ground != MATERIAL_ICE && ground != MATERIAL_DIRT &&
                    ground != MATERIAL_SNOW) {
                    break;
                }
                /* Pines: a narrow, tall canopy that reaches most of the way down
                   the trunk, which is what separates them from the broadleaf. */
                if (RngRange(&rng, 0, 999) < 8) {
                    FloraPlaceConifer(world, x, surface, &rng,
                                      RngRange(&rng, 130, 220), MATERIAL_LEAF);
                } else if (RngRange(&rng, 0, 999) < 14) {
                    /* Low needle-bushes, hunched against the cold. */
                    FloraPlaceBush(world, x, surface, &rng, MATERIAL_LEAF);
                }
                break;
            case WORLD_BIOME_VOLCANIC:
                if (ground != MATERIAL_ROCK) break;
                /* Dead trunks: the same branching with nothing hanging on
                   it. The ember wastes are what the other biomes look like
                   after they have burned. */
                if (RngRange(&rng, 0, 999) < 8) {
                    FloraPlaceBroadleaf(world, x, surface, &rng,
                                        RngRange(&rng, 64, 110), 0,
                                        MATERIAL_EMPTY);
                } else if (RngRange(&rng, 0, 999) < 30) {
                    FloraPlaceEmberBlooms(world, x, surface, &rng);
                }
                break;
            case WORLD_BIOME_OCEAN:
                /* Kelp from the sea floor, where the floor is under water. */
                if (WorldMaterialAt(world, x, surface - 1) == MATERIAL_WATER &&
                    RngRange(&rng, 0, 999) < 40) {
                    FloraPlaceKelp(world, x, surface, &rng);
                }
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

#define CAVERN_FEATURE_SPACING 1400

/* A spike of `material` hanging from (x, y) when `direction` is 1, or rising
   from it when -1: two cells wide at its root, tapering to one. */
static void WorldPlaceSpike(World *world, int x, int y, int length, int direction,
                            CellMaterial material)
{
    int step;

    for (step = 0; step < length; ++step) {
        int row = y + step * direction;
        int half = (length - step) * (length / 6 + 2) / length;
        int column;

        for (column = x - half; column <= x + half; ++column) {
            if (!WorldInBounds(world, column, row)) continue;
            if (WorldMaterialAt(world, column, row) != MATERIAL_EMPTY) continue;
            WorldSetGeneratedCell(world, column, row, material);
        }
    }
}

/* A mushroom: a pale stalk and a glowing dome of cap. */
static void WorldPlaceMushroomBody(World *world, int x, int floorY, int height, int capRadius)
{
    int y;
    int top = floorY - height;
    /* A stalk as thick as the mushroom is tall allows: a giant on a thread
       is a lollipop. */
    int stalk = height > 60 ? 9 : (height > 30 ? 6 : (height > 10 ? 3 : 1));
    int offset;

    for (y = floorY - 1; y > top; --y) {
        if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) return;
    }
    for (y = floorY - 1; y > top; --y) {
        for (offset = 0; offset < stalk; ++offset) {
            if (WorldMaterialAt(world, x + offset, y) == MATERIAL_EMPTY) {
                WorldSetGeneratedCell(world, x + offset, y, MATERIAL_WOOD);
            }
        }
    }
    for (y = top - capRadius / 2; y <= top; ++y) {
        int column;
        float across = (float)(top - y) / (float)(capRadius / 2 + 1);
        int half = (int)((float)capRadius * sqrtf(1.0f - across * across));

        for (column = x - half; column <= x + half + stalk - 1; ++column) {
            if (!WorldInBounds(world, column, y)) continue;
            if (WorldMaterialAt(world, column, y) != MATERIAL_EMPTY) continue;
            WorldSetGeneratedCell(world, column, y, MATERIAL_FUNGUS);
        }
    }
}

static void WorldPlaceMushroom(World *world, int x, int floorY, int height, int capRadius)
{
    world->generationPlant = PlantIdAt(x, floorY);
    WorldPlaceMushroomBody(world, x, floorY, height, capRadius);
    world->generationPlant = 0u;
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
        int radiusX = RngRange(&rng, 220, 420);
        int radiusY = RngRange(&rng, 90, 160);
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
                    CellMaterial here = WorldMaterialAt(world, centerX + dx, y);

                    /* Ground becomes the basin's rock; a hollow that was
                       already there stays one. */
                    if (WorldInBounds(world, centerX + dx, y) && MaterialIsSolid(here) &&
                        !MaterialIsDynamic(here)) {
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
            if (dx % 30 == 0 && shape > 0.3f && RngRange(&rng, 0, 99) < 55) {
                WorldPlaceSpike(world, centerX + dx, ceiling - 1,
                                RngRange(&rng, 14, 18 + (int)(46.0f * shape)), 1,
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
            for (dx = -radiusX + 30; dx <= radiusX - 30; dx += RngRange(&rng, 32, 70)) {
                int x = centerX + dx;
                int y = centerY;

                while (y < centerY + radiusY &&
                       WorldMaterialAt(world, x, y + 1) == MATERIAL_EMPTY) {
                    ++y;
                }
                if (y >= centerY + radiusY) continue;
                WorldPlaceMushroom(world, x, y + 1, RngRange(&rng, 30, 84),
                                   RngRange(&rng, 14, 32));
            }
            break;
        case CAVERN_CRYSTAL:
            for (dx = -radiusX + 20; dx <= radiusX - 20; dx += RngRange(&rng, 20, 50)) {
                int x = centerX + dx;
                int y = centerY;

                while (y < centerY + radiusY &&
                       WorldMaterialAt(world, x, y + 1) == MATERIAL_EMPTY) {
                    ++y;
                }
                if (y >= centerY + radiusY) continue;
                WorldPlaceSpike(world, x, y, RngRange(&rng, 24, 90), -1,
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

        /* No mouth under the sea, on the shelf or on a drowned shore: the
           sea would pour down it into every cave the tunnel joins. */
        if (IsNearSpawn(world, (int)x) ||
            WorldBiomeAt(world, (int)x) == WORLD_BIOME_OCEAN ||
            SurfaceHeightAt(world, (int)x) > (int)WorldSeaLevelY(world) - 16) {
            continue;
        }
        y = (float)SurfaceHeightAt(world, (int)x) - 2.0f;
        if (RngRange(&rng, 0, 1) == 0) {
            angle = PI - angle;
        }
        for (step = 0; step < steps; ++step) {
            /* Never narrower than the character is tall: a tunnel is a way
               down, and one a walker has to crouch through is a wall. */
            float radius = 20.0f + 10.0f * (0.5f + 0.5f * sinf((float)step * 0.03f +
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
            x += cosf(angle) * 6.0f;
            y += sinf(angle) * 6.0f;
            if (y >= (float)world->height - 20.0f) break;
        }
    }
}

/* Every generated pool left the way the water would leave it.
 *
 * Features are laid one after another and each may open the one before it:
 * a tunnel dug past a lake, a cave under a pond's bed, a vault into a lava
 * pocket. Walling each of them off made rims of rock nobody built. Instead
 * the liquid is let go the way it would go on its first tick, and removed
 * rather than moved: a cell with nothing under it or beside it goes, then
 * whatever that leaves standing on nothing goes, until every pool is held
 * by what is around it — a lake cut by a tunnel stands at the height of the
 * cut, one with a hole in its bed is gone. A chunk streamed into play then
 * has nothing in it to flow. Only the chunks the per-chunk counts say hold
 * liquid are looked at. */
static bool LiquidHeld(const World *world, int x, int y)
{
    return (y + 1 >= world->height || WorldMaterialAt(world, x, y + 1) != MATERIAL_EMPTY) &&
           WorldMaterialAt(world, x - 1, y) != MATERIAL_EMPTY &&
           WorldMaterialAt(world, x + 1, y) != MATERIAL_EMPTY;
}

/* Where generated water meets generated lava, the lava has already met it:
   the face between them is cooled rock, the crust a lava lake grows wherever
   the sea finds it. Left touching, the pair would be generated asleep and
   boil the moment the player came near. Only the chunks holding both are
   read. */
static void SettleLavaAgainstWater(World *world)
{
    int chunkY;

    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunk = WorldChunkIndex(world, chunkX, chunkY);
            int y;

            if (world->chunkLava[chunk] == 0u) continue;
            for (y = chunkY * WORLD_CHUNK_SIZE;
                 y < (chunkY + 1) * WORLD_CHUNK_SIZE && y < world->height; ++y) {
                int x;

                for (x = chunkX * WORLD_CHUNK_SIZE;
                     x < (chunkX + 1) * WORLD_CHUNK_SIZE && x < world->width; ++x) {
                    int offsetY;
                    bool wet = false;

                    if (WorldMaterialAt(world, x, y) != MATERIAL_LAVA) continue;
                    for (offsetY = -1; offsetY <= 1 && !wet; ++offsetY) {
                        int offsetX;

                        for (offsetX = -1; offsetX <= 1; ++offsetX) {
                            if (WorldMaterialAt(world, x + offsetX, y + offsetY) ==
                                MATERIAL_WATER) {
                                wet = true;
                                break;
                            }
                        }
                    }
                    if (wet) {
                        WorldSetGeneratedCell(world, x, y, MATERIAL_BASALT);
                    }
                }
            }
        }
    }
}

static void SettleLiquids(World *world)
{
    size_t capacity = 1u << 16;
    int *queue = malloc(capacity * sizeof(*queue));
    size_t count = 0;
    int chunkY;

    if (queue == NULL) return;
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunk = WorldChunkIndex(world, chunkX, chunkY);
            int y;

            if (world->chunkWater[chunk] == 0u && world->chunkLava[chunk] == 0u) {
                continue;
            }
            for (y = chunkY * WORLD_CHUNK_SIZE;
                 y < (chunkY + 1) * WORLD_CHUNK_SIZE && y < world->height; ++y) {
                int x;

                for (x = chunkX * WORLD_CHUNK_SIZE;
                     x < (chunkX + 1) * WORLD_CHUNK_SIZE && x < world->width; ++x) {
                    if (!MaterialIsLiquid(WorldMaterialAt(world, x, y)) ||
                        LiquidHeld(world, x, y)) {
                        continue;
                    }
                    if (count == capacity) {
                        int *grown = realloc(queue, capacity * 2u * sizeof(*queue));

                        if (grown == NULL) {
                            free(queue);
                            return;
                        }
                        queue = grown;
                        capacity *= 2u;
                    }
                    queue[count++] = y * world->width + x;
                }
            }
        }
    }
    /* A stack: the order does not change where it ends. */
    while (count > 0) {
        int cell = queue[--count];
        int x = cell % world->width;
        int y = cell / world->width;
        static const int stepX[3] = {1, -1, 0};
        static const int stepY[3] = {0, 0, -1};
        int direction;

        if (!MaterialIsLiquid(WorldMaterialAt(world, x, y)) || LiquidHeld(world, x, y)) {
            continue;
        }
        WorldSetGeneratedCell(world, x, y, MATERIAL_EMPTY);
        for (direction = 0; direction < 3; ++direction) {
            int nextX = WorldWrapColumn(x + stepX[direction], world->width);
            int nextY = y + stepY[direction];

            if (nextY < 0 || !MaterialIsLiquid(WorldMaterialAt(world, nextX, nextY))) {
                continue;
            }
            if (count == capacity) {
                int *grown = realloc(queue, capacity * 2u * sizeof(*queue));

                if (grown == NULL) {
                    free(queue);
                    return;
                }
                queue = grown;
                capacity *= 2u;
            }
            queue[count++] = nextY * world->width + nextX;
        }
    }
    free(queue);
    SettleLavaAgainstWater(world);
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

void WorldGenGrowGrass(World *world, int x, int groundY, Rng *rng)
{
    FloraGrowGrass(world, x, groundY, rng);
}

void WorldGenPlaceTree(World *world, int x, int groundY, Rng *rng)
{
    FloraPlaceBroadleaf(world, x, groundY, rng, RngRange(rng, 80, 130),
                        RngRange(rng, 11, 16), MATERIAL_LEAF);
}

/* Sand as a blanket on limestone.
 *
 * Sand is a falling material, and the world is generated asleep: a grain
 * laid over a cave, on the lip of a mesa or against the wall of a pit sits
 * where it was put only until its chunk is first streamed into play, and
 * then the whole slope slides at once — a desert that costs the simulation
 * everything the moment the player arrives. So below a blanket of
 * SAND_BLANKET_ROWS the dune is limestone, and anywhere a grain would move
 * on its first tick — nothing solid under it, or open on either side below —
 * it is laid as limestone too. A rule applied from the top of the column
 * down never makes a grain it has already passed unstable: what it changes
 * becomes solid, and solid holds up everything above it. */
#define SAND_BLANKET_ROWS 14

static bool SandHeldAt(const World *world, int x, int y)
{
    CellMaterial material = WorldMaterialAt(world, x, y);

    return MaterialIsSolid(material) && !MaterialIsFlora(material);
}

static void SettleSand(World *world)
{
    int x;

    for (x = 0; x < world->width; ++x) {
        int run = 0;
        int y;

        for (y = WorldSkyRows(world); y < world->height; ++y) {
            if (WorldMaterialAt(world, x, y) != MATERIAL_SAND) {
                run = 0;
                continue;
            }
            ++run;
            if (run > SAND_BLANKET_ROWS || y + 1 >= world->height ||
                !SandHeldAt(world, x, y + 1) || !SandHeldAt(world, x - 1, y + 1) ||
                !SandHeldAt(world, x + 1, y + 1)) {
                WorldSetGeneratedCell(world, x, y, MATERIAL_LIMESTONE);
            }
        }
    }
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
    /* The back layer from the ground as it now lies, before anything is
       built: what the builders leave replaces it behind their rooms. */
    WorldGenerateBackWalls(world);
    /* Ruins and dungeons after the ground has its final shape, and before
       the sea is poured: a sunken temple fills with the sea around it. */
    WorldGenerateUnderground(world);
    WorldGenerateRuins(world);
    /* After every feature that could change the shape of the ground, so the
       coastline is the coastline the world actually ended up with. */
    GenerateSea(world);
    /* Before the plants, so a pine stands in the snow and the treeline is
       where the snow begins. */
    GenerateSnow(world);
    /* After everything that cuts or piles the ground, before the plants:
       no generated grain of sand is left with nothing under it. */
    SettleSand(world);
    /* And then the water, on ground that will no longer move. */
    SettleLiquids(world);
    /* Last, so that every plant grows on the surface as it finally is rather
       than on one a later feature was going to bury. */
    GenerateFlora(world);
    GenerateCaveGrowth(world);
}

void WorldGenSetBackWall(World *world, int firstX, int firstY, int lastX, int lastY,
                         CellMaterial material)
{
    int blockY;

    if (world->backWalls == NULL) return;
    if (firstY < 0) firstY = 0;
    if (lastY >= world->height) lastY = world->height - 1;
    for (blockY = firstY / WORLD_BACK_WALL_SCALE; blockY <= lastY / WORLD_BACK_WALL_SCALE;
         ++blockY) {
        int blockX;

        for (blockX = (int)floorf((float)firstX / (float)WORLD_BACK_WALL_SCALE);
             blockX <= (int)floorf((float)lastX / (float)WORLD_BACK_WALL_SCALE); ++blockX) {
            int column = PositiveModulo(blockX, world->backWallColumns);

            world->backWalls[(size_t)blockY * (size_t)world->backWallColumns +
                             (size_t)column] = (uint8_t)material;
        }
    }
}

/* The natural back layer.
 *
 * Noita's caves are hollows in a rock the player can see behind them, and so
 * is every tunnel the player digs; without it a cave here was a hole cut out
 * of the picture with the sky's backdrop showing through the middle of the
 * ground. So under the surface everything has a wall behind it: soil or
 * sand or ice near the top by biome, rock under that, basalt in the deep.
 *
 * "Under the surface" is the ground's own top smoothed across fifty
 * columns by a median: a valley wider than that keeps its sky, a tunnel
 * mouth or a shaft narrower than that is still inside the hill. Measured
 * once, as generated, and never again — digging reveals the wall that was
 * always there. */
#define BACK_WALL_MEDIAN_REACH 24

static int BackWallCompare(const void *first, const void *second)
{
    int a = *(const int *)first;
    int b = *(const int *)second;

    return (a > b) - (a < b);
}

void WorldGenerateBackWalls(World *world)
{
    int *tops;
    int blockX;

    if (world->backWalls == NULL || world->width < 2 * BACK_WALL_MEDIAN_REACH) return;
    tops = malloc((size_t)world->width * sizeof(*tops));
    if (tops == NULL) return;
    {
        int x;

        for (x = 0; x < world->width; ++x) {
            tops[x] = SurfaceSolidY(world, x);
            if (tops[x] < 0) tops[x] = world->height;
        }
    }
    for (blockX = 0; blockX < world->backWallColumns; ++blockX) {
        int samples[BACK_WALL_MEDIAN_REACH + 1];
        int count = 0;
        int centre = blockX * WORLD_BACK_WALL_SCALE + WORLD_BACK_WALL_SCALE / 2;
        int offset;
        int top;
        WorldBiome biome = WorldBiomeAt(world, centre % world->width);
        int deep = (int)WorldGroundY(world, 0.80f);
        int blockY;

        for (offset = -BACK_WALL_MEDIAN_REACH; offset <= BACK_WALL_MEDIAN_REACH;
             offset += 2) {
            samples[count++] = tops[PositiveModulo(centre + offset, world->width)];
        }
        qsort(samples, (size_t)count, sizeof(samples[0]), BackWallCompare);
        top = samples[count / 2] + 6;
        for (blockY = top / WORLD_BACK_WALL_SCALE + 1; blockY < world->backWallRows;
             ++blockY) {
            int y = blockY * WORLD_BACK_WALL_SCALE;
            int depth = y - top;
            CellMaterial wall;

            if (y >= deep) {
                wall = MATERIAL_BASALT;
            } else if (depth < 70) {
                wall = biome == WORLD_BIOME_DUNES    ? MATERIAL_LIMESTONE
                       : biome == WORLD_BIOME_FROST  ? MATERIAL_ICE
                       : biome == WORLD_BIOME_VOLCANIC ? MATERIAL_ROCK
                       : biome == WORLD_BIOME_OCEAN  ? MATERIAL_SAND
                                                     : MATERIAL_DIRT;
            } else {
                wall = MATERIAL_ROCK;
            }
            world->backWalls[(size_t)blockY * (size_t)world->backWallColumns +
                             (size_t)blockX] = (uint8_t)wall;
        }
    }
    free(tops);
}
