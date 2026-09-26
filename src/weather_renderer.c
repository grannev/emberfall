/* Rain, snow, ash, sand and lightning. See weather_renderer.h. */
#include "weather_renderer.h"

#include <math.h>
#include <string.h>

#include "materials.h"

static float WeatherRandom(WeatherRenderer *renderer)
{
    renderer->rng = renderer->rng * 1664525u + 1013904223u;
    return (float)(renderer->rng >> 8) / 16777216.0f;
}

void WeatherRendererInit(WeatherRenderer *renderer, uint64_t seed)
{
    if (renderer == NULL) {
        return;
    }
    memset(renderer, 0, sizeof(*renderer));
    renderer->rng = (uint32_t)(seed ^ (seed >> 32)) | 1u;
    renderer->nextFlash = 6.0f;
}

void WeatherRendererClear(WeatherRenderer *renderer)
{
    int index;

    if (renderer == NULL) {
        return;
    }
    for (index = 0; index < WEATHER_DROP_CAPACITY; ++index) {
        renderer->drops[index].active = false;
    }
    renderer->flash = 0.0f;
}

void WeatherRendererShift(WeatherRenderer *renderer, float dx)
{
    int index;

    if (renderer == NULL) {
        return;
    }
    for (index = 0; index < WEATHER_DROP_CAPACITY; ++index) {
        renderer->drops[index].position.x += dx;
    }
}

static WeatherDrop *WeatherTakeDrop(WeatherRenderer *renderer)
{
    WeatherDrop *drop = &renderer->drops[renderer->next];

    renderer->next = (renderer->next + 1) % WEATHER_DROP_CAPACITY;
    return drop;
}

/* What falls in a kind of weather, and how many a second at full strength
   over a view's width. */
static bool WeatherFalls(WeatherKind kind, WeatherDropKind *drop, float *rate)
{
    switch (kind) {
    case WEATHER_RAIN: *drop = WEATHER_DROP_RAIN; *rate = 700.0f; return true;
    case WEATHER_STORM: *drop = WEATHER_DROP_RAIN; *rate = 1300.0f; return true;
    case WEATHER_SNOW: *drop = WEATHER_DROP_SNOW; *rate = 260.0f; return true;
    case WEATHER_BLIZZARD: *drop = WEATHER_DROP_SNOW; *rate = 760.0f; return true;
    case WEATHER_SANDSTORM: *drop = WEATHER_DROP_SAND; *rate = 900.0f; return true;
    case WEATHER_ASHFALL: *drop = WEATHER_DROP_ASH; *rate = 160.0f; return true;
    default: return false;
    }
}

static bool WeatherOpenSky(const World *world, float x, float y)
{
    int cellX = (int)floorf(x);
    int cellY = (int)floorf(y);

    return world != NULL && cellY >= 0 && cellY < world->height &&
           WorldGetCell(world, cellX, cellY) == MATERIAL_EMPTY &&
           WorldGetBackWall(world, cellX, cellY) == MATERIAL_EMPTY &&
           WorldGravityScaleAt(world, y) > 0.0f;
}

