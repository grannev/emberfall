/* Wind and weather. See weather.h. */
#include "weather.h"

#include <math.h>
#include <stddef.h>

#include "dynamic_terrain.h"
#include "materials.h"
#include "particles.h"

/* Seconds one episode of weather lasts, and how long it takes to come on
   and to clear at either end. */
#define WEATHER_EPISODE_SECONDS 150.0
#define WEATHER_RAMP_SECONDS 25.0

static uint64_t WeatherHash(uint64_t seed, int64_t a, int64_t b)
{
    uint64_t value = seed ^ ((uint64_t)a * 0x9e3779b97f4a7c15ull) ^
                     ((uint64_t)b * 0xc2b2ae3d27d4eb4full);

    value ^= value >> 31;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 29;
    value *= 0x94d049bb133111ebull;
    return value ^ (value >> 32);
}

static float WeatherUnit(uint64_t seed, int64_t a, int64_t b)
{
    return (float)(WeatherHash(seed, a, b) >> 40) / 16777216.0f;
}

/* Smooth noise over time, -1..1, one lattice point every `period` seconds. */
static float WeatherTimeNoise(uint64_t seed, double time, double period, int64_t salt)
{
    double at = time / period;
    int64_t lattice = (int64_t)floor(at);
    float along = (float)(at - (double)lattice);
    float first = WeatherUnit(seed, lattice, salt) * 2.0f - 1.0f;
    float second = WeatherUnit(seed, lattice + 1, salt) * 2.0f - 1.0f;

    along = along * along * (3.0f - 2.0f * along);
    return first + (second - first) * along;
}

void WeatherInit(WeatherSystem *weather, uint64_t seed, int forced)
{
    if (weather == NULL) {
        return;
    }
    weather->seed = seed;
    weather->forced = forced >= 0 && forced < WEATHER_KIND_COUNT ? forced : -1;
    weather->time = 0.0;
    RngSeed(&weather->rng, RngStreamSeed(seed, 0x5eaull));
    weather->stats = (WeatherStats){0};
}

void WeatherAdvance(WeatherSystem *weather, float deltaTime)
{
    if (weather == NULL || deltaTime <= 0.0f) {
        return;
    }
    weather->time += (double)deltaTime;
}

/* What each biome's sky does, and how often: a table per biome, rolled once
   per episode. */
static WeatherKind WeatherKindFor(WorldBiome biome, float roll)
{
    switch (biome) {
    case WORLD_BIOME_DUNES:
        return roll < 0.50f ? WEATHER_CLEAR
               : roll < 0.72f ? WEATHER_CLOUDY
                              : WEATHER_SANDSTORM;
    case WORLD_BIOME_FROST:
        return roll < 0.25f ? WEATHER_CLEAR
               : roll < 0.45f ? WEATHER_CLOUDY
               : roll < 0.80f ? WEATHER_SNOW
                              : WEATHER_BLIZZARD;
    case WORLD_BIOME_VOLCANIC:
        return roll < 0.40f ? WEATHER_CLOUDY
               : roll < 0.85f ? WEATHER_ASHFALL
                              : WEATHER_STORM;
    case WORLD_BIOME_OCEAN:
        return roll < 0.30f ? WEATHER_CLEAR
               : roll < 0.55f ? WEATHER_CLOUDY
               : roll < 0.82f ? WEATHER_RAIN
                              : WEATHER_STORM;
    case WORLD_BIOME_TEMPERATE:
    case WORLD_BIOME_COUNT:
    default:
        return roll < 0.35f ? WEATHER_CLEAR
               : roll < 0.60f ? WEATHER_CLOUDY
               : roll < 0.88f ? WEATHER_RAIN
                              : WEATHER_STORM;
    }
}

/* Steady wind of a kind of weather at full strength, cells per second. */
static float WeatherKindWind(WeatherKind kind)
{
    static const float wind[WEATHER_KIND_COUNT] = {
        [WEATHER_CLEAR] = 7.0f,     [WEATHER_CLOUDY] = 13.0f,
        [WEATHER_RAIN] = 18.0f,     [WEATHER_STORM] = 46.0f,
        [WEATHER_SNOW] = 11.0f,     [WEATHER_BLIZZARD] = 52.0f,
        [WEATHER_SANDSTORM] = 58.0f, [WEATHER_ASHFALL] = 9.0f,
    };

    return kind >= 0 && kind < WEATHER_KIND_COUNT ? wind[kind] : 0.0f;
}

