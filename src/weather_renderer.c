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
    drop->color = (Color){0, 0, 0, 0};
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

/* ---- the ambience ----------------------------------------------------------
 *
 * Small life of each place, found by sampling random cells of the view for
 * its source — a leaf with air under it, lava open to the air, a cave
 * ceiling, a wave top, snow in the sun, grass at night — so every effect
 * starts at the thing that makes it and nothing is laid over the view. */

static WeatherDrop *WeatherPut(WeatherRenderer *renderer, const World *world, WeatherDropKind kind,
                               Vector2 position, Vector2 velocity, float life, Color color)
{
    WeatherDrop *drop = WeatherTakeDrop(renderer);

    /* What does not glow is lit like the place it starts in: dim at night,
       dimmer under the ground. The drops are drawn over the lit world, not
       through its light, so this is their share of it. */
    /* A splash takes the colour of its drip, already lit. */
    if (kind != WEATHER_DROP_EMBER && kind != WEATHER_DROP_FIREFLY &&
        kind != WEATHER_DROP_GLINT && kind != WEATHER_DROP_SPLASH) {
        float lit = WorldGetBackWall(world, (int)floorf(position.x), (int)floorf(position.y)) !=
                            MATERIAL_EMPTY
                        ? 0.4f
                        : 0.28f + 0.72f * world->daylight;

        color.r = (unsigned char)((float)color.r * lit);
        color.g = (unsigned char)((float)color.g * lit);
        color.b = (unsigned char)((float)color.b * lit);
    }

    drop->active = true;
    drop->kind = (uint8_t)kind;
    drop->position = position;
    drop->velocity = velocity;
    drop->life = life;
    drop->phase = WeatherRandom(renderer) * 6.28f;
    drop->color = color;
    return drop;
}

static float WeatherBetween(WeatherRenderer *renderer, float low, float high)
{
    return low + (high - low) * WeatherRandom(renderer);
}

/* How many samples to take this frame for a rate given per sixtieth of a
   second: whole ones, and the fraction by chance. */
static int WeatherSamples(WeatherRenderer *renderer, float perTick, float deltaTime)
{
    float wanted = perTick * deltaTime * 60.0f;
    int count = (int)wanted;

    if (WeatherRandom(renderer) < wanted - (float)count) ++count;
    return count;
}

static Color WeatherShadeOf(WeatherRenderer *renderer, CellMaterial material, unsigned char alpha)
{
    Color base = MaterialAt(material)->color;
    float shade = WeatherBetween(renderer, 0.8f, 1.15f);

    return (Color){(unsigned char)fminf(255.0f, (float)base.r * shade),
                   (unsigned char)fminf(255.0f, (float)base.g * shade),
                   (unsigned char)fminf(255.0f, (float)base.b * shade), alpha};
}