static void WeatherSpawn(WeatherRenderer *renderer, const World *world, Rectangle visible,
                         WeatherDropKind kind, bool anywhere)
{
    WeatherDrop *drop;
    float wind = renderer->sample.wind;
    float x;
    float y;

    if (anywhere) {
        /* Filling the view where it has fewer drops than the weather holds:
           a view that moves fast has no time to wait for them to fall in. */
        x = visible.x + WeatherRandom(renderer) * visible.width;
        y = visible.y + WeatherRandom(renderer) * visible.height;
    } else if (kind == WEATHER_DROP_SAND && WeatherRandom(renderer) < 0.6f) {
        /* A sandstorm streams in from the side it blows from. */
        x = wind > 0.0f ? visible.x - 20.0f : visible.x + visible.width + 20.0f;
        y = visible.y + WeatherRandom(renderer) * visible.height;
    } else {
        /* From above the view, and upwind of it, so the slant fills it. */
        x = visible.x - wind * 0.6f + WeatherRandom(renderer) * visible.width;
        y = visible.y - 10.0f - WeatherRandom(renderer) * 30.0f;
    }
    if (!WeatherOpenSky(world, x, y) ||
        (anywhere && !WeatherOpenSky(world, x, visible.y))) {
        return;
    }
    drop = WeatherTakeDrop(renderer);
    drop->active = true;
    drop->kind = (uint8_t)kind;
    drop->position = (Vector2){x, y};
    drop->phase = WeatherRandom(renderer) * 6.28f;
    switch (kind) {
    case WEATHER_DROP_RAIN:
        drop->velocity = (Vector2){wind * 1.2f, 300.0f + WeatherRandom(renderer) * 80.0f};
        drop->life = 3.0f;
        break;
    case WEATHER_DROP_SNOW:
        drop->velocity = (Vector2){wind * 0.9f, 26.0f + WeatherRandom(renderer) * 22.0f};
        drop->life = 14.0f;
        break;
    case WEATHER_DROP_ASH:
        drop->velocity = (Vector2){wind * 0.8f, 14.0f + WeatherRandom(renderer) * 12.0f};
        drop->life = 16.0f;
        break;
    case WEATHER_DROP_SAND:
    default:
        drop->velocity = (Vector2){wind * (1.5f + WeatherRandom(renderer)),
                                   (WeatherRandom(renderer) - 0.4f) * 30.0f};
        drop->life = 4.0f;
        break;
    }
}

/* Seconds a drop of a kind spends crossing a view of this size, and so how
   many of them the view holds at a given rate. */
static float WeatherResidence(WeatherDropKind kind, Rectangle visible, float wind)
{
    switch (kind) {
    case WEATHER_DROP_RAIN: return visible.height / 340.0f;
    case WEATHER_DROP_SNOW: return fminf(14.0f, visible.height / 37.0f);
    case WEATHER_DROP_ASH: return fminf(16.0f, visible.height / 20.0f);
    case WEATHER_DROP_SAND: return fminf(4.0f, visible.width / fmaxf(30.0f, fabsf(wind) * 2.0f));
    default: return 0.0f;
    }
}

/* One kind of weather's share of the view: drops falling in from above at
   the weather's rate, and the view topped up anywhere it is short. */
static void WeatherFeed(WeatherRenderer *renderer, const World *world, Rectangle visible,
                        WeatherKind weather, float intensity, int inView, float *debt,
                        float deltaTime)
{
    WeatherDropKind kind;
    float rate;
    float scale = visible.width / 426.0f;
    int expected;
    int missing;

    if (!WeatherFalls(weather, &kind, &rate) || intensity <= 0.01f) {
        *debt = 0.0f;
        return;
    }
    *debt += rate * intensity * deltaTime * scale;
    while (*debt >= 1.0f) {
        WeatherSpawn(renderer, world, visible, kind, false);
        *debt -= 1.0f;
    }
    expected = (int)(rate * intensity * scale *
                     WeatherResidence(kind, visible, renderer->sample.wind));
    if (expected > WEATHER_DROP_CAPACITY * 3 / 4) expected = WEATHER_DROP_CAPACITY * 3 / 4;
    missing = expected - inView;
    if (missing > expected / 4) {
        int spawn;

        if (missing > 400) missing = 400;
        for (spawn = 0; spawn < missing; ++spawn) {
            WeatherSpawn(renderer, world, visible, kind, true);
        }
    }
}