static float WeatherKindCover(WeatherKind kind)
{
    static const float cover[WEATHER_KIND_COUNT] = {
        [WEATHER_CLEAR] = 0.35f,    [WEATHER_CLOUDY] = 0.75f,
        [WEATHER_RAIN] = 0.9f,      [WEATHER_STORM] = 1.0f,
        [WEATHER_SNOW] = 0.85f,     [WEATHER_BLIZZARD] = 1.0f,
        [WEATHER_SANDSTORM] = 0.5f, [WEATHER_ASHFALL] = 0.8f,
    };

    return kind >= 0 && kind < WEATHER_KIND_COUNT ? cover[kind] : 0.0f;
}

/* One biome's own weather at this moment: its episode's kind, how far the
   episode has ramped, which way its wind blows. */
static void WeatherEpisode(const WeatherSystem *weather, WorldBiome biome, WeatherKind *kind,
                           float *intensity, float *direction)
{
    /* Each biome keeps its own clock, so the whole planet does not change
       its weather at once. */
    double shifted = weather->time + (double)biome * 47.0;
    int64_t episode = (int64_t)floor(shifted / WEATHER_EPISODE_SECONDS);
    double within = shifted - (double)episode * WEATHER_EPISODE_SECONDS;
    float ramp = (float)fmin(within / WEATHER_RAMP_SECONDS,
                             (WEATHER_EPISODE_SECONDS - within) / WEATHER_RAMP_SECONDS);

    if (ramp > 1.0f) ramp = 1.0f;
    if (ramp < 0.0f) ramp = 0.0f;
    *kind = WeatherKindFor(biome, WeatherUnit(weather->seed, episode, 11 + biome));
    *intensity = ramp * ramp * (3.0f - 2.0f * ramp);
    *direction = WeatherUnit(weather->seed, episode, 91 + biome) < 0.5f ? -1.0f : 1.0f;
    if (weather->forced >= 0) {
        *kind = (WeatherKind)weather->forced;
        *intensity = 1.0f;
    }
}

/* Cells either side of a column the weather is gathered from, and the step:
   a biome's weather reaches this far past its border, fading. */
#define WEATHER_BLEND_REACH 384
#define WEATHER_BLEND_STEP 64

WeatherSample WeatherAt(const WeatherSystem *weather, const World *world, float x)
{
    WeatherSample sample = {WEATHER_CLEAR, 0.0f, WEATHER_CLEAR, 0.0f, 0.35f, 0.0f};
    float kindWeight[WEATHER_KIND_COUNT] = {0.0f};
    float total = 0.0f;
    float cover = 0.0f;
    float steadyWind = 0.0f;
    int offset;
    int best = -1;
    int second = -1;
    int kind;
    float gust;

    if (weather == NULL || world == NULL || world->width <= 0) {
        return sample;
    }
    /* The weather of every biome within reach, weighted down with distance:
       at a border the two biomes' weathers meet and cross-fade over the
       whole band instead of switching at one column. */
    for (offset = -WEATHER_BLEND_REACH; offset <= WEATHER_BLEND_REACH;
         offset += WEATHER_BLEND_STEP) {
        float weight = 1.0f - fabsf((float)offset) / (float)(WEATHER_BLEND_REACH + WEATHER_BLEND_STEP);
        WorldBiome biome = WorldBiomeAt(world, (int)floorf(x) + offset);
        WeatherKind here;
        float intensity;
        float direction;

        WeatherEpisode(weather, biome, &here, &intensity, &direction);
        kindWeight[here] += weight * intensity;
        cover += weight * (0.35f + (WeatherKindCover(here) - 0.35f) * intensity);
        steadyWind += weight * direction *
                      (WeatherKindWind(WEATHER_CLEAR) +
                       (WeatherKindWind(here) - WeatherKindWind(WEATHER_CLEAR)) * intensity);
        total += weight;
    }
    for (kind = 0; kind < WEATHER_KIND_COUNT; ++kind) {
        if (best < 0 || kindWeight[kind] > kindWeight[best]) {
            second = best;
            best = kind;
        } else if (second < 0 || kindWeight[kind] > kindWeight[second]) {
            second = kind;
        }
    }
    sample.kind = (WeatherKind)best;
    sample.intensity = kindWeight[best] / total;
    sample.other = (WeatherKind)second;
    sample.otherIntensity = kindWeight[second] / total;
    sample.cloudCover = cover / total;

    /* Gusting over a few seconds and varying along the ground over a few
       hundred cells. */
    gust = WeatherTimeNoise(weather->seed, weather->time + (double)x / 90.0, 5.5, 3) * 0.45f +
           WeatherTimeNoise(weather->seed, weather->time, 1.7, 5) * 0.2f;
    sample.wind = steadyWind / total * (1.0f + gust);
    return sample;
}