static void WeatherAmbience(WeatherRenderer *renderer, const World *world, WeatherHero hero,
                            Rectangle visible, float deltaTime)
{
    float wind = renderer->sample.wind;
    float windFactor = fminf(1.0f, fmaxf(0.0f, (fabsf(wind) - 6.0f) / 40.0f));
    float daylight = world->daylight;
    float scale = visible.width / 426.0f;
    int samples;
    int sample;

    /* Leaves: one now and then in calm air, streams of them in a gale. */
    samples = WeatherSamples(renderer, (30.0f + 400.0f * windFactor) * scale, deltaTime);
    for (sample = 0; sample < samples; ++sample) {
        int x = (int)(visible.x + WeatherRandom(renderer) * visible.width);
        int y = (int)(visible.y + WeatherRandom(renderer) * visible.height);

        if (WorldGetCell(world, x, y) != MATERIAL_LEAF ||
            WorldGetCell(world, x, y + 1) != MATERIAL_EMPTY ||
            WorldGetBackWall(world, x, y) != MATERIAL_EMPTY) {
            continue;
        }
        (void)WeatherPut(renderer, world, WEATHER_DROP_LEAF, (Vector2){(float)x + 0.5f, (float)y + 1.0f},
                         (Vector2){wind * 0.5f, WeatherBetween(renderer, 6.0f, 14.0f)},
                         WeatherBetween(renderer, 6.0f, 10.0f),
                         WeatherShadeOf(renderer, MATERIAL_LEAF, 235));
    }
    /* Embers off open lava, fire and the ember blooms. */
    samples = WeatherSamples(renderer, 160.0f * scale, deltaTime);
    for (sample = 0; sample < samples; ++sample) {
        int x = (int)(visible.x + WeatherRandom(renderer) * visible.width);
        int y = (int)(visible.y + WeatherRandom(renderer) * visible.height);
        CellMaterial here = WorldGetCell(world, x, y);
        float chance = here == MATERIAL_LAVA ? 0.5f
                       : here == MATERIAL_FIRE ? 0.35f
                       : here == MATERIAL_EMBERBLOOM ? 0.6f
                                                      : 0.0f;

        if (chance <= 0.0f || WorldGetCell(world, x, y - 1) != MATERIAL_EMPTY ||
            WeatherRandom(renderer) > chance) {
            continue;
        }
        (void)WeatherPut(renderer, world, WEATHER_DROP_EMBER, (Vector2){(float)x + 0.5f, (float)y - 0.5f},
                         (Vector2){wind * 0.3f + WeatherBetween(renderer, -6.0f, 6.0f),
                                   -WeatherBetween(renderer, 16.0f, 42.0f)},
                         WeatherBetween(renderer, 1.2f, 2.8f),
                         (Color){255, (unsigned char)WeatherBetween(renderer, 110.0f, 190.0f), 60, 255});
    }
    /* Drips from cave ceilings: water, or meltwater under ice. */
    samples = WeatherSamples(renderer, 120.0f * scale, deltaTime);
    for (sample = 0; sample < samples; ++sample) {
        int x = (int)(visible.x + WeatherRandom(renderer) * visible.width);
        int y = (int)(visible.y + WeatherRandom(renderer) * visible.height);
        CellMaterial above;

        if (WorldGetCell(world, x, y) != MATERIAL_EMPTY ||
            WorldGetBackWall(world, x, y) == MATERIAL_EMPTY) {
            continue;
        }
        above = WorldGetCell(world, x, y - 1);
        if (!MaterialIsSolid(above) || MaterialIsDynamic(above) || MaterialIsFlora(above) ||
            (above != MATERIAL_ROCK && above != MATERIAL_LIMESTONE && above != MATERIAL_ICE &&
             above != MATERIAL_DIRT) ||
            WeatherRandom(renderer) > 0.06f) {
            continue;
        }
        {
            WeatherDrop *drip = WeatherPut(
                renderer, world, WEATHER_DROP_DRIP, (Vector2){(float)x + 0.5f, (float)y + 0.2f},
                (Vector2){0.0f, 0.0f}, 5.0f,
                above == MATERIAL_ICE ? (Color){200, 226, 246, 210} : (Color){116, 152, 196, 210});

            /* It gathers before it falls. */
            drip->phase = WeatherBetween(renderer, 0.3f, 1.2f);
        }
    }
    /* Spray torn off the wave tops in a strong wind. */
    if (windFactor > 0.25f) {
        samples = WeatherSamples(renderer, 120.0f * windFactor * scale, deltaTime);
        for (sample = 0; sample < samples; ++sample) {
            int x = (int)(visible.x + WeatherRandom(renderer) * visible.width);
            int y = (int)(visible.y + WeatherRandom(renderer) * visible.height);

            if (WorldGetCell(world, x, y) != MATERIAL_WATER ||
                WorldGetCell(world, x, y - 1) != MATERIAL_EMPTY ||
                WorldGetBackWall(world, x, y - 1) != MATERIAL_EMPTY) {
                continue;
            }
            (void)WeatherPut(renderer, world, WEATHER_DROP_SPRAY, (Vector2){(float)x + 0.5f, (float)y - 0.5f},
                             (Vector2){wind * WeatherBetween(renderer, 0.8f, 1.4f),
                                       -WeatherBetween(renderer, 20.0f, 60.0f)},
                             1.2f, (Color){206, 224, 242, 170});
        }
    }
    /* The dunes' dust motes in the sun, snow glinting on the frost, and
       fireflies over the grass at night: each found at its own ground. */
    samples = WeatherSamples(renderer, 120.0f * scale, deltaTime);
    for (sample = 0; sample < samples; ++sample) {
        int x = (int)(visible.x + WeatherRandom(renderer) * visible.width);
        int y = (int)(visible.y + WeatherRandom(renderer) * visible.height);
        CellMaterial here = WorldGetCell(world, x, y);

        if (WorldGetCell(world, x, y - 1) != MATERIAL_EMPTY ||
            WorldGetBackWall(world, x, y - 1) != MATERIAL_EMPTY) {
            continue;
        }
        if (here == MATERIAL_SAND && daylight > 0.3f && WeatherRandom(renderer) < 0.15f) {
            (void)WeatherPut(renderer, world, WEATHER_DROP_MOTE,
                             (Vector2){(float)x + 0.5f, (float)y - WeatherBetween(renderer, 2.0f, 30.0f)},
                             (Vector2){wind * 0.3f, WeatherBetween(renderer, -2.0f, 2.0f)},
                             WeatherBetween(renderer, 3.0f, 6.0f), (Color){238, 214, 158, 120});
        } else if ((here == MATERIAL_SNOW || here == MATERIAL_ICE) && daylight > 0.2f &&
                   WeatherRandom(renderer) < 0.5f) {
            (void)WeatherPut(renderer, world, WEATHER_DROP_GLINT, (Vector2){(float)x, (float)y},
                             (Vector2){0.0f, 0.0f}, WeatherBetween(renderer, 0.25f, 0.5f),
                             (Color){255, 255, 255, 255});
        } else if (here == MATERIAL_GRASS && daylight < 0.35f && WeatherRandom(renderer) < 0.25f) {
            (void)WeatherPut(renderer, world, WEATHER_DROP_FIREFLY,
                             (Vector2){(float)x + 0.5f, (float)y - WeatherBetween(renderer, 3.0f, 22.0f)},
                             (Vector2){0.0f, 0.0f}, WeatherBetween(renderer, 4.0f, 8.0f),
                             (Color){214, 244, 120, 255});
        }
    }

    /* The character's breath in the cold: on the frost or wherever it is
       snowing, out in the open air. */
    renderer->breathTimer -= deltaTime;
    if (renderer->breathTimer <= 0.0f) {
        WorldBiome biome = WorldBiomeAt(world, (int)floorf(hero.mouth.x));
        bool cold = biome == WORLD_BIOME_FROST ||
                    ((renderer->sample.kind == WEATHER_SNOW ||
                      renderer->sample.kind == WEATHER_BLIZZARD) &&
                     renderer->sample.intensity > 0.3f);
        int puff;

        renderer->breathTimer = WeatherBetween(renderer, 2.2f, 3.4f);
        if (cold && WorldGetCell(world, (int)floorf(hero.mouth.x), (int)floorf(hero.mouth.y)) ==
                        MATERIAL_EMPTY &&
            WorldGravityScaleAt(world, hero.mouth.y) > 0.0f) {
            for (puff = 0; puff < 6; ++puff) {
                (void)WeatherPut(renderer, world, WEATHER_DROP_BREATH, hero.mouth,
                                 (Vector2){hero.velocity.x * 0.4f + wind * 0.2f +
                                               WeatherBetween(renderer, -5.0f, 5.0f),
                                           WeatherBetween(renderer, -7.0f, -1.0f)},
                                 WeatherBetween(renderer, 0.8f, 1.3f),
                                 (Color){226, 234, 244, 120});
            }
        }
    }
    /* Dust, sand and snow kicked up by his feet. */
    if (hero.grounded && fabsf(hero.velocity.x) > 20.0f) {
        renderer->stepTimer -= deltaTime * fabsf(hero.velocity.x) / 60.0f;
        if (renderer->stepTimer <= 0.0f) {
            CellMaterial under = WorldGetCell(world, (int)floorf(hero.feet.x),
                                              (int)floorf(hero.feet.y) + 1);
            int grain;

            renderer->stepTimer = 0.22f;
            if (under == MATERIAL_SAND || under == MATERIAL_SNOW || under == MATERIAL_ASH ||
                under == MATERIAL_DIRT || under == MATERIAL_GRASS) {
                for (grain = 0; grain < 3; ++grain) {
                    (void)WeatherPut(renderer, world, WEATHER_DROP_DUST,
                                     (Vector2){hero.feet.x, hero.feet.y - 0.5f},
                                     (Vector2){(hero.velocity.x > 0.0f ? -1.0f : 1.0f) *
                                                   WeatherBetween(renderer, 8.0f, 30.0f),
                                               -WeatherBetween(renderer, 8.0f, 28.0f)},
                                     WeatherBetween(renderer, 0.35f, 0.6f),
                                     WeatherShadeOf(renderer,
                                                    under == MATERIAL_GRASS ? MATERIAL_DIRT : under,
                                                    200));
                }
            }
        }
    } else {
        renderer->stepTimer = 0.0f;
    }
}