void WeatherRendererUpdate(WeatherRenderer *renderer, const WeatherSystem *weather,
                           const World *world, Rectangle visible, float deltaTime)
{
    int inView[5] = {0};
    float centreX = visible.x + visible.width * 0.5f;
    float centreY = visible.y + visible.height * 0.5f;
    int index;

    if (renderer == NULL || weather == NULL || world == NULL || deltaTime <= 0.0f) {
        return;
    }
    renderer->sample = WeatherAt(weather, world, centreX);
    renderer->sample.wind *= WorldGravityScaleAt(world, centreY);
    renderer->cloudTravel += renderer->sample.wind * 0.25f * deltaTime;
    renderer->thunderThisFrame = false;
    /* Out under the sky, eased so the weather does not snap off at a cave
       mouth. */
    {
        float target = WeatherOpenSky(world, centreX, visible.y) ||
                               WorldGetBackWall(world, (int)centreX, (int)centreY) ==
                                   MATERIAL_EMPTY
                           ? 1.0f
                           : 0.0f;

        if (WorldGravityScaleAt(world, centreY) <= 0.0f) target = 0.0f;
        renderer->outdoor += (target - renderer->outdoor) * fminf(1.0f, 2.5f * deltaTime);
    }

    /* Lightning in a storm, at random intervals of several seconds. */
    renderer->flash = fmaxf(0.0f, renderer->flash - deltaTime * 5.0f);
    if (renderer->sample.kind == WEATHER_STORM && renderer->sample.intensity > 0.4f) {
        renderer->nextFlash -= deltaTime;
        if (renderer->nextFlash <= 0.0f) {
            renderer->flash = 0.6f + 0.4f * WeatherRandom(renderer);
            renderer->nextFlash = 3.0f + WeatherRandom(renderer) * 9.0f;
            /* The thunder a later sound pass will play. */
            renderer->thunderThisFrame = true;
        }
    }

    renderer->activeDrops = 0;
    for (index = 0; index < WEATHER_DROP_CAPACITY; ++index) {
        WeatherDrop *drop = &renderer->drops[index];
        Vector2 next;
        CellMaterial hit;

        if (!drop->active) continue;
        drop->life -= deltaTime;
        if (drop->life <= 0.0f ||
            drop->position.y > visible.y + visible.height + 60.0f ||
            drop->position.x < visible.x - 300.0f ||
            drop->position.x > visible.x + visible.width + 300.0f) {
            drop->active = false;
            continue;
        }
        if (drop->kind == WEATHER_DROP_SNOW || drop->kind == WEATHER_DROP_ASH) {
            /* Flakes wander as they fall. */
            drop->phase += deltaTime * 2.2f;
            drop->velocity.x += (renderer->sample.wind * 0.9f + sinf(drop->phase) * 8.0f -
                                 drop->velocity.x) *
                                fminf(1.0f, 1.5f * deltaTime);
        }
        if (drop->kind == WEATHER_DROP_SPLASH) {
            drop->velocity.y += 260.0f * deltaTime;
        }
        next = (Vector2){drop->position.x + drop->velocity.x * deltaTime,
                         drop->position.y + drop->velocity.y * deltaTime};
        hit = WorldGetCell(world, (int)floorf(next.x), (int)floorf(next.y));
        if (hit != MATERIAL_EMPTY && drop->kind != WEATHER_DROP_SPLASH) {
            /* Rain breaks into a few droplets on whatever it meets; a flake
               or a grain simply stops being drawn. */
            /* One droplet from one drop in three: every drop splashing
               laid a bright seam of spray along every surface in the view. */
            if (drop->kind == WEATHER_DROP_RAIN && WeatherRandom(renderer) < 0.33f) {
                int splash;

                for (splash = 0; splash < 1; ++splash) {
                    WeatherDrop *droplet = WeatherTakeDrop(renderer);

                    droplet->active = true;
                    droplet->kind = WEATHER_DROP_SPLASH;
                    droplet->position = (Vector2){drop->position.x, drop->position.y - 1.0f};
                    droplet->velocity = (Vector2){(WeatherRandom(renderer) - 0.5f) * 60.0f,
                                                  -30.0f - WeatherRandom(renderer) * 40.0f};
                    droplet->life = 0.14f;
                    droplet->phase = 0.0f;
                }
            }
            drop->active = false;
            continue;
        }
        drop->position = next;
        ++renderer->activeDrops;
        if (next.x >= visible.x && next.x <= visible.x + visible.width &&
            next.y >= visible.y && next.y <= visible.y + visible.height) {
            ++inView[drop->kind];
        }
    }

    /* The weather here, and across a border the weather it is giving way
       to, each at its own strength. */
    {
        WeatherDropKind mainKind;
        WeatherDropKind otherKind;
        float unused;
        int mainIn = WeatherFalls(renderer->sample.kind, &mainKind, &unused) ? inView[mainKind] : 0;
        int otherIn = WeatherFalls(renderer->sample.other, &otherKind, &unused) ? inView[otherKind] : 0;

        WeatherFeed(renderer, world, visible, renderer->sample.kind, renderer->sample.intensity,
                    mainIn, &renderer->spawnDebt, deltaTime);
        WeatherFeed(renderer, world, visible, renderer->sample.other,
                    renderer->sample.otherIntensity, otherIn, &renderer->otherDebt, deltaTime);
    }
}

