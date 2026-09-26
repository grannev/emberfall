#ifndef WEATHER_H
#define WEATHER_H

/* Wind and weather.
 *
 * The weather is a schedule, not a simulation: each biome runs through
 * episodes of a couple of minutes — clear, cloud, rain, a storm; snow and
 * blizzards on the frost; sandstorms over the dunes; ash over the ember
 * wastes — each one ramping in and out, chosen from the world's seed and the
 * episode's number. It is a function of the seed and of time, so a replay
 * has the same weather and nothing has to be stored.
 *
 * Wind is what the weather does to the world. It blows along the ground in
 * one direction per episode, gusting, stronger the worse the weather, and it
 * only blows where there is sky: inside the ground — anywhere the back layer
 * stands behind the air — and out in space there is none. It lifts loose
 * sand and snow off exposed surfaces as grains that fly downwind and settle
 * again as real cells; the world carries it into smoke, steam and fire; the
 * particles drift with it; light bodies are pushed by it. Rain and snow
 * themselves are only drawn (weather_renderer.c): they never become cells.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "rng.h"
#include "world.h"

struct ParticleSystem;
struct DynamicTerrainSystem;

typedef enum WeatherKind {
    WEATHER_CLEAR = 0,
    WEATHER_CLOUDY,
    WEATHER_RAIN,
    WEATHER_STORM,
    WEATHER_SNOW,
    WEATHER_BLIZZARD,
    WEATHER_SANDSTORM,
    WEATHER_ASHFALL,
    WEATHER_KIND_COUNT
} WeatherKind;

typedef struct WeatherSample {
    /* The strongest weather here and how strong, 0..1: it ramps in and out
       over an episode, and across a biome border it gives way to the next
       biome's weather over several hundred cells instead of switching. */
    WeatherKind kind;
    float intensity;
    /* The weather it is giving way to, if any, and how strong that is. */
    WeatherKind other;
    float otherIntensity;
    /* How much of the sky is cloud, 0..1. */
    float cloudCover;
    /* Wind along the ground at this place, cells per second, signed. */
    float wind;
} WeatherSample;

typedef struct WeatherStats {
    int grainsLifted;
    int bodiesPushed;
} WeatherStats;

typedef struct WeatherSystem {
    uint64_t seed;
    /* Seconds of fixed steps since the world was made. */
    double time;
    /* The stream the lifted grains draw from. */
    Rng rng;
    WeatherStats stats;
    /* A weather held everywhere at full strength, or -1: for looking at one
       kind of weather without waiting for it (--weather). */
    int forced;
} WeatherSystem;

/* Grains lifted per fixed step at the strongest wind. */
#define WEATHER_MAX_GRAINS_PER_TICK 12
/* Wind below which nothing loose is lifted, cells per second. */
#define WEATHER_LIFT_WIND 16.0f

void WeatherInit(WeatherSystem *weather, uint64_t seed, int forced);
/* The kind named `name` (case-insensitive, as WeatherKindName spells it), or
   -1. */
int WeatherKindParse(const char *name);
/* One fixed step of time. */
void WeatherAdvance(WeatherSystem *weather, float deltaTime);
/* The weather over column `x` (its biome's episode), and the wind at the
   ground there — without asking whether there is sky at any given row. */
WeatherSample WeatherAt(const WeatherSystem *weather, const World *world, float x);
/* The wind at a place: the column's wind where the air is open to the sky,
   none inside the ground or above the space line. */
float WeatherWindAt(const WeatherSystem *weather, const World *world, float x, float y);
/* Lifts loose grains off exposed ground near `around` and sends them
   downwind as particles that settle again. Bounded per tick. */
void WeatherErode(WeatherSystem *weather, World *world, struct ParticleSystem *particles,
                  Vector2 around);
/* Pushes the awake bodies in open air toward the wind's speed, the lighter
   the body the harder. */
void WeatherPushBodies(WeatherSystem *weather, const World *world,
                       struct DynamicTerrainSystem *terrain, float deltaTime);
const char *WeatherKindName(WeatherKind kind);

#endif
