#ifndef WEATHER_RENDERER_H
#define WEATHER_RENDERER_H

/* What the weather looks like: rain that breaks into splashes on whatever it
   hits, snow, drifting ash, the sand of a storm streaming along the ground,
   the haze each of them hangs over the view, and lightning in a storm.
   Presentation only: nothing here becomes a cell, which is exactly what was
   asked of rain and snow. It reads the world through a const pointer to know
   where a drop lands and whether the view is out under the sky at all — in a
   cave there is no weather to see.

   A fixed pool of drops in world space round the camera, spawned upwind of
   the view at a rate the weather sets. */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "weather.h"
#include "world.h"

#define WEATHER_DROP_CAPACITY 3200

typedef enum WeatherDropKind {
    WEATHER_DROP_RAIN = 0,
    WEATHER_DROP_SPLASH,
    WEATHER_DROP_SNOW,
    WEATHER_DROP_ASH,
    WEATHER_DROP_SAND,
} WeatherDropKind;

typedef struct WeatherDrop {
    Vector2 position;
    Vector2 velocity;
    float life;
    float phase;
    uint8_t kind;
    bool active;
} WeatherDrop;

typedef struct WeatherRenderer {
    WeatherDrop drops[WEATHER_DROP_CAPACITY];
    int next;
    uint32_t rng;
    /* Drops owed to the next frame: the rate is per second and a frame
       rarely asks for a whole number. */
    float spawnDebt;
    float otherDebt;
    WeatherSample sample;
    /* How far the view is out under the sky, 0..1, eased. */
    float outdoor;
    /* Lightning: the flash's brightness, and when the next one comes. */
    float flash;
    float nextFlash;
    /* The distance the wind has carried the clouds. */
    float cloudTravel;
    int activeDrops;
    /* Placeholder for the sound rework: what the weather would be playing. */
    bool thunderThisFrame;
} WeatherRenderer;

void WeatherRendererInit(WeatherRenderer *renderer, uint64_t seed);
void WeatherRendererClear(WeatherRenderer *renderer);
void WeatherRendererShift(WeatherRenderer *renderer, float dx);
/* Advances every drop and spawns new ones for the weather over `visible`. */
void WeatherRendererUpdate(WeatherRenderer *renderer, const WeatherSystem *weather,
                           const World *world, Rectangle visible, float deltaTime);
/* The drops, in world space inside the camera. */
void WeatherRendererDraw(const WeatherRenderer *renderer, Rectangle visible);
/* The haze and the lightning over the whole target, screen space. */
void WeatherRendererDrawOverlay(const WeatherRenderer *renderer, int width, int height);

#endif