void WeatherRendererDraw(const WeatherRenderer *renderer, Rectangle visible)
{
    int index;

    if (renderer == NULL) {
        return;
    }
    for (index = 0; index < WEATHER_DROP_CAPACITY; ++index) {
        const WeatherDrop *drop = &renderer->drops[index];

        if (!drop->active || drop->position.x < visible.x - 8.0f ||
            drop->position.x > visible.x + visible.width + 8.0f ||
            drop->position.y < visible.y - 8.0f ||
            drop->position.y > visible.y + visible.height + 8.0f) {
            continue;
        }
        switch (drop->kind) {
        case WEATHER_DROP_RAIN:
            DrawLineEx(drop->position,
                       (Vector2){drop->position.x - drop->velocity.x * 0.018f,
                                 drop->position.y - drop->velocity.y * 0.018f},
                       0.8f, (Color){170, 196, 226, 150});
            break;
        case WEATHER_DROP_SPLASH:
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, (Color){150, 176, 206, 110});
            break;
        case WEATHER_DROP_SNOW:
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, (Color){238, 244, 255, 220});
            break;
        case WEATHER_DROP_ASH:
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, (Color){120, 112, 106, 200});
            break;
        case WEATHER_DROP_SAND:
        default:
            DrawLineEx(drop->position,
                       (Vector2){drop->position.x - drop->velocity.x * 0.03f,
                                 drop->position.y - drop->velocity.y * 0.03f},
                       0.7f, (Color){214, 186, 128, 170});
            break;
        }
    }
}

void WeatherRendererDrawOverlay(const WeatherRenderer *renderer, int width, int height)
{
    float amount;
    Color haze = {0, 0, 0, 0};

    if (renderer == NULL || width <= 0 || height <= 0) {
        return;
    }
    amount = renderer->sample.intensity * renderer->outdoor;
    switch (renderer->sample.kind) {
    /* No wash over the whole screen for snow or sand: their weather is the
       flakes and the grains themselves. */
    case WEATHER_STORM: haze = (Color){20, 26, 44, (unsigned char)(70.0f * amount)}; break;
    case WEATHER_RAIN: haze = (Color){60, 74, 96, (unsigned char)(28.0f * amount)}; break;
    case WEATHER_ASHFALL: haze = (Color){70, 60, 56, (unsigned char)(36.0f * amount)}; break;
    default: break;
    }
    if (haze.a > 0) {
        DrawRectangleGradientV(0, 0, width, height, (Color){haze.r, haze.g, haze.b, (unsigned char)(haze.a / 2)},
                               haze);
    }
    if (renderer->flash > 0.0f) {
        DrawRectangle(0, 0, width, height,
                      (Color){230, 236, 255,
                              (unsigned char)(150.0f * renderer->flash * renderer->outdoor)});
    }
}