int WeatherKindParse(const char *name)
{
    int kind;

    if (name == NULL) return -1;
    for (kind = 0; kind < WEATHER_KIND_COUNT; ++kind) {
        const char *expected = WeatherKindName((WeatherKind)kind);
        int index = 0;

        while (name[index] != '\0' && expected[index] != '\0' &&
               (name[index] == expected[index] || name[index] == expected[index] + 32)) {
            ++index;
        }
        if (name[index] == '\0' && expected[index] == '\0') return kind;
    }
    return -1;
}

float WeatherWindAt(const WeatherSystem *weather, const World *world, float x, float y)
{
    float space;

    if (weather == NULL || world == NULL || world->cells == NULL) {
        return 0.0f;
    }
    /* No air in space, and the pull and the air fade across the same band. */
    space = WorldGravityScaleAt(world, y);
    if (space <= 0.0f) {
        return 0.0f;
    }
    /* Inside the ground: the back layer stands behind this air. */
    if (WorldGetBackWall(world, (int)floorf(x), (int)floorf(y)) != MATERIAL_EMPTY) {
        return 0.0f;
    }
    return WeatherAt(weather, world, x).wind * space;
}

void WeatherErode(WeatherSystem *weather, World *world, struct ParticleSystem *particles,
                  Vector2 around)
{
    WeatherSample sample;
    int attempts;
    int attempt;
    float strength;

    if (weather == NULL || world == NULL || world->cells == NULL || particles == NULL) {
        return;
    }
    sample = WeatherAt(weather, world, around.x);
    strength = fabsf(sample.wind);
    if (strength < WEATHER_LIFT_WIND || WorldGravityScaleAt(world, around.y) <= 0.0f) {
        return;
    }
    attempts = (int)((float)WEATHER_MAX_GRAINS_PER_TICK *
                     fminf(1.0f, (strength - WEATHER_LIFT_WIND) / 40.0f) + 0.5f);
    for (attempt = 0; attempt < attempts; ++attempt) {
        int x = (int)floorf(around.x) + RngRange(&weather->rng, -230, 230);
        int y = (int)floorf(around.y) - 150;
        int last = (int)floorf(around.y) + 150;
        int downwind = sample.wind > 0.0f ? 1 : -1;
        CellMaterial material = MATERIAL_EMPTY;

        if (y < 1) y = 1;
        if (last > world->height - 2) last = world->height - 2;
        /* The first thing down the column: loose ground open to the sky and
           to the wind on its lee side, or nothing. */
        for (; y <= last; ++y) {
            material = WorldGetCell(world, x, y);
            if (material != MATERIAL_EMPTY) break;
        }
        if (y > last || (material != MATERIAL_SAND && material != MATERIAL_SNOW) ||
            WorldGetBackWall(world, x, y - 1) != MATERIAL_EMPTY ||
            WorldGetCell(world, x + downwind, y - 1) != MATERIAL_EMPTY) {
            continue;
        }
        WorldSetCell(world, x, y, MATERIAL_EMPTY);
        ParticlesSpawnWindGrain(particles, (Vector2){(float)x + 0.5f, (float)y - 0.5f},
                                (Vector2){sample.wind * RngFloat(&weather->rng, 0.6f, 1.1f),
                                          -RngFloat(&weather->rng, 10.0f, 45.0f)},
                                material);
        ++weather->stats.grainsLifted;
    }
}

/* Horizontal distance on the wrapped world, signed, from `from` to `to`. */
static float WeatherWrappedDistance(const World *world, float from, float to)
{
    float width = (float)world->width;
    float distance = fmodf(to - from, width);

    if (distance > width * 0.5f) distance -= width;
    if (distance < -width * 0.5f) distance += width;
    return distance;
}

