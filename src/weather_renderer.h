#ifndef WEATHER_RENDERER_H
#define WEATHER_RENDERER_H

/* What the weather looks like: rain that breaks into splashes on whatever it
   hits, snow, drifting ash, the sand of a storm streaming along the ground,
   and lightning in a storm — and the small life of each place, always local
   to its source and never a wash over the view: leaves the wind strips from
   the crowns, embers off lava and ember blooms, drips from cave ceilings,
   spray blown off the sea, dust motes over the dunes, glints on snow,
   fireflies over the grass at night, the character's breath in the cold and
   the dust his feet kick up.
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
    WEATHER_DROP_LEAF,
    WEATHER_DROP_EMBER,
    WEATHER_DROP_DRIP,
    WEATHER_DROP_SPRAY,
    WEATHER_DROP_MOTE,
    WEATHER_DROP_GLINT,
    WEATHER_DROP_FIREFLY,
    WEATHER_DROP_BREATH,
    WEATHER_DROP_DUST,
    WEATHER_DROP_KIND_COUNT
} WeatherDropKind;

/* What the ambience needs to know about the character: where his feet and
   mouth are and whether he is walking on the ground. */
typedef struct WeatherHero {
    Vector2 feet;
    Vector2 mouth;
    Vector2 velocity;
    bool grounded;
} WeatherHero;

typedef struct WeatherDrop {
    Vector2 position;
    Vector2 velocity;
    float life;
    float phase;
    /* A particle's own colour, for the ambience that takes it from the
       material it came off. */
    Color color;
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
    /* The character's ambience: seconds to the next breath, to the next
       footstep's dust. */
    float breathTimer;
    float stepTimer;
} WeatherRenderer;

void WeatherRendererInit(WeatherRenderer *renderer, uint64_t seed);
void WeatherRendererClear(WeatherRenderer *renderer);
void WeatherRendererShift(WeatherRenderer *renderer, float dx);
/* Advances every drop and spawns new ones for the weather and the ambience
   over `visible`. */
void WeatherRendererUpdate(WeatherRenderer *renderer, const WeatherSystem *weather,
                           const World *world, WeatherHero hero, Rectangle visible,
                           float deltaTime);
/* The drops, in world space inside the camera. */
void WeatherRendererDraw(const WeatherRenderer *renderer, Rectangle visible);
/* What of them glows — embers and fireflies — into the emissive pass. */
void WeatherRendererDrawEmissive(const WeatherRenderer *renderer, Rectangle visible);
/* The lightning over the whole target, screen space. */
void WeatherRendererDrawOverlay(const WeatherRenderer *renderer, int width, int height);

#endif