void WeatherRendererUpdate(WeatherRenderer *renderer, const WeatherSystem *weather,
                           const World *world, WeatherHero hero, Rectangle visible,
                           float deltaTime)
{
    int inView[WEATHER_DROP_KIND_COUNT] = {0};
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
        if (drop->position.y < visible.y - 240.0f) {
            drop->active = false;
            continue;
        }
        switch (drop->kind) {
        case WEATHER_DROP_SPLASH:
        case WEATHER_DROP_SPRAY:
            drop->velocity.y += 260.0f * deltaTime;
            break;
        case WEATHER_DROP_LEAF:
            /* A leaf tumbles: it swings from side to side as it sinks. */
            drop->phase += deltaTime * 3.4f;
            drop->velocity.x += (renderer->sample.wind * 0.75f + sinf(drop->phase) * 14.0f -
                                 drop->velocity.x) *
                                fminf(1.0f, 2.0f * deltaTime);
            drop->velocity.y += (10.0f + cosf(drop->phase * 1.3f) * 9.0f - drop->velocity.y) *
                                fminf(1.0f, 2.0f * deltaTime);
            break;
        case WEATHER_DROP_EMBER:
            drop->phase += deltaTime * 4.0f;
            drop->velocity.y += 9.0f * deltaTime;
            drop->velocity.x += (renderer->sample.wind * 0.6f + sinf(drop->phase) * 6.0f -
                                 drop->velocity.x) *
                                fminf(1.0f, 1.2f * deltaTime);
            break;
        case WEATHER_DROP_DRIP:
            if (drop->phase > 0.0f) {
                drop->phase -= deltaTime;
                drop->velocity = (Vector2){0.0f, 0.0f};
            } else {
                drop->velocity.y += 300.0f * deltaTime;
            }
            break;
        case WEATHER_DROP_MOTE:
        case WEATHER_DROP_FIREFLY:
            drop->phase += deltaTime * (drop->kind == WEATHER_DROP_MOTE ? 0.9f : 1.6f);
            drop->velocity.x = (drop->kind == WEATHER_DROP_MOTE ? renderer->sample.wind * 0.3f : 0.0f) +
                               sinf(drop->phase * 0.7f) * (drop->kind == WEATHER_DROP_MOTE ? 3.0f : 9.0f);
            drop->velocity.y = cosf(drop->phase * 0.9f) * (drop->kind == WEATHER_DROP_MOTE ? 2.0f : 6.0f);
            break;
        case WEATHER_DROP_BREATH:
            drop->velocity.x *= fmaxf(0.0f, 1.0f - 1.5f * deltaTime);
            drop->velocity.y *= fmaxf(0.0f, 1.0f - 1.5f * deltaTime);
            break;
        case WEATHER_DROP_DUST:
            drop->velocity.y += 90.0f * deltaTime;
            drop->velocity.x *= fmaxf(0.0f, 1.0f - 3.0f * deltaTime);
            break;
        default:
            break;
        }
        next = (Vector2){drop->position.x + drop->velocity.x * deltaTime,
                         drop->position.y + drop->velocity.y * deltaTime};
        hit = WorldGetCell(world, (int)floorf(next.x), (int)floorf(next.y));
        /* Leaves and embers pass through the crowns they come out of; what
           floats in the air or sits on a surface is not stopped at all. */
        if (drop->kind != WEATHER_DROP_RAIN && drop->kind != WEATHER_DROP_SNOW &&
            drop->kind != WEATHER_DROP_ASH && drop->kind != WEATHER_DROP_SAND &&
            MaterialIsFlora(hit)) {
            hit = MATERIAL_EMPTY;
        }
        /* A glint sits on its snow, and a drip hangs from its ceiling until
           it lets go. Everything else that drifts into the ground is gone. */
        if (drop->kind == WEATHER_DROP_GLINT ||
            (drop->kind == WEATHER_DROP_DRIP && drop->phase > 0.0f)) {
            hit = MATERIAL_EMPTY;
        }
        if (hit != MATERIAL_EMPTY && drop->kind == WEATHER_DROP_DRIP) {
            int splash;

            /* A drip lands with a tiny splash of its own colour. */
            for (splash = 0; splash < 2; ++splash) {
                WeatherDrop *droplet = WeatherPut(
                    renderer, world, WEATHER_DROP_SPLASH,
                    (Vector2){drop->position.x, drop->position.y - 0.5f},
                    (Vector2){WeatherBetween(renderer, -25.0f, 25.0f), -WeatherBetween(renderer, 18.0f, 40.0f)},
                    0.18f, drop->color);

                droplet->phase = 0.0f;
            }
            drop->active = false;
            continue;
        }
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
                    droplet->color = (Color){150, 176, 206, 110};
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

    WeatherAmbience(renderer, world, hero, visible, deltaTime);

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
        case WEATHER_DROP_SPRAY:
        case WEATHER_DROP_DUST:
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, drop->color);
            break;
        case WEATHER_DROP_LEAF:
            /* Edge on, then flat: a turning leaf. */
            DrawRectangleV(drop->position,
                           sinf(drop->phase) > 0.0f ? (Vector2){2.0f, 1.0f} : (Vector2){1.0f, 1.0f},
                           drop->color);
            break;
        case WEATHER_DROP_EMBER: {
            Color color = drop->color;

            color.a = (unsigned char)(255.0f * fminf(1.0f, drop->life * 1.5f));
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, color);
            break;
        }
        case WEATHER_DROP_DRIP:
            DrawRectangleV(drop->position,
                           (Vector2){1.0f, drop->phase > 0.0f ? 1.0f : 2.0f}, drop->color);
            break;
        case WEATHER_DROP_MOTE:
        case WEATHER_DROP_BREATH: {
            /* In and out gently rather than popping. */
            Color color = drop->color;
            float fade = fminf(1.0f, drop->life * 1.2f);

            if (drop->kind == WEATHER_DROP_MOTE) {
                fade *= fminf(1.0f, (6.0f - drop->life) * 1.0f + 0.2f);
            }
            color.a = (unsigned char)((float)color.a * fmaxf(0.0f, fade));
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, color);
            break;
        }
        case WEATHER_DROP_GLINT: {
            Color color = drop->color;

            color.a = (unsigned char)(230.0f * fminf(1.0f, drop->life * 5.0f));
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, color);
            break;
        }
        case WEATHER_DROP_FIREFLY: {
            Color color = drop->color;
            float blink = 0.5f + 0.5f * sinf(drop->phase * 5.0f);

            color.a = (unsigned char)(255.0f * blink * fminf(1.0f, drop->life));
            DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, color);
            break;
        }
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