int WeatherReleaseTumbleweeds(WeatherSystem *weather, World *world, Vector2 around)
{
    int released = 0;
    int index;

    if (weather == NULL || world == NULL || world->cells == NULL) {
        return 0;
    }
    for (index = 0; index < world->tumbleweedCount; ++index) {
        WorldTumbleweed *weed = &world->tumbleweeds[index];
        float wind;
        uint16_t plant;
        int radius = weed->radius;
        int x;
        int y;

        if (weed->released ||
            fabsf(WeatherWrappedDistance(world, around.x, (float)weed->x)) >
                WEATHER_TUMBLEWEED_REACH ||
            fabsf((float)weed->y - around.y) > WEATHER_TUMBLEWEED_REACH) {
            continue;
        }
        wind = WeatherWindAt(weather, world, (float)weed->x, (float)weed->y);
        if (fabsf(wind) < WEATHER_TUMBLEWEED_WIND) continue;
        /* Not all at once: each gust takes a few, the stronger the more. */
        if (RngFloat(&weather->rng, 0.0f, 1.0f) >
            fabsf(wind) / WEATHER_TUMBLEWEED_WIND * 0.004f) {
            continue;
        }
        weed->released = true;
        /* Whichever of its cells is still there names it: burned, drilled
           or brushed away, there is nothing left to blow. */
        plant = 0u;
        for (y = weed->y - radius; y <= weed->y + radius && plant == 0u; ++y) {
            for (x = weed->x - radius; x <= weed->x + radius; ++x) {
                if (WorldGetCell(world, x, y) == MATERIAL_DRYBRUSH) {
                    plant = WorldGetPlant(world, x, y);
                    break;
                }
            }
        }
        if (plant == 0u) continue;
        /* Snap the twigs it stands on — every one of its cells with ground
           under it — and let the ordinary detach check find the ball loose
           and make a body of it, which the wind then rolls. */
        for (y = weed->y - radius; y <= weed->y + radius + 1; ++y) {
            for (x = weed->x - radius; x <= weed->x + radius; ++x) {
                CellMaterial below = WorldGetCell(world, x, y + 1);

                if (WorldGetCell(world, x, y) == MATERIAL_DRYBRUSH &&
                    WorldGetPlant(world, x, y) == plant && below != MATERIAL_EMPTY &&
                    !MaterialIsFlora(below)) {
                    WorldSetCell(world, x, y, MATERIAL_EMPTY);
                }
            }
        }
        WorldRecordDestruction(world, weed->x - radius - 1, weed->y - radius - 1,
                               weed->x + radius + 1, weed->y + radius + 2);
        ++released;
        ++weather->stats.tumbleweedsReleased;
    }
    return released;
}

void WeatherPushBodies(WeatherSystem *weather, const World *world,
                       struct DynamicTerrainSystem *terrain, float deltaTime)
{
    int slot;

    if (weather == NULL || world == NULL || terrain == NULL || deltaTime <= 0.0f) {
        return;
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &terrain->bodies[slot];
        float wind;
        float density;
        bool light;

        if (!body->active || body->cellCount <= 0) {
            continue;
        }
        wind = WeatherWindAt(weather, world, body->position.x, body->position.y);
        if (wind == 0.0f) {
            continue;
        }
        density = body->mass / (float)body->cellCount;
        light = density < WEATHER_LIGHT_BODY_DENSITY;
        /* A crown of leaves or a ball of brush at rest is picked up again by
           a strong wind; a rock is left where it lies. */
        if (!body->awake) {
            if (!light || fabsf(wind) < WEATHER_LIFT_WIND) continue;
            if (!DynamicTerrainWakeBody(terrain, (TerrainBodyHandle){(uint16_t)slot,
                                                                     body->generation})) {
                continue;
            }
        }
        /* Drag toward the wind, divided by how heavy each cell is: a crown
           of leaves goes with it, a slab of rock hardly notices. */
        body->velocity.x += (wind - body->velocity.x) * fminf(1.0f, 0.35f / density * deltaTime);
        if (light) {
            Vector2 minimum;
            Vector2 maximum;

            /* Brush rolls rather than slides: it turns at the rate its
               speed would roll it over the ground, and every so often it
               hits a bump and hops. */
            if (TerrainBodyWorldBounds(body, &minimum, &maximum)) {
                float radius = fmaxf(2.0f, (maximum.y - minimum.y) * 0.5f);
                bool onGround = WorldCellBlocksBodies(world, (int)floorf(body->position.x),
                                                      (int)floorf(maximum.y) + 1);

                if (onGround) {
                    body->angularVelocity +=
                        (body->velocity.x / radius - body->angularVelocity) *
                        fminf(1.0f, 4.0f * deltaTime);
                    if (fabsf(body->velocity.x) > 12.0f &&
                        RngFloat(&weather->rng, 0.0f, 1.0f) < 0.02f) {
                        body->velocity.y = -RngFloat(&weather->rng, 18.0f, 16.0f + fabsf(wind) * 0.6f);
                    }
                }
            }
        }
        ++weather->stats.bodiesPushed;
    }
}

const char *WeatherKindName(WeatherKind kind)
{
    static const char *const names[WEATHER_KIND_COUNT] = {
        [WEATHER_CLEAR] = "CLEAR",       [WEATHER_CLOUDY] = "CLOUDY",
        [WEATHER_RAIN] = "RAIN",         [WEATHER_STORM] = "STORM",
        [WEATHER_SNOW] = "SNOW",         [WEATHER_BLIZZARD] = "BLIZZARD",
        [WEATHER_SANDSTORM] = "SANDSTORM", [WEATHER_ASHFALL] = "ASHFALL",
    };

    return kind >= 0 && kind < WEATHER_KIND_COUNT ? names[kind] : "UNKNOWN";
}