void WeatherRendererDrawEmissive(const WeatherRenderer *renderer, Rectangle visible)
{
    int index;

    if (renderer == NULL) {
        return;
    }
    for (index = 0; index < WEATHER_DROP_CAPACITY; ++index) {
        const WeatherDrop *drop = &renderer->drops[index];
        Color color;

        if (!drop->active ||
            (drop->kind != WEATHER_DROP_EMBER && drop->kind != WEATHER_DROP_FIREFLY) ||
            drop->position.x < visible.x - 8.0f ||
            drop->position.x > visible.x + visible.width + 8.0f ||
            drop->position.y < visible.y - 8.0f ||
            drop->position.y > visible.y + visible.height + 8.0f) {
            continue;
        }
        color = drop->color;
        if (drop->kind == WEATHER_DROP_EMBER) {
            color.a = (unsigned char)(255.0f * fminf(1.0f, drop->life * 1.5f));
        } else {
            color.a = (unsigned char)(255.0f * (0.5f + 0.5f * sinf(drop->phase * 5.0f)) *
                                      fminf(1.0f, drop->life));
        }
        DrawRectangleV(drop->position, (Vector2){1.0f, 1.0f}, color);
    }
}

void WeatherRendererDrawOverlay(const WeatherRenderer *renderer, int width, int height)
{
    if (renderer == NULL || width <= 0 || height <= 0) {
        return;
    }
    /* No wash over the whole screen for any weather: rain, snow, sand and
       ash are the drops themselves, and a storm is its sky. Only the
       lightning, which is a moment and not a tint. */
    if (renderer->flash > 0.0f) {
        DrawRectangle(0, 0, width, height,
                      (Color){230, 236, 255,
                              (unsigned char)(150.0f * renderer->flash * renderer->outdoor)});
    }
}
