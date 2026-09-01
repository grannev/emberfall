/* Headless invariant tests for the simulation core.
 *
 * These never open a window and never touch the GPU: World owns only CPU state,
 * while WorldRenderer is not linked into this binary. That keeps the suite fast
 * enough to run on every build and lets it cover many more cases than the
 * windowed smoke test.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <raylib.h>
#include <raymath.h>

#include "camera_feedback.h"
#include "game.h"
#include "materials.h"
#include "particles.h"
#include "player.h"
#include "presentation_fx.h"
#include "terrain_body_render_data.h"
#include "abilities.h"
#include "world.h"
#include "dynamic_terrain.h"
#include "environment_renderer.h"
#include "terrain_extraction.h"
#include "terrain_detach.h"
#include "terrain_damage.h"
#include "terrain_impulse.h"
#include "terrain_interaction.h"
#include "terrain_physics.h"
#include "world_components.h"
#include "world_render_data.h"

static int testsRun = 0;
static int testsFailed = 0;
static const char *currentTest = "";

#define CHECK(condition, ...)                                                  \
    do {                                                                       \
        if (!(condition)) {                                                    \
            ++testsFailed;                                                     \
            fprintf(stderr, "FAIL %s: %s\n      ", currentTest, #condition);   \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            return;                                                            \
        }                                                                      \
    } while (0)

#define RUN(test)                                                              \
    do {                                                                       \
        int failedBefore = testsFailed;                                        \
        currentTest = #test;                                                   \
        ++testsRun;                                                            \
        test();                                                                \
        if (testsFailed == failedBefore) {                                     \
            printf("ok   %s\n", #test);                                        \
        }                                                                      \
    } while (0)

static int CountMaterial(const World *world, CellMaterial material)
{
    int count = 0;
    int y;

    for (y = 0; y < world->height; ++y) {
        int x;

        for (x = 0; x < world->width; ++x) {
            if (WorldGetCell(world, x, y) == material) {
                ++count;
            }
        }
    }
    return count;
}

static void FillRect(World *world, int x0, int y0, int x1, int y1,
                     CellMaterial material)
{
    int y;

    for (y = y0; y <= y1; ++y) {
        int x;

        for (x = x0; x <= x1; ++x) {
            WorldSetCell(world, x, y, material);
        }
    }
}

static void Tick(World *world, int count)
{
    int i;

    for (i = 0; i < count; ++i) {
        WorldUpdate(world);
    }
}

static bool HasGameEvent(const GameEventBuffer *events, GameEventType type)
{
    uint16_t index;

    for (index = 0u; index < events->count; ++index) {
        if (events->events[index].type == type) {
            return true;
        }
    }
    return false;
}

/* --- game boundary ----------------------------------------------------- */

typedef struct RenderProbe {
    int regions;
    uint64_t pixels;
} RenderProbe;

static bool CaptureRenderChunk(void *context, Rectangle bounds,
                               const Color *pixels,
                               const Color *emissivePixels)
{
    RenderProbe *probe = context;

    if (pixels == NULL || emissivePixels == NULL) {
        ++testsFailed;
        fprintf(stderr, "FAIL %s: render visitor received incomplete staging data\n",
                currentTest);
        return false;
    }
    ++probe->regions;
    probe->pixels += (uint64_t)bounds.width * (uint64_t)bounds.height;
    return true;
}

static void test_world_render_preparation_is_headless_and_incremental(void)
{
    World world;
    RenderProbe probe = {0};
    Rectangle wholeWorld = {0.0f, 0.0f, 64.0f, 64.0f};

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);

    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 4, "first preparation visited %d/4 chunks",
          probe.regions);
    CHECK(probe.pixels == 64u * 64u,
          "first preparation staged %llu/4096 pixels",
          (unsigned long long)probe.pixels);

    probe = (RenderProbe){0};
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 0,
          "settled renderer rebuilt %d unchanged chunks", probe.regions);

    WorldSetCell(&world, 10, 10, MATERIAL_EMPTY);
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 1,
          "one local edit rebuilt %d chunks instead of one", probe.regions);
    WorldUnload(&world);
}

typedef struct EmptyRenderProbe {
    bool sawTranslucentAir;
} EmptyRenderProbe;

static bool CaptureEmptyRenderData(void *context, Rectangle bounds,
                                   const Color *pixels,
                                   const Color *emissivePixels)
{
    EmptyRenderProbe *probe = context;
    int count = (int)(bounds.width * bounds.height);
    int index;

    for (index = 0; index < count; ++index) {
        if (pixels[index].a > 0u && pixels[index].a < 255u &&
            emissivePixels[index].a == 0u) {
            probe->sawTranslucentAir = true;
            break;
        }
    }
    return true;
}

static void test_empty_world_render_data_preserves_background_depth(void)
{
    World world;
    EmptyRenderProbe probe = {0};

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldPrepareVisible(&world, (Rectangle){0.0f, 0.0f, 32.0f, 32.0f},
                        CaptureEmptyRenderData, &probe);
    CHECK(probe.sawTranslucentAir,
          "empty world pixels still fully hide the environment background");
    WorldUnload(&world);
}

/* A chunk the renderer cannot place must keep its dirty flag. Dropping it
   instead leaves stale pixels on screen until something unrelated happens to
   change that chunk again, which is exactly the kind of bug that only shows up
   on a machine with a smaller page cache than the one it was written on. */
static bool RefuseRenderChunk(void *context, Rectangle bounds, const Color *pixels,
                              const Color *emissivePixels)
{
    (void)bounds;
    (void)pixels;
    (void)emissivePixels;
    ++(*(int *)context);
    return false;
}

typedef struct EmissiveProbe {
    bool lavaEmits;
    bool fireEmits;
    bool sandStaysDark;
} EmissiveProbe;

static bool CaptureMaterialEmission(void *context, Rectangle bounds,
                                    const Color *pixels,
                                    const Color *emissivePixels)
{
    EmissiveProbe *probe = context;

    (void)pixels;
    if (bounds.x <= 4.0f && bounds.y <= 4.0f &&
        bounds.x + bounds.width > 6.0f && bounds.y + bounds.height > 4.0f) {
        int width = (int)bounds.width;
        int row = 4 - (int)bounds.y;
        int lavaColumn = 4 - (int)bounds.x;
        int fireColumn = 5 - (int)bounds.x;
        int sandColumn = 6 - (int)bounds.x;
        Color lava = emissivePixels[row * width + lavaColumn];
        Color fire = emissivePixels[row * width + fireColumn];
        Color sand = emissivePixels[row * width + sandColumn];

        probe->lavaEmits = lava.a == 255u &&
                           (lava.r != 0u || lava.g != 0u || lava.b != 0u);
        probe->fireEmits = fire.a == 255u &&
                           (fire.r != 0u || fire.g != 0u || fire.b != 0u);
        probe->sandStaysDark = sand.r == 0u && sand.g == 0u &&
                               sand.b == 0u && sand.a == 0u;
    }
    return true;
}

static void test_emissive_render_data_selects_emitters_not_bright_terrain(void)
{
    World world;
    EmissiveProbe probe = {0};
    Rectangle wholeWorld = {0.0f, 0.0f, 32.0f, 32.0f};

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 4, 4, MATERIAL_LAVA);
    WorldSetCell(&world, 5, 4, MATERIAL_FIRE);
    WorldSetCell(&world, 6, 4, MATERIAL_SAND);

    WorldPrepareVisible(&world, wholeWorld, CaptureMaterialEmission, &probe);
    CHECK(probe.lavaEmits, "lava produced no emissive render data");
    CHECK(probe.fireEmits, "fire produced no emissive render data");
    CHECK(probe.sandStaysDark,
          "bright sand leaked into the explicit emissive mask");
    WorldUnload(&world);
}

static void test_particle_emission_is_explicit_per_effect(void)
{
    ParticleSystem particles;
    int i;
    bool laserGlows = false;
    bool boostGlows = false;

    ParticlesInit(&particles, 0xE6BEu);
    ParticlesSpawnLaserSparks(&particles, (Vector2){10.0f, 10.0f},
                              (Vector2){1.0f, 0.0f});
    ParticlesSpawnBoostTrail(&particles, (Vector2){12.0f, 10.0f},
                             (Vector2){120.0f, 0.0f}, 2);
    for (i = 0; i < MAX_PARTICLES; ++i) {
        const Particle *particle = &particles.particles[i];

        if (!particle->active || particle->emission <= 0.0f) {
            continue;
        }
        if (particle->color.r == 255u && particle->color.g == 225u) {
            laserGlows = true;
        } else {
            boostGlows = true;
        }
    }
    CHECK(laserGlows, "laser sparks carry no emissive metadata");
    CHECK(boostGlows, "boost trail carries no emissive metadata");

    /* Reinitialisation catches stale pool fields; steam must remain a visual
       particle without accidentally inheriting glow from an overwritten slot. */
    ParticlesInit(&particles, 0xE6BEu);
    ParticlesSpawnSteam(&particles, (Vector2){10.0f, 10.0f});
    for (i = 0; i < MAX_PARTICLES; ++i) {
        CHECK(!particles.particles[i].active ||
                  particles.particles[i].emission == 0.0f,
              "steam particle %d leaked into emissive", i);
    }
}

static PresentationFxDescription TestFxDescription(PresentationFxType type,
                                                    PresentationFxPriority priority,
                                                    float lifetime)
{
    return (PresentationFxDescription){
        .type = type,
        .priority = priority,
        .start = {12.0f, 18.0f},
        .end = {24.0f, 20.0f},
        .color = {255, 180, 80, 255},
        .startRadius = 1.0f,
        .endRadius = 8.0f,
        .width = 1.5f,
        .intensity = 0.8f,
        .lifetime = lifetime,
        .emissive = true,
    };
}

static void test_presentation_fx_spawn_update_and_expire(void)
{
    PresentationFxSystem fx;
    int type;

    PresentationFxInit(&fx);
    for (type = 0; type < PRESENTATION_FX_TYPE_COUNT; ++type) {
        CHECK(PresentationFxSpawn(
                  &fx, TestFxDescription((PresentationFxType)type,
                                         PRESENTATION_FX_PRIORITY_NORMAL,
                                         0.20f)),
              "primitive type %d did not spawn", type);
    }
    CHECK(fx.stats.active == (uint16_t)PRESENTATION_FX_TYPE_COUNT,
          "active count is %u after spawning %d primitive types",
          (unsigned int)fx.stats.active, PRESENTATION_FX_TYPE_COUNT);
    CHECK(fx.stats.peak == fx.stats.active, "peak did not follow active count");

    PresentationFxUpdate(&fx, 0.10f);
    CHECK(fx.stats.active == (uint16_t)PRESENTATION_FX_TYPE_COUNT,
          "FX expired before its lifetime");
    PresentationFxUpdate(&fx, 0.10f);
    CHECK(fx.stats.active == 0u, "expired FX remained active");
    CHECK(fx.stats.peak == (uint16_t)PRESENTATION_FX_TYPE_COUNT,
          "expiration incorrectly reset peak telemetry");
}

static void test_presentation_fx_rejects_invalid_lifetime_and_clears(void)
{
    PresentationFxSystem fx;
    PresentationFxDescription description =
        TestFxDescription(PRESENTATION_FX_FLASH,
                          PRESENTATION_FX_PRIORITY_NORMAL, 0.0f);

    PresentationFxInit(&fx);
    CHECK(!PresentationFxSpawn(&fx, description),
          "zero-lifetime FX was accepted");
    description.lifetime = NAN;
    CHECK(!PresentationFxSpawn(&fx, description),
          "non-finite FX lifetime was accepted");
    description.lifetime = 0.25f;
    description.delay = -0.1f;
    CHECK(!PresentationFxSpawn(&fx, description),
          "negative FX delay was accepted");
    CHECK(fx.stats.active == 0u, "invalid FX changed active count");
    CHECK(fx.stats.dropped == 3u,
          "invalid FX did not increment dropped telemetry");

    description.delay = 0.0f;
    CHECK(PresentationFxSpawn(&fx, description), "valid FX did not spawn");
    PresentationFxClear(&fx);
    CHECK(fx.stats.active == 0u && fx.stats.peak == 0u &&
              fx.stats.dropped == 0u,
          "clear did not reset FX state and telemetry");
}

static void test_presentation_fx_capacity_has_bounded_priority_overflow(void)
{
    PresentationFxSystem fx;
    PresentationFxDescription low =
        TestFxDescription(PRESENTATION_FX_GLOW,
                          PRESENTATION_FX_PRIORITY_LOW, 1.0f);
    PresentationFxDescription high =
        TestFxDescription(PRESENTATION_FX_RING,
                          PRESENTATION_FX_PRIORITY_HIGH, 1.0f);
    unsigned int index;
    bool foundHigh = false;

    PresentationFxInit(&fx);
    for (index = 0u; index < PRESENTATION_FX_CAPACITY; ++index) {
        CHECK(PresentationFxSpawn(&fx, low),
              "low-priority FX %u/%u did not fit", index,
              PRESENTATION_FX_CAPACITY);
    }
    CHECK(PresentationFxSpawn(&fx, high),
          "high-priority FX did not replace a low-priority effect");
    CHECK(fx.stats.active == PRESENTATION_FX_CAPACITY,
          "overflow changed bounded active count to %u",
          (unsigned int)fx.stats.active);
    CHECK(fx.stats.peak == PRESENTATION_FX_CAPACITY,
          "capacity was not recorded as peak");
    CHECK(fx.stats.dropped == 1u,
          "replacement did not count one dropped effect");
    for (index = 0u; index < fx.stats.active; ++index) {
        if (fx.effects[index].description.priority ==
            PRESENTATION_FX_PRIORITY_HIGH) {
            foundHigh = true;
            break;
        }
    }
    CHECK(foundHigh, "replacement policy lost the incoming high-priority FX");

    PresentationFxClear(&fx);
    for (index = 0u; index < PRESENTATION_FX_CAPACITY; ++index) {
        CHECK(PresentationFxSpawn(&fx, high),
              "high-priority FX %u/%u did not fit", index,
              PRESENTATION_FX_CAPACITY);
    }
    CHECK(!PresentationFxSpawn(&fx, low),
          "low-priority FX evicted a high-priority effect");
    CHECK(fx.stats.active == PRESENTATION_FX_CAPACITY &&
              fx.stats.dropped == 1u,
          "rejected overflow corrupted capacity telemetry");
}

static void test_presentation_fx_events_create_visuals_without_mutating_events(void)
{
    PresentationFxSystem fx;
    GameEventBuffer events = {0};
    GameEventBuffer before;
    uint16_t spawned;
    uint16_t index;
    int flashes = 0;
    int rings = 0;
    int glows = 0;
    int trails = 0;
    int puffs = 0;

    events.events[0] = (GameEvent){
        .type = GAME_EVENT_EXPLOSION,
        .position = {48.0f, 52.0f},
        .radius = 42.0f,
    };
    events.events[1] = (GameEvent){
        .type = GAME_EVENT_PLAYER_IMPACT,
        .position = {30.0f, 22.0f},
        .strength = 90.0f,
    };
    events.count = 2u;
    before = events;

    PresentationFxInit(&fx);
    spawned = PresentationFxConsumeEvents(&fx, &events);
    CHECK(spawned == 23u && fx.stats.active == 23u,
          "two events created %u FX instead of 23", (unsigned int)spawned);
    for (index = 0u; index < fx.stats.active; ++index) {
        if (fx.effects[index].description.type == PRESENTATION_FX_FLASH) {
            ++flashes;
        } else if (fx.effects[index].description.type == PRESENTATION_FX_RING) {
            ++rings;
        } else if (fx.effects[index].description.type == PRESENTATION_FX_GLOW) {
            ++glows;
        } else if (fx.effects[index].description.type == PRESENTATION_FX_TRAIL) {
            ++trails;
        } else if (fx.effects[index].description.type == PRESENTATION_FX_PUFF) {
            ++puffs;
        }
    }
    CHECK(flashes == 2 && rings == 2 && glows == 2 && trails == 10 &&
              puffs == 7,
          "event conversion created flash=%d ring=%d glow=%d trail=%d puff=%d",
          flashes, rings, glows, trails, puffs);
    CHECK(memcmp(&events, &before, sizeof(events)) == 0,
          "presentation event conversion mutated gameplay events");
}

static void test_presentation_fx_delays_a_stage_without_shortening_it(void)
{
    PresentationFxSystem fx;
    PresentationFxDescription description =
        TestFxDescription(PRESENTATION_FX_GLOW,
                          PRESENTATION_FX_PRIORITY_NORMAL, 0.20f);

    description.delay = 0.15f;
    PresentationFxInit(&fx);
    CHECK(PresentationFxSpawn(&fx, description), "delayed FX did not spawn");
    CHECK(fx.effects[0].age == -0.15f, "delay was not represented in FX age");
    PresentationFxUpdate(&fx, 0.15f);
    CHECK(fx.stats.active == 1u && fabsf(fx.effects[0].age) < 0.0001f,
          "delayed FX was shortened before it became visible");
    PresentationFxUpdate(&fx, 0.19f);
    CHECK(fx.stats.active == 1u, "delayed FX expired before its own lifetime");
    PresentationFxUpdate(&fx, 0.01f);
    CHECK(fx.stats.active == 0u, "delayed FX outlived delay plus lifetime");
}

static void test_presentation_fx_maps_every_combat_event_with_bounded_rates(void)
{
    PresentationFxSystem fx;
    GameEventBuffer events = {0};
    uint16_t firstSpawn;
    uint16_t repeatedSpawn;

    events.events[events.count++] = (GameEvent){
        .type = GAME_EVENT_LASER_HIT,
        .position = {20.0f, 20.0f},
        .direction = {1.0f, 0.0f},
    };
    events.events[events.count++] = (GameEvent){
        .type = GAME_EVENT_CRYO_HIT,
        .position = {22.0f, 20.0f},
        .direction = {1.0f, 0.0f},
    };
    events.events[events.count++] = (GameEvent){
        .type = GAME_EVENT_FORCE,
        .position = {10.0f, 20.0f},
        .direction = {1.0f, 0.0f},
        .radius = 84.0f,
    };
    events.events[events.count++] = (GameEvent){
        .type = GAME_EVENT_BOOST_STAGE,
        .position = {10.0f, 20.0f},
        .direction = {160.0f, 0.0f},
        .count = 3,
    };
    events.events[events.count++] = (GameEvent){
        .type = GAME_EVENT_PLAYER_DRILL,
        .position = {24.0f, 20.0f},
        .direction = {160.0f, 0.0f},
        .material = MATERIAL_ROCK,
        .count = 12,
    };

    PresentationFxInit(&fx);
    firstSpawn = PresentationFxConsumeEvents(&fx, &events);
    CHECK(firstSpawn >= 25u && fx.stats.active == firstSpawn,
          "combat event set spawned only %u FX", (unsigned int)firstSpawn);
    repeatedSpawn = PresentationFxConsumeEvents(&fx, &events);
    CHECK(repeatedSpawn < firstSpawn,
          "held contacts ignored presentation spawn cooldowns");
    CHECK(fx.stats.active <= PRESENTATION_FX_CAPACITY,
          "combat event conversion exceeded fixed FX capacity");
}

static void test_laser_contact_heat_uses_elapsed_time_not_frame_count(void)
{
    PresentationFxSystem oneFrame;
    PresentationFxSystem twoFrames;
    GameEventBuffer events = {0};

    events.events[0] = (GameEvent){
        .type = GAME_EVENT_LASER_HIT,
        .position = {20.0f, 20.0f},
        .direction = {1.0f, 0.0f},
    };
    events.count = 1u;
    PresentationFxInit(&oneFrame);
    PresentationFxInit(&twoFrames);
    (void)PresentationFxConsumeEvents(&oneFrame, &events);
    (void)PresentationFxConsumeEvents(&twoFrames, &events);

    PresentationFxUpdate(&oneFrame, 0.10f);
    (void)PresentationFxConsumeEvents(&oneFrame, &events);
    PresentationFxUpdate(&twoFrames, 0.05f);
    (void)PresentationFxConsumeEvents(&twoFrames, &events);
    PresentationFxUpdate(&twoFrames, 0.05f);
    (void)PresentationFxConsumeEvents(&twoFrames, &events);

    CHECK(fabsf(oneFrame.laserContactTime - 0.10f) < 0.0001f &&
              fabsf(twoFrames.laserContactTime - 0.10f) < 0.0001f,
          "laser contact heat depends on presentation frame count");
}

static void test_camera_feedback_stacks_clamps_and_expires(void)
{
    CameraFeedback feedback;
    GameEventBuffer events = {0};
    GameEventBuffer before;
    CameraFeedbackOutput output;
    int index;

    for (index = 0; index < 24; ++index) {
        events.events[events.count++] = (GameEvent){
            .type = GAME_EVENT_EXPLOSION,
            .position = {(float)index, 10.0f},
            .radius = 42.0f,
        };
    }
    before = events;
    CameraFeedbackInit(&feedback);
    CameraFeedbackConsumeEvents(&feedback, &events, (Vector2){80.0f, 40.0f});
    CHECK(memcmp(&events, &before, sizeof(events)) == 0,
          "camera feedback mutated gameplay events");
    CHECK(feedback.stats.active == CAMERA_IMPULSE_CAPACITY &&
              feedback.stats.peak == CAMERA_IMPULSE_CAPACITY &&
              feedback.stats.dropped == 8u,
          "camera impulse stack did not enforce its capacity");
    output = CameraFeedbackUpdate(
        &feedback,
        (CameraFeedbackMotion){.normalSpeed = 115.0f,
                               .maximumSpeed = 680.0f,
                               .viewWidth = 320.0f,
                               .viewHeight = 180.0f},
        1.0f / 60.0f);
    CHECK(sqrtf(output.impulseOffset.x * output.impulseOffset.x +
                output.impulseOffset.y * output.impulseOffset.y) <= 8.001f,
          "stacked camera offset escaped its clamp");
    CHECK(fabsf(output.rotationDegrees) <= 1.101f &&
              output.zoomKick <= 0.101f,
          "stacked rotation/zoom escaped camera clamps");
    (void)CameraFeedbackUpdate(
        &feedback,
        (CameraFeedbackMotion){.normalSpeed = 115.0f,
                               .maximumSpeed = 680.0f,
                               .viewWidth = 320.0f,
                               .viewHeight = 180.0f},
        1.0f);
    CHECK(feedback.stats.active == 0u,
          "expired camera impulses remained in the stack");
}

static void test_camera_lookahead_damps_reversal_before_leading_backward(void)
{
    CameraFeedback feedback;
    CameraFeedbackMotion motion = {
        .velocity = {300.0f, 0.0f},
        .normalSpeed = 115.0f,
        .maximumSpeed = 680.0f,
        .viewWidth = 320.0f,
        .viewHeight = 180.0f,
    };
    CameraFeedbackOutput output = {0};
    int step;

    CameraFeedbackInit(&feedback);
    for (step = 0; step < 30; ++step) {
        output = CameraFeedbackUpdate(&feedback, motion, 1.0f / 60.0f);
    }
    CHECK(output.lookahead.x > 20.0f, "camera never developed speed lookahead");
    motion.velocity.x = -300.0f;
    output = CameraFeedbackUpdate(&feedback, motion, 1.0f / 60.0f);
    CHECK(output.lookahead.x > 0.0f,
          "camera whipped through the player on the first reversal frame");
    for (step = 0; step < 120; ++step) {
        output = CameraFeedbackUpdate(&feedback, motion, 1.0f / 60.0f);
    }
    CHECK(output.lookahead.x < -20.0f,
          "camera never settled into reversed speed lookahead");
}

static void test_camera_feedback_invalid_time_preserves_safe_stable_state(void)
{
    CameraFeedback feedback;
    CameraFeedback before;
    CameraFeedbackOutput output;

    CameraFeedbackInit(&feedback);
    feedback.lookahead = (Vector2){18.0f, -7.0f};
    feedback.viewScale = 1.35f;
    before = feedback;

    output = CameraFeedbackUpdate(&feedback, (CameraFeedbackMotion){0}, 0.0f);
    CHECK(output.viewScale >= 1.0f &&
              fabsf(output.viewScale - before.viewScale) < 0.0001f,
          "zero-dt camera update returned unsafe view scale %.3f",
          (double)output.viewScale);
    CHECK(Vector2Distance(output.lookahead, before.lookahead) < 0.0001f &&
              Vector2LengthSqr(output.impulseOffset) == 0.0f &&
              output.rotationDegrees == 0.0f && output.zoomKick == 0.0f,
          "zero-dt camera update changed stable or transient output");
    CHECK(memcmp(&feedback, &before, sizeof(feedback)) == 0,
          "zero-dt camera update mutated controller state");

    output = CameraFeedbackUpdate(&feedback, (CameraFeedbackMotion){0}, NAN);
    CHECK(output.viewScale >= 1.0f &&
              fabsf(output.viewScale - before.viewScale) < 0.0001f &&
              Vector2Distance(output.lookahead, before.lookahead) < 0.0001f,
          "NaN-dt camera update did not preserve its stable state");
    CHECK(Vector2LengthSqr(output.impulseOffset) == 0.0f &&
              output.rotationDegrees == 0.0f && output.zoomKick == 0.0f &&
              memcmp(&feedback, &before, sizeof(feedback)) == 0,
          "NaN-dt camera update emitted or retained a transient impulse");

    feedback.viewScale = NAN;
    output = CameraFeedbackUpdate(&feedback, (CameraFeedbackMotion){0}, 0.0f);
    CHECK(output.viewScale == 1.0f,
          "invalid stored view scale was not sanitized: %.3f",
          (double)output.viewScale);
}

static void test_transient_camera_never_changes_mouse_aim_transform(void)
{
    Camera2D stable = {
        .offset = {640.0f, 360.0f},
        .target = {420.0f, 180.0f},
        .rotation = 0.0f,
        .zoom = 4.0f,
    };
    Camera2D before = stable;
    Vector2 mouse = {917.0f, 441.0f};
    Vector2 aimWorld = GetScreenToWorld2D(mouse, stable);
    Camera2D presentation = CameraFeedbackApplyTransient(
        stable,
        (CameraFeedbackOutput){
            .impulseOffset = {7.0f, -5.0f},
            .rotationDegrees = 1.0f,
            .zoomKick = 0.1f,
        });
    Vector2 reticleScreen = GetWorldToScreen2D(aimWorld, stable);
    Vector2 shakenScreen = GetWorldToScreen2D(aimWorld, presentation);

    CHECK(memcmp(&stable, &before, sizeof(stable)) == 0,
          "applying camera feedback mutated the stable aim camera");
    CHECK(Vector2Distance(reticleScreen, mouse) < 0.001f,
          "stable aim/reticle transform drifted from the mouse by %.4f px",
          (double)Vector2Distance(reticleScreen, mouse));
    CHECK(Vector2Distance(shakenScreen, mouse) > 1.0f,
          "camera test did not exercise a meaningful transient transform");
    CHECK(Vector2Distance(presentation.target, stable.target) > 1.0f &&
              presentation.rotation != stable.rotation &&
              presentation.zoom < stable.zoom,
          "transient feedback was not applied to the presentation copy");
}

static void test_a_refused_chunk_keeps_its_dirty_flag(void)
{
    World world;
    RenderProbe probe = {0};
    Rectangle wholeWorld = {0.0f, 0.0f, 64.0f, 64.0f};
    int refused = 0;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);

    WorldPrepareVisible(&world, wholeWorld, RefuseRenderChunk, &refused);
    CHECK(refused == 4, "first preparation offered %d/4 chunks", refused);

    /* Nothing was accepted, so the same four chunks must still be owed. */
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 4,
          "a refused chunk was forgotten: %d/4 offered again", probe.regions);

    probe = (RenderProbe){0};
    WorldPrepareVisible(&world, wholeWorld, CaptureRenderChunk, &probe);
    CHECK(probe.regions == 0,
          "an accepted chunk was rebuilt again: %d chunks", probe.regions);
    WorldUnload(&world);
}

static void test_game_event_buffer_is_fixed_and_ordered(void)
{
    GameEventBuffer events = {0};
    int index;

    for (index = 0; index < MAX_GAME_EVENTS; ++index) {
        CHECK(GameEventsPush(&events, (GameEvent){
                  .type = GAME_EVENT_PLAYER_DRILL,
                  .count = index,
              }),
              "event %d was rejected before capacity", index);
    }
    CHECK(events.count == MAX_GAME_EVENTS, "buffer stored %u/%d events",
          events.count, MAX_GAME_EVENTS);
    CHECK(events.events[17].count == 17, "event order changed at index 17");
    CHECK(!GameEventsPush(&events, (GameEvent){.type = GAME_EVENT_EXPLOSION}),
          "event buffer grew past fixed capacity");
    CHECK(events.dropped == 1u, "overflow reported %u dropped events",
          events.dropped);

    GameEventsClear(&events);
    CHECK(events.count == 0u && events.dropped == 0u,
          "clear left count=%u dropped=%u", events.count, events.dropped);
}

static void test_game_update_publishes_transient_events(void)
{
    GameConfig config = GameDefaultConfig();
    GameState game;
    GameEventBuffer events = {0};
    GameInput input = {0};
    int cellX;
    int cellY;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0xE6BEu;
    CHECK(GameInit(&game, config), "game allocation failed");

    input.aimWorld = (Vector2){game.player.position.x + 20.0f,
                               game.player.position.y};
    input.ability[ABILITY_EXPLOSION] = true;
    GameUpdate(&game, &input, config.fixedStep, &events);
    CHECK(HasGameEvent(&events, GAME_EVENT_EXPLOSION),
          "explosion did not publish a game event");

    cellX = (int)game.player.position.x + 40;
    cellY = (int)game.player.position.y + 20;
    WorldSetCell(&game.world, cellX, cellY, MATERIAL_WATER);
    WorldSetCell(&game.world, cellX + 1, cellY, MATERIAL_LAVA);
    input = (GameInput){.aimWorld = game.player.position};
    GameUpdate(&game, &input, config.fixedStep, &events);
    CHECK(HasGameEvent(&events, GAME_EVENT_MATERIAL_REACTION),
          "water/lava reaction did not cross the GameEvents boundary");
    CHECK(!HasGameEvent(&events, GAME_EVENT_EXPLOSION),
          "one-frame explosion event leaked into the next update");
    GameUnload(&game);
}

/* Cells hold the low sixteen bits of the tick counter. The saving is 54 MiB,
   and the price is that the counter wraps; this checks that the wrap costs
   nothing anyone can see, and in particular that the truncated value never
   becomes zero, which is what would make every never-written cell in an awake
   chunk skip the same tick together. */
static void test_the_tick_counter_survives_wrapping_its_cell_stamp(void)
{
    World world;
    int before;
    int step;
    int lowest = 0;

    CHECK(WorldInit(&world, 64, 96), "world allocation failed");
    FillRect(&world, 20, 10, 40, 20, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);

    /* Jump to just before the wrap rather than ticking there: the behaviour
       under test is the arithmetic, not sixty-five thousand ticks of sand. */
    world.tick = 0xFFFAu;
    for (step = 0; step < 40; ++step) {
        WorldUpdate(&world);
        CHECK((uint16_t)world.tick != 0u,
              "the tick stamp reached zero, which unwritten cells already hold");
    }

    CHECK(world.tick > 0x10000u, "the test never reached the wrap: tick=%u",
          world.tick);
    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "sand was lost across the tick stamp wrap: %d of %d left",
          CountMaterial(&world, MATERIAL_SAND), before);

    for (step = 0; step < world.height; ++step) {
        int x;

        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, step) == MATERIAL_SAND && step > lowest) {
                lowest = step;
            }
        }
    }
    CHECK(lowest > 20, "sand stopped falling across the wrap; lowest row %d",
          lowest);
    WorldUnload(&world);
}

/* --- active chunk scheduler --------------------------------------------- */

/* The schedule exists in two representations — a flag per chunk and a compact
   per-row list — and every wake path has to keep them agreeing. A duplicate in
   a list would simulate a chunk twice in one tick; a flag with no list entry
   would drop it silently. */
static void CheckScheduleIsConsistent(const World *world, const char *when)
{
    int listed = 0;
    int flagged = 0;
    int chunkY;
    int chunkX;

    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int slot;
        int previous = -1;

        listed += (int)world->activeRowCount[chunkY];
        for (slot = 0; slot < (int)world->activeRowCount[chunkY]; ++slot) {
            int column = (int)world->activeRowColumns[(size_t)chunkY *
                                                          (size_t)world->chunkColumns +
                                                      (size_t)slot];

            CHECK(column >= 0 && column < world->chunkColumns,
                  "%s: scheduled column %d is out of range", when, column);
            CHECK(world->activeChunks[(size_t)chunkY * (size_t)world->chunkColumns +
                                      (size_t)column] != 0u,
                  "%s: chunk %d,%d is listed but not flagged", when, column, chunkY);
            /* WorldUpdate sorts each row before walking it, so after a tick a
               repeat would show up as a non-increasing pair. */
            CHECK(column != previous,
                  "%s: chunk %d,%d appears twice in the schedule", when, column,
                  chunkY);
            previous = column;
        }
    }
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            if (world->activeChunks[(size_t)chunkY * (size_t)world->chunkColumns +
                                    (size_t)chunkX] != 0u) {
                ++flagged;
            }
        }
    }
    CHECK(listed == flagged, "%s: %d chunks listed but %d flagged", when, listed,
          flagged);
    CHECK(world->activeChunkCount == flagged,
          "%s: reported %d active chunks but %d are flagged", when,
          world->activeChunkCount, flagged);
}

static void test_the_schedule_never_lists_a_chunk_twice(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 256, 160), "world allocation failed");
    /* Sand pouring across a chunk boundary and lava beside water: both wake
       neighbours constantly, from inside a tick and from outside one. */
    FillRect(&world, 20, 10, 44, 30, MATERIAL_SAND);
    FillRect(&world, 60, 100, 90, 110, MATERIAL_WATER);
    FillRect(&world, 91, 100, 120, 110, MATERIAL_LAVA);
    CheckScheduleIsConsistent(&world, "after seeding");

    for (step = 0; step < 60; ++step) {
        WorldUpdate(&world);
        CheckScheduleIsConsistent(&world, "during simulation");
        if (step == 30) {
            /* A mutation from outside a tick has to schedule the tick that is
               about to run, not the one that just finished. */
            WorldDrillCircle(&world, 200, 80, 5);
            CheckScheduleIsConsistent(&world, "after an out-of-tick mutation");
            CHECK(world.activeChunkCount > 0,
                  "drilling between ticks scheduled nothing");
        }
    }
    WorldUnload(&world);
}

/* --- determinism --------------------------------------------------------- */

/* A cheap order-sensitive digest of everything the simulation owns. Comparing
   whole worlds cell by cell would work too, but a single number makes a failure
   message readable and keeps the two-world comparisons below short. */
static uint64_t WorldDigest(const World *world)
{
    uint64_t digest = 0xcbf29ce484222325ull;
    int y;

    for (y = 0; y < world->height; ++y) {
        int x;

        for (x = 0; x < world->width; ++x) {
            const Cell *cell = &world->cells[(size_t)y * (size_t)world->width +
                                             (size_t)x];

            digest = (digest ^ (uint64_t)cell->material) * 0x100000001b3ull;
            digest = (digest ^ (uint64_t)(int64_t)(cell->temperature * 16.0f)) *
                     0x100000001b3ull;
        }
    }
    return digest;
}

static void test_environment_palettes_validate_parse_and_force(void)
{
    EnvironmentRenderer environment;
    EnvironmentPalette parsed = ENVIRONMENT_PALETTE_AUTO;
    EnvironmentPalette seeded = EnvironmentPaletteForSeed(0xE6BEu);
    int index;

    CHECK(EnvironmentPalettesValidate(),
          "environment palette table failed validation");
    CHECK(seeded >= 0 && seeded < ENVIRONMENT_PALETTE_COUNT,
          "seed selected invalid palette %d", (int)seeded);
    for (index = 0; index < ENVIRONMENT_PALETTE_COUNT; ++index) {
        const EnvironmentPaletteDefinition *definition =
            EnvironmentPaletteDefinitionAt((EnvironmentPalette)index);

        CHECK(definition != NULL && definition->name != NULL &&
                  definition->cliName != NULL,
              "palette %d has no complete definition", index);
        CHECK(EnvironmentPaletteParse(definition->cliName, &parsed) &&
                  parsed == (EnvironmentPalette)index,
              "palette '%s' did not parse back to %d", definition->cliName,
              index);
    }
    CHECK(EnvironmentPaletteDefinitionAt(ENVIRONMENT_PALETTE_AUTO) == NULL &&
              !EnvironmentPaletteParse("molten-copy", &parsed),
          "invalid environment palette was accepted");

    EnvironmentRendererInit(&environment, 0xE6BEu,
                            ENVIRONMENT_PALETTE_ABYSSAL_BLUE);
    CHECK(environment.palette == ENVIRONMENT_PALETTE_ABYSSAL_BLUE &&
              environment.forcedPalette == ENVIRONMENT_PALETTE_ABYSSAL_BLUE,
          "forced palette was ignored");
    EnvironmentRendererSyncSeed(&environment, 0x1234u);
    CHECK(environment.palette == ENVIRONMENT_PALETTE_ABYSSAL_BLUE,
          "world seed replaced a forced palette");
    CHECK(EnvironmentRendererSetPalette(&environment,
                                        ENVIRONMENT_PALETTE_AUTO) &&
              environment.palette == EnvironmentPaletteForSeed(0x1234u),
          "auto palette did not return to deterministic seed selection");
    CHECK(!EnvironmentRendererSetPalette(
              &environment, (EnvironmentPalette)ENVIRONMENT_PALETTE_COUNT),
          "out-of-range forced palette was accepted");
}

static void test_environment_descriptors_are_seeded_and_finite(void)
{
    EnvironmentRenderer first;
    EnvironmentRenderer second;
    EnvironmentRenderer different;

    EnvironmentRendererInit(&first, 0xE6BEu, ENVIRONMENT_PALETTE_AUTO);
    EnvironmentRendererInit(&second, 0xE6BEu, ENVIRONMENT_PALETTE_AUTO);
    EnvironmentRendererInit(&different, 0xA11CEu, ENVIRONMENT_PALETTE_AUTO);

    CHECK(memcmp(&first, &second, sizeof(first)) == 0,
          "same seed produced different environment descriptors");
    CHECK(first.palette == EnvironmentPaletteForSeed(0xE6BEu),
          "auto palette disagrees with seed selector");
    CHECK(memcmp(first.structures, different.structures,
                 sizeof(first.structures)) != 0,
          "different seeds produced identical environment structures");
    CHECK(EnvironmentRendererStateIsValid(&first) &&
              EnvironmentRendererStateIsValid(&different),
          "seed generated invalid or non-finite environment geometry");
}

static void test_environment_view_handles_resize_rotation_and_invalid_time(void)
{
    EnvironmentRenderer environment;
    EnvironmentRenderer before;
    Camera2D camera = {
        .offset = {640.0f, 360.0f},
        .target = {8192.0f, 220.0f},
        .rotation = 1.1f,
        .zoom = 2.0f,
    };
    Rectangle wide = EnvironmentRendererOverscanBounds(1920, 1080);
    Rectangle small = EnvironmentRendererOverscanBounds(640, 360);

    EnvironmentRendererInit(&environment, 0xE6BEu,
                            ENVIRONMENT_PALETTE_VERDIGRIS_STORM);
    before = environment;
    CHECK(EnvironmentRendererViewIsValid(camera, 1920, 1080) &&
              EnvironmentRendererViewIsValid(camera, 640, 360),
          "valid rotated/zoomed camera or resize was rejected");
    CHECK(wide.x < 0.0f && wide.y < 0.0f && wide.width > 1920.0f &&
              wide.height > 1080.0f && small.width > 640.0f &&
              small.height > 360.0f,
          "environment overscan does not cover rotated view corners");
    CHECK(memcmp(&environment, &before, sizeof(environment)) == 0,
          "resize/view helper mutated persistent environment state");

    EnvironmentRendererUpdate(&environment, 0.0f);
    EnvironmentRendererUpdate(&environment, NAN);
    CHECK(environment.time == before.time,
          "zero/NaN presentation time advanced environment animation");
    camera.zoom = NAN;
    CHECK(!EnvironmentRendererViewIsValid(camera, 1280, 720) &&
              !EnvironmentRendererViewIsValid((Camera2D){0}, 0, 720),
          "invalid camera or target dimensions were accepted");
}

static void test_environment_state_never_changes_gameplay_world(void)
{
    World world;
    EnvironmentRenderer environment;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);
    before = WorldDigest(&world);
    EnvironmentRendererInit(&environment, world.seed,
                            ENVIRONMENT_PALETTE_AUTO);
    EnvironmentRendererUpdate(&environment, 1.0f / 60.0f);
    (void)EnvironmentRendererSetPalette(
        &environment, ENVIRONMENT_PALETTE_EMBER_WASTE);
    EnvironmentRendererSyncSeed(&environment, 0x1234u);
    CHECK(WorldDigest(&world) == before,
          "presentation environment changed the gameplay world digest");
    WorldUnload(&world);
}

static int FirstSolidY(const World *world, int x)
{
    int y;

    for (y = 0; y < world->height; ++y) {
        if (WorldMaterialIsSolid(WorldGetCell(world, x, y))) return y;
    }
    return world->height;
}

static void test_biome_layout_is_seeded_complete_and_bounded(void)
{
    World first = {.width = 16384, .height = 864, .seed = 0x51EDu};
    World second = {.width = 16384, .height = 864, .seed = 0x51EDu};
    bool seen[WORLD_BIOME_COUNT] = {false};
    int x;
    int biome;

    for (x = 0; x < first.width; x += 128) {
        WorldBiome firstBiome = WorldBiomeAt(&first, x);
        WorldBiome secondBiome = WorldBiomeAt(&second, x);

        CHECK(firstBiome == secondBiome,
              "same seed disagreed about biome at x=%d", x);
        CHECK(firstBiome >= 0 && firstBiome < WORLD_BIOME_COUNT,
              "column %d returned invalid biome %d", x, firstBiome);
        seen[firstBiome] = true;
    }
    for (biome = 0; biome < WORLD_BIOME_COUNT; ++biome) {
        CHECK(seen[biome], "production width contains no %s",
              WorldBiomeName((WorldBiome)biome));
        CHECK(strcmp(WorldBiomeName((WorldBiome)biome), "UNKNOWN") != 0,
              "biome %d has no debug name", biome);
    }
    CHECK(WorldBiomeAt(&first, -100) == WorldBiomeAt(&first, 0),
          "negative x did not clamp to the first biome");
    CHECK(WorldBiomeAt(&first, first.width + 100) ==
              WorldBiomeAt(&first, first.width - 1),
          "past-end x did not clamp to the last biome");
}

static void test_generated_biomes_have_distinct_material_identity(void)
{
    World world;
    int signatures[WORLD_BIOME_COUNT] = {0};
    int x;

    /* Six nominal regions are enough to include one full four-biome cycle,
       while keeping this structural generation test far below production RAM. */
    CHECK(WorldInit(&world, 8192, 288), "world allocation failed");
    WorldGenerate(&world, 0xB10B1E5u);

    for (x = 0; x < world.width; ++x) {
        WorldBiome biome = WorldBiomeAt(&world, x);
        int y;

        for (y = 0; y < world.height; ++y) {
            CellMaterial material = WorldGetCell(&world, x, y);

            if ((biome == WORLD_BIOME_TEMPERATE && material == MATERIAL_DIRT) ||
                (biome == WORLD_BIOME_DUNES && material == MATERIAL_SAND) ||
                (biome == WORLD_BIOME_FROST && material == MATERIAL_ICE) ||
                (biome == WORLD_BIOME_VOLCANIC && material == MATERIAL_LAVA)) {
                ++signatures[biome];
            }
        }
    }

    CHECK(signatures[WORLD_BIOME_TEMPERATE] > 1000,
          "temperate terrain has only %d dirt cells",
          signatures[WORLD_BIOME_TEMPERATE]);
    CHECK(signatures[WORLD_BIOME_DUNES] > 1000,
          "dunes have only %d sand cells", signatures[WORLD_BIOME_DUNES]);
    CHECK(signatures[WORLD_BIOME_FROST] > 300,
          "frost shelf has only %d ice cells", signatures[WORLD_BIOME_FROST]);
    CHECK(signatures[WORLD_BIOME_VOLCANIC] > 100,
          "ember wastes have only %d lava cells",
          signatures[WORLD_BIOME_VOLCANIC]);
    WorldUnload(&world);
}

static void test_biome_boundaries_and_spawn_are_coherent(void)
{
    World world;
    Vector2 spawn;
    int boundary;
    int checkedBoundaries = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 8192, 288), "world allocation failed");
    WorldGenerate(&world, 0xB10B1E5u);

    /* A biome boundary may change strata, but never creates the vertical wall
       the old per-region surface profiles would have produced. Cave mouths and
       authored landmarks can still make real cliffs away from boundaries. */
    for (boundary = 1; boundary < world.width; ++boundary) {
        if (WorldBiomeAt(&world, boundary - 1) !=
            WorldBiomeAt(&world, boundary)) {
            int leftY = FirstSolidY(&world, boundary - 1);
            int rightY = FirstSolidY(&world, boundary);
            int difference = leftY - rightY;

            if (difference < 0) difference = -difference;
            CHECK(difference <= 6,
                  "biome seam at x=%d jumps %d cells (%d -> %d)", boundary,
                  difference, leftY, rightY);
            ++checkedBoundaries;
        }
    }
    CHECK(checkedBoundaries >= 3,
          "wide test world exposed only %d biome boundaries",
          checkedBoundaries);

    spawn = WorldPlayerSpawn(&world);
    CHECK((int)spawn.x == world.width / 2,
          "spawn moved away from the protected center plateau");
    for (y = (int)spawn.y - 6; y <= (int)spawn.y + 6; ++y) {
        for (x = (int)spawn.x - 6; x <= (int)spawn.x + 6; ++x) {
            CHECK(!WorldMaterialIsSolid(WorldGetCell(&world, x, y)),
                  "spawn clearance contains solid terrain at %d,%d", x, y);
        }
    }
    CHECK(FirstSolidY(&world, (int)spawn.x) > (int)spawn.y &&
              FirstSolidY(&world, (int)spawn.x) - (int)spawn.y <= 12,
          "spawn has no nearby floor: player y=%d floor y=%d", (int)spawn.y,
          FirstSolidY(&world, (int)spawn.x));
    WorldUnload(&world);
}

static void test_every_biome_can_host_the_protected_spawn(void)
{
    World world;
    bool checked[WORLD_BIOME_COUNT] = {false};
    int checkedCount = 0;
    uint64_t seed;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    for (seed = 1u; seed <= 128u && checkedCount < WORLD_BIOME_COUNT; ++seed) {
        WorldBiome biome;
        Vector2 spawn;
        int floorY;

        WorldGenerate(&world, seed);
        biome = WorldBiomeAt(&world, world.width / 2);
        if (checked[biome]) continue;

        spawn = WorldPlayerSpawn(&world);
        floorY = FirstSolidY(&world, (int)spawn.x);
        CHECK(floorY > (int)spawn.y && floorY - (int)spawn.y <= 12,
              "%s seed 0x%llx spawned at y=%d with floor y=%d",
              WorldBiomeName(biome), (unsigned long long)seed, (int)spawn.y,
              floorY);
        checked[biome] = true;
        ++checkedCount;
    }
    CHECK(checkedCount == WORLD_BIOME_COUNT,
          "seeded spawn cycle exposed only %d/%d biomes", checkedCount,
          WORLD_BIOME_COUNT);
    WorldUnload(&world);
}

static void test_the_same_seed_always_generates_the_same_world(void)
{
    World first;
    World second;

    CHECK(WorldInit(&first, 256, 144), "world allocation failed");
    CHECK(WorldInit(&second, 256, 144), "world allocation failed");
    WorldGenerate(&first, 0x51EDu);
    WorldGenerate(&second, 0x51EDu);

    CHECK(first.seed == 0x51EDu && second.seed == 0x51EDu,
          "the world did not record the seed it was generated from");
    CHECK(WorldDigest(&first) == WorldDigest(&second),
          "the same seed produced two different worlds");
    WorldUnload(&first);
    WorldUnload(&second);
}

static void test_regenerating_one_world_from_a_seed_reproduces_it(void)
{
    World world;
    uint64_t first;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    WorldGenerate(&world, 0x51EDu);
    first = WorldDigest(&world);
    WorldGenerate(&world, 0xA11CEu);
    CHECK(WorldDigest(&world) != first, "two seeds produced the same world");
    WorldGenerate(&world, 0x51EDu);
    CHECK(WorldDigest(&world) == first,
          "regenerating from the original seed did not restore the world");
    WorldUnload(&world);
}

/* Generation must not be perturbed by anything drawn later, which is why
   terrain and gameplay effects use separate streams off the same seed. */
static void test_world_effects_cannot_shift_the_terrain_a_seed_produces(void)
{
    World world;
    uint64_t clean;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    WorldGenerate(&world, 0x51EDu);
    clean = WorldDigest(&world);

    WorldGenerate(&world, 0x51EDu);
    WorldDrillCircle(&world, 128, 100, 6);
    WorldDestroyCircle(&world, 60, 100, 8, 0.5f);
    WorldGenerate(&world, 0x51EDu);
    CHECK(WorldDigest(&world) == clean,
          "drawing from the effect stream changed the terrain of a seed");
    WorldUnload(&world);
}

/* The whole point of a seeded game: a bug report is a seed plus a list of
   inputs, and a regression test can replay one exactly. */
static void test_a_seeded_session_replays_identically(void)
{
    GameConfig config = GameDefaultConfig();
    GameState first;
    GameState second;
    GameEventBuffer events = {0};
    int step;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0x1234u;
    CHECK(GameInit(&first, config), "game allocation failed");
    CHECK(GameInit(&second, config), "game allocation failed");
    CHECK(first.worldSeed == second.worldSeed,
          "the same configured seed produced two different worlds");

    for (step = 0; step < 40; ++step) {
        GameInput input = {0};

        input.move = (Vector2){1.0f, step % 7 == 0 ? -1.0f : 0.0f};
        input.boostHeld = step > 5;
        input.ability[ABILITY_LASER] = step % 5 == 0;
        input.ability[ABILITY_EXPLOSION] = step == 12;
        input.ability[ABILITY_FORCE] = step == 20;
        input.ability[ABILITY_CRYO] = step % 11 == 0;
        input.aimWorld = (Vector2){first.player.position.x + 30.0f,
                                   first.player.position.y + 8.0f};
        GameUpdate(&first, &input, config.fixedStep, &events);
        input.aimWorld = (Vector2){second.player.position.x + 30.0f,
                                   second.player.position.y + 8.0f};
        GameUpdate(&second, &input, config.fixedStep, &events);
    }

    CHECK(first.player.position.x == second.player.position.x &&
              first.player.position.y == second.player.position.y,
          "replay diverged: player at %.4f,%.4f vs %.4f,%.4f",
          first.player.position.x, first.player.position.y,
          second.player.position.x, second.player.position.y);
    CHECK(WorldDigest(&first.world) == WorldDigest(&second.world),
          "replay diverged: the two worlds no longer match");
    GameUnload(&first);
    GameUnload(&second);
}

/* The role split is enforced by the type system — visual particles see a
   `const World *` — but the invariant is worth stating in a test too, because
   a future spawner could give a visual effect the SETTLE contact mode by
   mistake and quietly start writing cells. */
static void test_visual_particles_never_change_the_world(void)
{
    World world;
    ParticleSystem particles;
    uint64_t before;
    int step;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    FillRect(&world, 0, 60, 127, 95, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    ParticlesSpawnExplosion(&particles, (Vector2){64.0f, 40.0f});
    ParticlesSpawnLaserSparks(&particles, (Vector2){64.0f, 58.0f},
                              (Vector2){0.0f, 1.0f});
    ParticlesSpawnImpact(&particles, (Vector2){64.0f, 58.0f},
                         (Vector2){0.0f, -1.0f}, 120.0f);
    ParticlesSpawnBoostTrail(&particles, (Vector2){64.0f, 30.0f},
                             (Vector2){200.0f, 0.0f}, 2);
    ParticlesSpawnBoostBurst(&particles, (Vector2){64.0f, 30.0f},
                             (Vector2){200.0f, 0.0f}, 2);
    ParticlesSpawnForceBlast(&particles, (Vector2){64.0f, 40.0f},
                             (Vector2){0.0f, 1.0f});
    ParticlesSpawnSteam(&particles, (Vector2){64.0f, 50.0f});

    before = WorldDigest(&world);
    for (step = 0; step < 120; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }
    CHECK(WorldDigest(&world) == before,
          "visual particles changed the world they were only meant to read");
    WorldUnload(&world);
}

/* --- dynamic terrain bodies --------------------------------------------- */

/* One system for the whole section, initialised per test. It owns a 1.25 MiB
   raster arena, which belongs on the heap rather than on a test's stack — the
   same reason GameState holds it rather than passing it around. */
static DynamicTerrainSystem terrain;

/* Fills a solid rectangle of one material inside a body's raster. */
static void FillBody(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                     int x0, int y0, int x1, int y1, CellMaterial material,
                     float temperature)
{
    int y;

    for (y = y0; y <= y1; ++y) {
        int x;

        for (x = x0; x <= x1; ++x) {
            DynamicTerrainSetCell(system, handle, x, y, material, temperature);
        }
    }
}

static void test_a_fresh_manager_holds_no_bodies(void)
{
    const DynamicTerrainStats *stats;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    stats = DynamicTerrainStatistics(&terrain);
    CHECK(stats->activeBodies == 0, "a fresh manager reports %d bodies",
          stats->activeBodies);
    CHECK(stats->allocatedDynamicCells == 0,
          "a fresh manager reserves %d cells", stats->allocatedDynamicCells);
    CHECK(DynamicTerrainGet(&terrain, TerrainBodyInvalidHandle()) == NULL,
          "the invalid handle resolved to a body");
    DynamicTerrainUnload(&terrain);
}

/* `TerrainBodyHandle handle = {0};` is what callers write without thinking.
   It must not name body zero: generation zero is reserved as never-live
   precisely so that the lazy initialiser fails instead of silently working
   until the first slot is reused. */
static void test_a_zero_initialised_handle_names_nothing(void)
{
    TerrainBodyHandle zeroed = {0};
    TerrainBodyHandle real;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    real = DynamicTerrainAllocBody(&terrain, 4, 4);

    CHECK(real.index == 0u, "the test needs the first allocation to take slot 0");
    CHECK(real.generation != 0u, "a live handle carries generation zero");
    CHECK(DynamicTerrainGet(&terrain, zeroed) == NULL,
          "a zero-initialised handle resolved to body zero");

    DynamicTerrainSetCell(&terrain, zeroed, 1, 1, MATERIAL_ROCK, 20.0f);
    CHECK(DynamicTerrainGetConst(&terrain, real)->cellCount == 0,
          "a zero-initialised handle wrote into body zero");
    DynamicTerrainFreeBody(&terrain, zeroed);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 1,
          "a zero-initialised handle freed body zero");
    DynamicTerrainUnload(&terrain);
}

static void test_allocation_returns_a_usable_body(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 8, 4);
    body = DynamicTerrainGet(&terrain, handle);

    CHECK(body != NULL, "allocation returned an unusable handle");
    CHECK(body->active && body->awake, "a new body is not active and awake");
    CHECK(body->width == 8 && body->height == 4, "body raster is %dx%d",
          body->width, body->height);
    CHECK(body->cellCount == 0, "a new body already holds %d cells",
          body->cellCount);
    CHECK(DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 32,
          "reserving an 8x4 raster reported %d cells",
          DynamicTerrainStatistics(&terrain)->allocatedDynamicCells);
    DynamicTerrainUnload(&terrain);
}

/* Every budget is enforced by refusing work, never by growing. */
static void test_the_manager_refuses_work_past_its_budgets(void)
{
    TerrainBodyHandle handles[MAX_TERRAIN_BODIES];
    TerrainBodyHandle overflow;
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    /* A shape that does not fit a slot is refused before any slot is used. */
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain,
                                                    TERRAIN_BODY_MAX_SPAN + 1, 1)) == NULL,
          "a body wider than the span limit was accepted");
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain, 128, 128)) == NULL,
          "a raster larger than the per-body capacity was accepted");
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain, 0, 4)) == NULL,
          "a zero-width body was accepted");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "a refused allocation still consumed a slot");

    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        handles[index] = DynamicTerrainAllocBody(&terrain, 4, 4);
        CHECK(DynamicTerrainGet(&terrain, handles[index]) != NULL,
              "slot %d of %d could not be allocated", index,
              MAX_TERRAIN_BODIES);
    }
    overflow = DynamicTerrainAllocBody(&terrain, 4, 4);
    CHECK(DynamicTerrainGet(&terrain, overflow) == NULL,
          "the manager allocated past MAX_TERRAIN_BODIES");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == MAX_TERRAIN_BODIES,
          "a refused allocation changed the active count");
    DynamicTerrainUnload(&terrain);
}

static void test_a_freed_slot_is_reused(void)
{
    TerrainBodyHandle first;
    TerrainBodyHandle second;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    first = DynamicTerrainAllocBody(&terrain, 6, 6);
    CHECK(DynamicTerrainGet(&terrain, first) != NULL, "allocation failed");

    DynamicTerrainFreeBody(&terrain, first);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "freeing a body left it counted");
    CHECK(DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 0,
          "freeing a body left its raster reserved");

    second = DynamicTerrainAllocBody(&terrain, 6, 6);
    CHECK(DynamicTerrainGet(&terrain, second) != NULL,
          "the freed slot could not be reused");
    CHECK(second.index == first.index, "reuse picked slot %u instead of %u",
          second.index, first.index);
    DynamicTerrainUnload(&terrain);
}

/* The reason handles carry a generation at all: a reference held across a free
   must not silently address whatever body lands in the slot next. */
static void test_a_stale_handle_never_resolves_to_the_new_body(void)
{
    TerrainBodyHandle stale;
    TerrainBodyHandle fresh;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    stale = DynamicTerrainAllocBody(&terrain, 4, 4);
    DynamicTerrainSetCell(&terrain, stale, 0, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFreeBody(&terrain, stale);

    CHECK(DynamicTerrainGet(&terrain, stale) == NULL,
          "a handle to a freed body still resolves");

    fresh = DynamicTerrainAllocBody(&terrain, 4, 4);
    CHECK(fresh.index == stale.index, "the test needs the slot to be reused");
    CHECK(fresh.generation != stale.generation,
          "the reused slot kept generation %u", fresh.generation);
    CHECK(DynamicTerrainGet(&terrain, stale) == NULL,
          "the stale handle resolved to the body that replaced it");

    /* Writing through a stale handle must be a no-op, not a write into the
       new body's raster. */
    DynamicTerrainSetCell(&terrain, stale, 1, 1, MATERIAL_SAND, 20.0f);
    CHECK(DynamicTerrainCellAt(&terrain, fresh, 1, 1) == MATERIAL_EMPTY,
          "a stale handle wrote into the body that replaced it");
    CHECK(DynamicTerrainGet(&terrain, fresh)->cellCount == 0,
          "a stale write changed the new body's cell count");

    /* Double free is safe and does not disturb the live body. */
    DynamicTerrainFreeBody(&terrain, stale);
    CHECK(DynamicTerrainGet(&terrain, fresh) != NULL,
          "freeing a stale handle killed the live body");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 1,
          "freeing a stale handle changed the active count");
    DynamicTerrainUnload(&terrain);
}

static void test_reset_releases_every_body(void)
{
    TerrainBodyHandle handles[4];
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    for (index = 0; index < 4; ++index) {
        handles[index] = DynamicTerrainAllocBody(&terrain, 8, 8);
        CHECK(DynamicTerrainGet(&terrain, handles[index]) != NULL,
              "allocation %d failed", index);
    }

    DynamicTerrainReset(&terrain);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "reset left %d bodies alive",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    CHECK(DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 0,
          "reset left %d cells reserved",
          DynamicTerrainStatistics(&terrain)->allocatedDynamicCells);
    for (index = 0; index < 4; ++index) {
        CHECK(DynamicTerrainGet(&terrain, handles[index]) == NULL,
              "handle %d survived the reset", index);
    }
    /* Peaks are session figures and deliberately outlive a reset. */
    CHECK(DynamicTerrainStatistics(&terrain)->peakBodies == 4,
          "reset erased the peak body count");
    CHECK(DynamicTerrainStatistics(&terrain)->peakDynamicCells == 256,
          "reset erased the peak cell count");
    DynamicTerrainUnload(&terrain);
}

/* A body must preserve enough of each cell to be drawn, collided with and put
   back: which material it is, and how hot it was. */
static void test_a_body_preserves_the_material_and_heat_it_was_given(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 5, 3);
    DynamicTerrainSetCell(&terrain, handle, 0, 0, MATERIAL_ROCK, 715.5f);
    DynamicTerrainSetCell(&terrain, handle, 4, 2, MATERIAL_ICE, -14.0f);
    DynamicTerrainSetCell(&terrain, handle, 2, 1, MATERIAL_DIRT, 20.0f);

    CHECK(DynamicTerrainCellAt(&terrain, handle, 0, 0) == MATERIAL_ROCK &&
              DynamicTerrainCellAt(&terrain, handle, 4, 2) == MATERIAL_ICE &&
              DynamicTerrainCellAt(&terrain, handle, 2, 1) == MATERIAL_DIRT,
          "the raster did not keep the materials it was given");
    CHECK(DynamicTerrainCellAt(&terrain, handle, 1, 1) == MATERIAL_EMPTY,
          "an untouched slot is not empty");
    /* Exact, not approximately: temperature is stored as the float World holds,
       so a cell sitting just under a phase threshold cannot cross it merely by
       being torn off and put back. */
    CHECK(DynamicTerrainTemperatureAt(&terrain, handle, 0, 0) == 715.5f,
          "rock came back at %.3f degrees instead of 715.5",
          (double)DynamicTerrainTemperatureAt(&terrain, handle, 0, 0));
    CHECK(DynamicTerrainTemperatureAt(&terrain, handle, 4, 2) == -14.0f,
          "ice came back at %.3f degrees instead of -14",
          (double)DynamicTerrainTemperatureAt(&terrain, handle, 4, 2));

    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->cellCount == 3, "three writes produced %d cells",
          body->cellCount);

    /* Clearing a cell gives the slot back. */
    DynamicTerrainSetCell(&terrain, handle, 2, 1, MATERIAL_EMPTY, 0.0f);
    CHECK(body->cellCount == 2, "clearing a cell left %d cells",
          body->cellCount);
    DynamicTerrainUnload(&terrain);
}

/* Reads and writes outside the raster must be refused rather than reaching a
   neighbouring body: bodies share one arena. */
static void test_out_of_range_cell_access_is_safe(void)
{
    TerrainBodyHandle first;
    TerrainBodyHandle second;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    first = DynamicTerrainAllocBody(&terrain, 4, 4);
    second = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, second, 0, 0, 3, 3, MATERIAL_SAND, 20.0f);

    DynamicTerrainSetCell(&terrain, first, 4, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(&terrain, first, 0, 4, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(&terrain, first, -1, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(&terrain, first, 999, 999, MATERIAL_ROCK, 20.0f);

    CHECK(DynamicTerrainGetConst(&terrain, first)->cellCount == 0,
          "an out-of-range write was counted");
    CHECK(DynamicTerrainGetConst(&terrain, second)->cellCount == 16,
          "an out-of-range write reached the next body");
    CHECK(DynamicTerrainCellAt(&terrain, first, 4, 0) == MATERIAL_EMPTY &&
              DynamicTerrainCellAt(&terrain, first, -1, -1) == MATERIAL_EMPTY,
          "an out-of-range read returned material");
    CHECK(DynamicTerrainTemperatureAt(&terrain, first, 99, 99) == 0.0f,
          "an out-of-range temperature read returned heat");
    DynamicTerrainUnload(&terrain);
}

static void test_finalize_reports_the_occupied_bounds(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A raster with deliberate slack around its contents. */
    handle = DynamicTerrainAllocBody(&terrain, 16, 12);
    FillBody(&terrain, handle, 3, 2, 9, 7, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);

    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->minimumX == 3 && body->maximumX == 9 && body->minimumY == 2 &&
              body->maximumY == 7,
          "bounds are %d..%d, %d..%d instead of 3..9, 2..7", body->minimumX,
          body->maximumX, body->minimumY, body->maximumY);
    CHECK(body->cellCount == 7 * 6, "finalize counted %d cells instead of 42",
          body->cellCount);

    /* An empty body reports an inverted range rather than a false extent. */
    FillBody(&terrain, handle, 3, 2, 9, 7, MATERIAL_EMPTY, 0.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    CHECK(body->cellCount == 0 && body->maximumX < body->minimumX,
          "an emptied body still claims an extent");
    CHECK(body->mass == 0.0f && body->inertia == 0.0f,
          "an emptied body kept mass %.3f and inertia %.3f", (double)body->mass,
          (double)body->inertia);
    DynamicTerrainUnload(&terrain);
}

static void test_mass_follows_material_density(void)
{
    TerrainBodyHandle rock;
    TerrainBodyHandle ice;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    rock = DynamicTerrainAllocBody(&terrain, 4, 4);
    ice = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, rock, 0, 0, 3, 3, MATERIAL_ROCK, 20.0f);
    FillBody(&terrain, ice, 0, 0, 3, 3, MATERIAL_ICE, -14.0f);
    DynamicTerrainFinalizeBody(&terrain, rock);
    DynamicTerrainFinalizeBody(&terrain, ice);

    CHECK(fabsf(DynamicTerrainGetConst(&terrain, rock)->mass -
                16.0f * MaterialAt(MATERIAL_ROCK)->density) < 0.001f,
          "sixteen rock cells weigh %.3f",
          (double)DynamicTerrainGetConst(&terrain, rock)->mass);
    /* The property that actually matters downstream: the same slab of ice must
       be lighter than the same slab of rock. */
    CHECK(DynamicTerrainGetConst(&terrain, ice)->mass <
              DynamicTerrainGetConst(&terrain, rock)->mass,
          "a slab of ice is not lighter than the same slab of rock");
    DynamicTerrainUnload(&terrain);
}

static void test_centre_of_mass_lands_where_the_shape_says(void)
{
    TerrainBodyHandle even;
    TerrainBodyHandle lopsided;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    /* A uniform 4x4 block centred on its own middle: cell centres run from 0.5
       to 3.5, so the mean is 2.0. */
    even = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, even, 0, 0, 3, 3, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, even);
    body = DynamicTerrainGetConst(&terrain, even);
    CHECK(fabsf(body->centerOfMass.x - 2.0f) < 0.001f &&
              fabsf(body->centerOfMass.y - 2.0f) < 0.001f,
          "a uniform block centres at %.3f,%.3f instead of 2,2",
          (double)body->centerOfMass.x, (double)body->centerOfMass.y);

    /* Half rock, half ice: the centre must sit toward the heavier half. */
    lopsided = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, lopsided, 0, 0, 1, 3, MATERIAL_ROCK, 20.0f);
    FillBody(&terrain, lopsided, 2, 0, 3, 3, MATERIAL_ICE, -14.0f);
    DynamicTerrainFinalizeBody(&terrain, lopsided);
    body = DynamicTerrainGetConst(&terrain, lopsided);
    CHECK(body->centerOfMass.x < 2.0f,
          "the centre of mass ignored density: x = %.3f",
          (double)body->centerOfMass.x);
    CHECK(fabsf(body->centerOfMass.y - 2.0f) < 0.001f,
          "a vertically uniform body centres at y = %.3f instead of 2",
          (double)body->centerOfMass.y);
    DynamicTerrainUnload(&terrain);
}

static void test_inertia_grows_with_how_spread_out_a_body_is(void)
{
    TerrainBodyHandle single;
    TerrainBodyHandle compact;
    TerrainBodyHandle spread;
    float compactInertia;
    float spreadInertia;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    /* A one-cell body must still resist rotation. Zero inertia would give it
       infinite angular acceleration the moment anything touched it, which is
       why each cell contributes its own moment as well as its distance. */
    single = DynamicTerrainAllocBody(&terrain, 1, 1);
    DynamicTerrainSetCell(&terrain, single, 0, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, single);
    CHECK(DynamicTerrainGetConst(&terrain, single)->inertia > 0.0f,
          "a single-cell body has zero moment of inertia");

    /* Same mass, different spread: sixteen cells packed 4x4 against sixteen
       cells strung out 16x1. */
    compact = DynamicTerrainAllocBody(&terrain, 4, 4);
    FillBody(&terrain, compact, 0, 0, 3, 3, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, compact);
    spread = DynamicTerrainAllocBody(&terrain, 16, 1);
    FillBody(&terrain, spread, 0, 0, 15, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, spread);

    compactInertia = DynamicTerrainGetConst(&terrain, compact)->inertia;
    spreadInertia = DynamicTerrainGetConst(&terrain, spread)->inertia;
    CHECK(fabsf(DynamicTerrainGetConst(&terrain, compact)->mass -
                DynamicTerrainGetConst(&terrain, spread)->mass) < 0.001f,
          "the two shapes were meant to weigh the same");
    CHECK(spreadInertia > compactInertia * 2.0f,
          "a long bar (%.2f) does not resist rotation far more than a compact "
          "block of the same mass (%.2f)", (double)spreadInertia,
          (double)compactInertia);
    DynamicTerrainUnload(&terrain);
}

/* The subsystem never receives a World, which is the strongest form of this
   guarantee; the test states the intention so a future signature change that
   hands it one has to argue with something. */
static void test_the_body_manager_leaves_the_world_alone(void)
{
    World world;
    TerrainBodyHandle handle;
    uint64_t before;
    int activeBefore;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    WorldGenerate(&world, 0xB0D1E5u);
    Tick(&world, 4);
    before = WorldDigest(&world);
    activeBefore = world.activeChunkCount;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 12, 12);
    FillBody(&terrain, handle, 0, 0, 11, 11, MATERIAL_ROCK, 400.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainFreeBody(&terrain, handle);
    DynamicTerrainReset(&terrain);
    DynamicTerrainUnload(&terrain);

    CHECK(WorldDigest(&world) == before,
          "the body manager changed the world it never received");
    CHECK(world.activeChunkCount == activeBefore,
          "the body manager woke chunks");
    WorldUnload(&world);
}

/* GameState owns the subsystem, so its lifecycle has to survive a regeneration
   without leaking bodies from the world that no longer exists. */
static void test_regenerating_the_world_drops_its_detached_pieces(void)
{
    GameConfig config = GameDefaultConfig();
    GameState game;
    TerrainBodyHandle handle;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0xB0D1E5u;
    CHECK(GameInit(&game, config), "game allocation failed");
    CHECK(DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies == 0,
          "a new game already has detached terrain");

    handle = DynamicTerrainAllocBody(&game.dynamicTerrain, 8, 8);
    CHECK(DynamicTerrainGet(&game.dynamicTerrain, handle) != NULL,
          "allocation through GameState failed");

    GameRegenerate(&game);
    CHECK(DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies == 0,
          "regenerating the world kept %d bodies cut from the old one",
          DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies);
    CHECK(DynamicTerrainGet(&game.dynamicTerrain, handle) == NULL,
          "a handle survived the world it was cut from");
    GameUnload(&game);
}

static void test_terrain_render_key_tracks_raster_edits_only(void)
{
    TerrainBodyHandle handle;
    TerrainBodyRenderKey emptyKey;
    TerrainBodyRenderKey populatedKey;
    TerrainBodyRenderKey repeatedKey;
    TerrainBodyRenderKey heatedKey;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 4, 4);
    emptyKey = TerrainBodyRenderKeyAt(&terrain, handle.index);
    CHECK(TerrainBodyRenderKeyIsLive(emptyKey),
          "a live empty body produced a dead render key");

    DynamicTerrainSetCell(&terrain, handle, 1, 1, MATERIAL_ROCK, 20.0f);
    populatedKey = TerrainBodyRenderKeyAt(&terrain, handle.index);
    CHECK(!TerrainBodyRenderKeyEquals(emptyKey, populatedKey),
          "a material edit did not invalidate the render key");

    DynamicTerrainSetCell(&terrain, handle, 1, 1, MATERIAL_ROCK, 20.0f);
    repeatedKey = TerrainBodyRenderKeyAt(&terrain, handle.index);
    CHECK(TerrainBodyRenderKeyEquals(populatedKey, repeatedKey),
          "an identical write dirtied an unchanged body texture");

    DynamicTerrainSetCell(&terrain, handle, 1, 1, MATERIAL_ROCK, 500.0f);
    heatedKey = TerrainBodyRenderKeyAt(&terrain, handle.index);
    CHECK(!TerrainBodyRenderKeyEquals(repeatedKey, heatedKey),
          "a temperature edit did not invalidate the render key");
    DynamicTerrainUnload(&terrain);
}

static void test_terrain_render_bounds_follow_the_simulation_transform(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;
    Rectangle bounds;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 2, 1);
    FillBody(&terrain, handle, 0, 0, 1, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    body = DynamicTerrainGet(&terrain, handle);
    body->position = (Vector2){10.0f, 20.0f};
    body->angle = PI * 0.5f;

    /* The two-cell bar has COM (1, 0.5). Rotated ninety degrees around that
       exact point, its 2x1 local rectangle becomes a 1x2 world rectangle. */
    bounds = TerrainBodyRenderWorldBounds(body);
    CHECK(fabsf(bounds.x - 9.5f) < 0.001f &&
              fabsf(bounds.y - 19.0f) < 0.001f &&
              fabsf(bounds.width - 1.0f) < 0.001f &&
              fabsf(bounds.height - 2.0f) < 0.001f,
          "rotated render bounds are %.3f,%.3f %.3fx%.3f",
          (double)bounds.x, (double)bounds.y, (double)bounds.width,
          (double)bounds.height);
    CHECK(TerrainBodyRenderIntersects(
              body, (Rectangle){9.0f, 19.5f, 2.0f, 1.0f}),
          "visible rotated body was culled");
    CHECK(!TerrainBodyRenderIntersects(
              body, (Rectangle){30.0f, 30.0f, 4.0f, 4.0f}),
          "offscreen rotated body survived culling");
    DynamicTerrainUnload(&terrain);
}

static void test_terrain_render_key_rejects_free_reset_and_slot_reuse(void)
{
    TerrainBodyHandle stale;
    TerrainBodyHandle fresh;
    TerrainBodyRenderKey staleKey;
    TerrainBodyRenderKey freshKey;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    stale = DynamicTerrainAllocBody(&terrain, 3, 3);
    DynamicTerrainSetCell(&terrain, stale, 1, 1, MATERIAL_DIRT, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, stale);
    staleKey = TerrainBodyRenderKeyAt(&terrain, stale.index);

    DynamicTerrainFreeBody(&terrain, stale);
    CHECK(!TerrainBodyRenderKeyIsLive(
              TerrainBodyRenderKeyAt(&terrain, stale.index)),
          "a freed body kept a live renderer cache key");

    fresh = DynamicTerrainAllocBody(&terrain, 3, 3);
    CHECK(fresh.index == stale.index,
          "the cache reuse test did not reuse its simulation slot");
    DynamicTerrainSetCell(&terrain, fresh, 1, 1, MATERIAL_ICE, -14.0f);
    DynamicTerrainFinalizeBody(&terrain, fresh);
    freshKey = TerrainBodyRenderKeyAt(&terrain, fresh.index);
    CHECK(!TerrainBodyRenderKeyEquals(staleKey, freshKey),
          "a reused simulation slot matched its stale texture identity");

    DynamicTerrainReset(&terrain);
    CHECK(!TerrainBodyRenderKeyIsLive(
              TerrainBodyRenderKeyAt(&terrain, fresh.index)),
          "reset left a renderer cache identity live");
    DynamicTerrainUnload(&terrain);
}

static void test_terrain_render_data_handles_empty_and_maximum_rasters(void)
{
    TerrainBodyHandle empty;
    TerrainBodyHandle maximum;
    const TerrainBody *body;
    TerrainBodyRenderKey key;
    Rectangle bounds;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    empty = DynamicTerrainAllocBody(&terrain, 8, 8);
    DynamicTerrainFinalizeBody(&terrain, empty);
    body = DynamicTerrainGetConst(&terrain, empty);
    bounds = TerrainBodyRenderWorldBounds(body);
    CHECK(!TerrainBodyRenderIsDrawable(body) && bounds.width == 0.0f &&
              bounds.height == 0.0f,
          "an empty body produced drawable render bounds");
    CHECK(!TerrainBodyRenderIntersects(
              body, (Rectangle){-10.0f, -10.0f, 20.0f, 20.0f}),
          "an empty body passed camera culling");

    maximum = DynamicTerrainAllocBody(&terrain, TERRAIN_BODY_MAX_SPAN,
                                      TERRAIN_BODY_RASTER_CAPACITY /
                                          TERRAIN_BODY_MAX_SPAN);
    CHECK(DynamicTerrainGet(&terrain, maximum) != NULL,
          "the largest legal raster was refused");
    DynamicTerrainSetCell(&terrain, maximum, 0, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainSetCell(
        &terrain, maximum, TERRAIN_BODY_MAX_SPAN - 1,
        TERRAIN_BODY_RASTER_CAPACITY / TERRAIN_BODY_MAX_SPAN - 1,
        MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, maximum);
    body = DynamicTerrainGetConst(&terrain, maximum);
    key = TerrainBodyRenderKeyAt(&terrain, maximum.index);
    bounds = TerrainBodyRenderWorldBounds(body);
    CHECK(TerrainBodyRenderIsDrawable(body) &&
              key.width * key.height == TERRAIN_BODY_RASTER_CAPACITY,
          "the maximum raster is not safely drawable");
    CHECK(fabsf(bounds.width - (float)key.width) < 0.001f &&
              fabsf(bounds.height - (float)key.height) < 0.001f,
          "maximum raster bounds are %.1fx%.1f instead of %dx%d",
          (double)bounds.width, (double)bounds.height,
          key.width, key.height);
    DynamicTerrainUnload(&terrain);
}

/* --- connected solid components ----------------------------------------- */

/* One workspace for the whole section. It is about 34 KiB, which belongs in a
   long-lived owner rather than on the stack of every test — and that is exactly
   how the future dynamic terrain system will hold it. */
static WorldComponentWorkspace componentWorkspace;

static const char *ComponentStatusName(WorldComponentStatus status)
{
    switch (status) {
    case WORLD_COMPONENT_DETACHED: return "DETACHED";
    case WORLD_COMPONENT_ANCHORED: return "ANCHORED";
    case WORLD_COMPONENT_UNKNOWN: return "UNKNOWN";
    case WORLD_COMPONENT_TOO_LARGE: return "TOO_LARGE";
    default: return "INVALID";
    }
}

static WorldComponentResult FindComponent(const World *world, Rectangle region,
                                          int seedX, int seedY)
{
    return WorldFindComponent(world, &componentWorkspace, region, seedX, seedY,
                              WORLD_COMPONENT_MAX_CELLS);
}

/* Every fixture below uses a world large enough to hold three chunks in each
   direction, so a component can be placed across a chunk border on purpose. */
static Rectangle WholeWorld(const World *world)
{
    return (Rectangle){0.0f, 0.0f, (float)world->width, (float)world->height};
}

static void test_a_lone_island_is_reported_detached(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 40, 40, 44, 44, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 42, 42);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "a lone island reported %s", ComponentStatusName(found.status));
    CHECK(found.cellCount == 25, "island has %d cells, expected 25",
          found.cellCount);
    CHECK(found.minimumX == 40 && found.maximumX == 44 &&
              found.minimumY == 40 && found.maximumY == 44,
          "island bounds are %d..%d, %d..%d", found.minimumX, found.maximumX,
          found.minimumY, found.maximumY);
    WorldUnload(&world);
}

static void test_two_islands_are_separate_components(void)
{
    World world;
    WorldComponentResult first;
    WorldComponentResult second;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 10, 10, 14, 14, MATERIAL_ROCK);
    FillRect(&world, 60, 60, 64, 64, MATERIAL_ROCK);

    first = FindComponent(&world, WholeWorld(&world), 12, 12);
    second = FindComponent(&world, WholeWorld(&world), 62, 62);

    CHECK(first.status == WORLD_COMPONENT_DETACHED &&
              second.status == WORLD_COMPONENT_DETACHED,
          "islands reported %s and %s", ComponentStatusName(first.status),
          ComponentStatusName(second.status));
    CHECK(first.cellCount == 25 && second.cellCount == 25,
          "islands have %d and %d cells, expected 25 each", first.cellCount,
          second.cellCount);
    /* The point of the test: neither search wandered into the other island. */
    CHECK(first.maximumX == 14 && second.minimumX == 60,
          "the two islands were merged into one component");
    WorldUnload(&world);
}

/* Water is not solid, so it cannot hold two blocks of rock together. This pins
   the membership rule down: a component is made of solid cells, not of
   non-empty ones. */
static void test_a_liquid_gap_does_not_join_two_components(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 20, 40, 24, 44, MATERIAL_ROCK);
    FillRect(&world, 25, 40, 29, 44, MATERIAL_WATER);
    FillRect(&world, 30, 40, 34, 44, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 22, 42);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "the left block reported %s", ComponentStatusName(found.status));
    CHECK(found.cellCount == 25,
          "water joined the two blocks: %d cells instead of 25",
          found.cellCount);
    WorldUnload(&world);
}

/* The floor of these fixtures spans the whole world, so it touches the map
   border — which the simulation already treats as immovable rock. */
static void BuildIslandOnABridge(World *world)
{
    FillRect(world, 0, 80, world->width - 1, world->height - 1, MATERIAL_ROCK);
    FillRect(world, 30, 20, 40, 30, MATERIAL_ROCK);
    FillRect(world, 35, 31, 35, 79, MATERIAL_ROCK);
}

static void test_an_intact_bridge_to_the_ground_prevents_detachment(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    BuildIslandOnABridge(&world);

    found = FindComponent(&world, WholeWorld(&world), 35, 25);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "an island still bridged to the ground reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

static void test_cutting_the_bridge_detaches_the_island(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    BuildIslandOnABridge(&world);
    FillRect(&world, 35, 50, 35, 52, MATERIAL_EMPTY);

    found = FindComponent(&world, WholeWorld(&world), 35, 25);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "an island cut free reported %s", ComponentStatusName(found.status));
    /* 11x11 island plus the nineteen cells of bridge still hanging from it. */
    CHECK(found.cellCount == 121 + 19, "the freed piece has %d cells, expected 140",
          found.cellCount);
    CHECK(found.minimumY == 20 && found.maximumY == 49,
          "the freed piece spans rows %d..%d, expected 20..49", found.minimumY,
          found.maximumY);

    /* The stump below the cut is still part of the ground. */
    found = FindComponent(&world, WholeWorld(&world), 35, 60);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "the stump left standing on the floor reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

/* Connectivity is four-neighbour, and that is a decision rather than an
   accident, so it is pinned here in both directions. Two blocks meeting at a
   corner are two components... */
static void test_a_corner_contact_does_not_join_two_components(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 40, 40, 44, 44, MATERIAL_ROCK);
    FillRect(&world, 45, 45, 49, 49, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 42, 42);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "the upper block reported %s", ComponentStatusName(found.status));
    CHECK(found.cellCount == 25,
          "a corner contact merged two blocks: %d cells instead of 25",
          found.cellCount);
    WorldUnload(&world);
}

/* ...and the price of that, stated openly: a piece joined to the ground by a
   single diagonal staircase reads as free. Such a join is one cell thick and
   would not hold anything up, but a caller that wants a minimum thickness
   before tearing terrain off has to impose it itself. Changing connectivity
   later should mean deliberately editing this test, not discovering it. */
static void test_a_diagonal_only_join_to_the_ground_reads_as_detached(void)
{
    World world;
    WorldComponentResult found;
    int step;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 0, 80, world.width - 1, world.height - 1, MATERIAL_ROCK);
    FillRect(&world, 30, 20, 40, 30, MATERIAL_ROCK);
    /* A staircase from the island's bottom-right corner down to the floor.
       Every cell touches the next only at a corner, the first touches the
       island only at a corner, and the hole punched under the last one leaves
       it touching the floor only at a corner too. */
    for (step = 0; step < 49; ++step) {
        WorldSetCell(&world, 41 + step, 31 + step, MATERIAL_ROCK);
    }
    WorldSetCell(&world, 89, 80, MATERIAL_EMPTY);

    found = FindComponent(&world, WholeWorld(&world), 35, 25);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "a diagonally joined island reported %s",
          ComponentStatusName(found.status));
    CHECK(found.cellCount == 121,
          "the staircase was counted as part of the island: %d cells",
          found.cellCount);
    WorldUnload(&world);
}

/* Nothing in the detector is chunk-aware, and this test exists to keep it that
   way: a future chunk-local optimisation must not quietly cut components at a
   chunk border. */
static void test_a_component_crossing_a_chunk_boundary_stays_whole(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    CHECK(28 / WORLD_CHUNK_SIZE != 36 / WORLD_CHUNK_SIZE,
          "the fixture no longer straddles a chunk border");
    FillRect(&world, 28, 28, 36, 36, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 28, 28);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "a component across a chunk border reported %s",
          ComponentStatusName(found.status));
    CHECK(found.cellCount == 81,
          "a component across a chunk border has %d cells, expected 81",
          found.cellCount);
    CHECK(found.minimumX == 28 && found.maximumX == 36 &&
              found.minimumY == 28 && found.maximumY == 36,
          "bounds %d..%d, %d..%d were cut at a chunk border", found.minimumX,
          found.maximumX, found.minimumY, found.maximumY);
    WorldUnload(&world);
}

/* Reaching the edge of the query region is not by itself a reason to give up:
   the detector peeks one cell past it to learn whether the component actually
   continues. It gives up only when something solid is really there. */
static void test_a_component_continuing_past_the_region_is_unknown(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 10, 40, 60, 40, MATERIAL_ROCK);

    found = FindComponent(&world, (Rectangle){20.0f, 30.0f, 21.0f, 21.0f}, 30, 40);
    CHECK(found.status == WORLD_COMPONENT_UNKNOWN,
          "a bar running out of the region reported %s",
          ComponentStatusName(found.status));
    /* A failed search reports nothing rather than however much it explored: a
       partial component is not a smaller component, and handing one back
       invites a caller to act on it. */
    CHECK(found.cellCount == 0 && found.minimumX == 0 && found.maximumX == 0,
          "an unknown result carried %d cells and bounds %d..%d",
          found.cellCount, found.minimumX, found.maximumX);
    WorldUnload(&world);
}

static void test_a_component_that_only_touches_the_region_edge_is_detached(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 22, 60, 26, 64, MATERIAL_ROCK);

    /* A region exactly the size of the island: every cell of it sits on the
       region edge, and every neighbour outside is empty. */
    found = FindComponent(&world, (Rectangle){22.0f, 60.0f, 5.0f, 5.0f}, 24, 62);
    CHECK(found.status == WORLD_COMPONENT_DETACHED,
          "an island filling its region reported %s",
          ComponentStatusName(found.status));
    CHECK(found.cellCount == 25, "island has %d cells, expected 25",
          found.cellCount);
    WorldUnload(&world);
}

static void test_a_component_touching_the_world_edge_is_anchored(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 0, 40, 4, 44, MATERIAL_ROCK);

    found = FindComponent(&world, WholeWorld(&world), 2, 42);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "a component against the map border reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

static void test_a_component_larger_than_the_budget_is_too_large(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 96, 96), "world allocation failed");
    FillRect(&world, 10, 10, 29, 29, MATERIAL_ROCK);

    found = WorldFindComponent(&world, &componentWorkspace, WholeWorld(&world),
                               20, 20, 50);
    CHECK(found.status == WORLD_COMPONENT_TOO_LARGE,
          "a 400-cell block under a 50-cell budget reported %s",
          ComponentStatusName(found.status));

    /* The same block fits comfortably in the full workspace. */
    found = FindComponent(&world, WholeWorld(&world), 20, 20);
    CHECK(found.status == WORLD_COMPONENT_DETACHED && found.cellCount == 400,
          "the same block reported %s with %d cells",
          ComponentStatusName(found.status), found.cellCount);
    WorldUnload(&world);
}

/* A region wider than the visited bitmap can index is refused outright.
   Clipping it instead would silently answer a question the caller did not
   ask. */
static void test_an_oversized_or_malformed_query_is_refused(void)
{
    World world;
    WorldComponentResult found;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    FillRect(&world, 40, 40, 44, 44, MATERIAL_ROCK);

    found = FindComponent(&world, (Rectangle){0.0f, 0.0f, 200.0f, 100.0f}, 42, 42);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "a %d-wide region reported %s instead of being refused",
          WORLD_COMPONENT_MAX_SPAN + 1, ComponentStatusName(found.status));

    /* The span is judged on what the caller asked for, before the world clips
       it. Otherwise the same oversized region would be refused in open ground
       and accepted at the border, purely because the map trimmed it. */
    found = FindComponent(&world, (Rectangle){-160.0f, 0.0f, 200.0f, 100.0f},
                          42, 42);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "an oversized region was accepted at the map border, reporting %s",
          ComponentStatusName(found.status));

    /* A legal region that merely hangs over the edge is still fine. */
    FillRect(&world, 0, 40, 4, 44, MATERIAL_ROCK);
    found = FindComponent(&world, (Rectangle){-40.0f, 20.0f, 100.0f, 60.0f}, 2, 42);
    CHECK(found.status == WORLD_COMPONENT_ANCHORED,
          "a legal region overhanging the border reported %s",
          ComponentStatusName(found.status));

    found = FindComponent(&world, (Rectangle){20.0f, 20.0f, 64.0f, 64.0f}, 10, 10);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "a seed outside the region reported %s",
          ComponentStatusName(found.status));

    found = FindComponent(&world, (Rectangle){20.0f, 20.0f, 64.0f, 64.0f}, 60, 60);
    CHECK(found.status == WORLD_COMPONENT_INVALID,
          "a seed on an empty cell reported %s",
          ComponentStatusName(found.status));
    WorldUnload(&world);
}

/* The detector is a query, not a step of the simulation. It must leave no trace
   at all — not a material, not a temperature, and not a woken chunk. */
static void test_the_detector_never_changes_the_world(void)
{
    World world;
    uint64_t before;
    int activeBefore;
    uint32_t tickBefore;

    CHECK(WorldInit(&world, 256, 144), "world allocation failed");
    WorldGenerate(&world, 0xC0FFEEu);
    FillRect(&world, 60, 40, 70, 50, MATERIAL_ROCK);
    FillRect(&world, 100, 40, 160, 40, MATERIAL_ROCK);
    Tick(&world, 4);

    before = WorldDigest(&world);
    activeBefore = world.activeChunkCount;
    tickBefore = world.tick;

    /* One query of each outcome the detector can produce. */
    (void)FindComponent(&world, (Rectangle){40.0f, 20.0f, 64.0f, 64.0f}, 65, 45);
    (void)FindComponent(&world, (Rectangle){100.0f, 20.0f, 32.0f, 64.0f}, 110, 40);
    (void)FindComponent(&world, (Rectangle){0.0f, 100.0f, 100.0f, 44.0f}, 10, 130);
    (void)WorldFindComponent(&world, &componentWorkspace,
                             (Rectangle){40.0f, 20.0f, 64.0f, 64.0f}, 65, 45, 4);
    (void)FindComponent(&world, (Rectangle){0.0f, 0.0f, 200.0f, 100.0f}, 65, 45);

    CHECK(WorldDigest(&world) == before,
          "the detector changed materials or temperatures");
    CHECK(world.activeChunkCount == activeBefore,
          "the detector woke chunks: %d active, expected %d",
          world.activeChunkCount, activeBefore);
    CHECK(world.tick == tickBefore, "the detector advanced the tick counter");
    WorldUnload(&world);
}

/* --- world to body extraction ------------------------------------------- */

/* Extraction is the first thing that joins the detector to the body store, and
   its one hard promise is atomicity: either it completes, or the world is
   exactly as it was. Most of these tests are therefore failure tests that
   compare a digest across the attempt. */

static TerrainExtractResult ExtractAt(World *world, DynamicTerrainSystem *system,
                                      Rectangle region, int seedX, int seedY)
{
    WorldComponentResult component =
        WorldFindComponent(world, &componentWorkspace, region, seedX, seedY,
                           WORLD_COMPONENT_MAX_CELLS);

    return TerrainExtractComponent(world, system, &componentWorkspace, component);
}

/* An island of two materials at two temperatures, so a test can tell whether
   the body kept what it was given rather than merely the right number of
   cells. */
static void BuildExtractableIsland(World *world)
{
    FillRect(world, 40, 30, 47, 35, MATERIAL_ROCK);
    FillRect(world, 44, 30, 47, 32, MATERIAL_ICE);
    WorldSetTemperature(world, 41, 34, 640.0f);
    WorldSetTemperature(world, 45, 31, -14.0f);
}

static void test_extraction_moves_an_island_out_of_the_world(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;
    int rockBefore;
    int iceBefore;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    BuildExtractableIsland(&world);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);
    iceBefore = CountMaterial(&world, MATERIAL_ICE);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);
    CHECK(body != NULL, "extraction returned an unusable handle");

    /* The static world no longer holds any of it. */
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 0 &&
              CountMaterial(&world, MATERIAL_ICE) == 0,
          "extraction left %d rock and %d ice cells behind",
          CountMaterial(&world, MATERIAL_ROCK),
          CountMaterial(&world, MATERIAL_ICE));

    /* And the body holds all of it: nothing was lost in transit. */
    CHECK(body->cellCount == rockBefore + iceBefore,
          "body holds %d cells, the island had %d", body->cellCount,
          rockBefore + iceBefore);
    CHECK(extracted.cellCount == body->cellCount,
          "the result claims %d cells but the body has %d", extracted.cellCount,
          body->cellCount);
    CHECK(DynamicTerrainStatistics(&terrain)->extractionsSucceeded == 1 &&
              DynamicTerrainStatistics(&terrain)->extractionsFailed == 0,
          "counters read %d succeeded / %d failed",
          DynamicTerrainStatistics(&terrain)->extractionsSucceeded,
          DynamicTerrainStatistics(&terrain)->extractionsFailed);
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_extraction_conserves_materials_and_heat(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;
    int rock = 0;
    int ice = 0;
    int localX;
    int localY;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    BuildExtractableIsland(&world);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);

    for (localY = 0; localY < body->height; ++localY) {
        for (localX = 0; localX < body->width; ++localX) {
            CellMaterial material =
                DynamicTerrainCellAt(&terrain, extracted.body, localX, localY);

            if (material == MATERIAL_ROCK) ++rock;
            if (material == MATERIAL_ICE) ++ice;
        }
    }
    /* The island is 8x6 rock with a 4x3 corner replaced by ice. */
    CHECK(rock == 8 * 6 - 4 * 3 && ice == 4 * 3,
          "body holds %d rock and %d ice, expected %d and %d", rock, ice,
          8 * 6 - 4 * 3, 4 * 3);

    /* Temperature travels as the float the world held, exactly. */
    CHECK(DynamicTerrainTemperatureAt(&terrain, extracted.body, 1, 4) == 640.0f,
          "the hot cell arrived at %.3f instead of 640",
          (double)DynamicTerrainTemperatureAt(&terrain, extracted.body, 1, 4));
    CHECK(DynamicTerrainTemperatureAt(&terrain, extracted.body, 5, 1) == -14.0f,
          "the frozen cell arrived at %.3f instead of -14",
          (double)DynamicTerrainTemperatureAt(&terrain, extracted.body, 5, 1));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Local (0,0) is the component's bounding-box corner and the body's origin is
   its centre of mass, so the documented transform has to round-trip. */
static void test_extraction_places_the_body_where_the_island_was(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A uniform 8x6 block, whose centre of mass is its middle. */
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);

    CHECK(body->sourceX == 40 && body->sourceY == 30,
          "the body came from %d,%d instead of 40,30", body->sourceX,
          body->sourceY);
    CHECK(body->width == 8 && body->height == 6, "raster is %dx%d, expected 8x6",
          body->width, body->height);
    CHECK(body->minimumX == 0 && body->maximumX == 7 && body->minimumY == 0 &&
              body->maximumY == 5,
          "local bounds are %d..%d, %d..%d", body->minimumX, body->maximumX,
          body->minimumY, body->maximumY);
    CHECK(fabsf(body->centerOfMass.x - 4.0f) < 0.001f &&
              fabsf(body->centerOfMass.y - 3.0f) < 0.001f,
          "local centre of mass is %.3f,%.3f instead of 4,3",
          (double)body->centerOfMass.x, (double)body->centerOfMass.y);
    /* The world position of that centre: 40 + 4, 30 + 3. */
    CHECK(fabsf(body->position.x - 44.0f) < 0.001f &&
              fabsf(body->position.y - 33.0f) < 0.001f,
          "the body sits at %.3f,%.3f instead of 44,33", (double)body->position.x,
          (double)body->position.y);
    CHECK(body->angle == 0.0f && body->velocity.x == 0.0f &&
              body->velocity.y == 0.0f && body->angularVelocity == 0.0f,
          "a freshly extracted body is already moving");
    CHECK(body->mass > 0.0f && body->inertia > 0.0f,
          "mass %.3f and inertia %.3f were not finalised", (double)body->mass,
          (double)body->inertia);
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Clearing goes through the ordinary world write path, so the chunks that held
   the island must be scheduled and marked for a pixel rebuild — otherwise the
   island would still be on screen after being torn out. */
static void test_extraction_wakes_and_dirties_the_chunks_it_emptied(void)
{
    World world;
    TerrainExtractResult extracted;
    size_t chunkIndex;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);

    /* Settle first, then clear the flags, so what the extraction sets is the
       only thing left to see. */
    Tick(&world, 3);
    chunkIndex = (size_t)(30 / WORLD_CHUNK_SIZE) * (size_t)world.chunkColumns +
                 (size_t)(40 / WORLD_CHUNK_SIZE);
    memset(world.dirtyChunks, 0,
           (size_t)world.chunkColumns * (size_t)world.chunkRows);
    memset(world.lightDirtyChunks, 0,
           (size_t)world.chunkColumns * (size_t)world.chunkRows);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    CHECK(world.dirtyChunks[chunkIndex] != 0u,
          "the emptied chunk was not marked for a pixel rebuild");
    CHECK(world.lightDirtyChunks[chunkIndex] != 0u,
          "the emptied chunk did not have its light inputs invalidated");
    CHECK(world.activeChunks[chunkIndex] != 0u,
          "the emptied chunk was not scheduled for simulation");
    CHECK(world.activeChunkCount > 0, "extraction scheduled nothing at all");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_extraction_crosses_a_chunk_boundary(void)
{
    World world;
    TerrainExtractResult extracted;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    CHECK(28 / WORLD_CHUNK_SIZE != 40 / WORLD_CHUNK_SIZE,
          "the fixture no longer straddles a chunk border");
    FillRect(&world, 28, 28, 40, 40, MATERIAL_DIRT);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 30, 30);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));
    body = DynamicTerrainGetConst(&terrain, extracted.body);
    CHECK(body->cellCount == 13 * 13,
          "a body across a chunk border holds %d of 169 cells", body->cellCount);
    CHECK(CountMaterial(&world, MATERIAL_DIRT) == 0,
          "%d dirt cells survived on the far side of the chunk border",
          CountMaterial(&world, MATERIAL_DIRT));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Every refusal must leave the world byte-for-byte as it was, and must not
   strand a half-filled body in the store. */
static void CheckExtractionRefused(World *world, DynamicTerrainSystem *system,
                                   TerrainExtractResult extracted,
                                   TerrainExtractStatus expected,
                                   uint64_t digestBefore, int bodiesBefore,
                                   const char *what)
{
    CHECK(extracted.status == expected, "%s reported %s instead of %s", what,
          TerrainExtractStatusName(extracted.status),
          TerrainExtractStatusName(expected));
    CHECK(WorldDigest(world) == digestBefore, "%s changed the world", what);
    CHECK(DynamicTerrainGet(system, extracted.body) == NULL,
          "%s handed back a live body", what);
    CHECK(DynamicTerrainStatistics(system)->activeBodies == bodiesBefore,
          "%s left %d bodies allocated instead of %d", what,
          DynamicTerrainStatistics(system)->activeBodies, bodiesBefore);
}

static void test_an_anchored_component_is_never_extracted(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A floor spanning the map touches the border, so it is anchored. */
    FillRect(&world, 0, 80, 127, 95, MATERIAL_ROCK);
    before = WorldDigest(&world);

    CheckExtractionRefused(&world, &terrain,
                           ExtractAt(&world, &terrain, WholeWorld(&world), 60, 88),
                           TERRAIN_EXTRACT_NOT_DETACHED, before, 0,
                           "extracting anchored ground");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_an_unknown_component_is_never_extracted(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A bar that runs out of the query region: the detector cannot prove it
       free, so extraction must not guess. */
    FillRect(&world, 10, 40, 100, 40, MATERIAL_ROCK);
    before = WorldDigest(&world);

    CheckExtractionRefused(
        &world, &terrain,
        ExtractAt(&world, &terrain, (Rectangle){20.0f, 30.0f, 21.0f, 21.0f}, 30, 40),
        TERRAIN_EXTRACT_NOT_DETACHED, before, 0,
        "extracting an unknown component");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_a_component_the_detector_refused_is_never_extracted(void)
{
    World world;
    WorldComponentResult component;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 30, 30, 60, 60, MATERIAL_ROCK);
    before = WorldDigest(&world);

    /* A budget too small for the block: the detector reports TOO_LARGE, and
       that is not a licence to tear out whatever it managed to walk. */
    component = WorldFindComponent(&world, &componentWorkspace,
                                   WholeWorld(&world), 40, 40, 64);
    CHECK(component.status == WORLD_COMPONENT_TOO_LARGE,
          "the fixture no longer produces TOO_LARGE");
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_NOT_DETACHED, before, 0,
        "extracting a truncated component");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

static void test_extraction_without_a_free_body_slot_changes_nothing(void)
{
    World world;
    uint64_t before;
    int index;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    before = WorldDigest(&world);

    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        CHECK(DynamicTerrainGet(&terrain,
                                DynamicTerrainAllocBody(&terrain, 2, 2)) != NULL,
              "could not fill slot %d", index);
    }

    CheckExtractionRefused(&world, &terrain,
                           ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34),
                           TERRAIN_EXTRACT_NO_BODY_SLOT, before,
                           MAX_TERRAIN_BODIES, "extracting with a full store");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 8 * 6,
          "a refused extraction removed rock from the world");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* A ring: few enough cells to pass the detector, but a bounding box larger than
   any body's raster. The store must say so rather than tear out what fits. */
static void test_a_component_too_wide_for_a_body_changes_nothing(void)
{
    World world;
    WorldComponentResult component;
    Rectangle region = {30.0f, 20.0f, 128.0f, 90.0f};
    uint64_t before;

    CHECK(WorldInit(&world, 200, 140), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 30, 20, 157, 20, MATERIAL_ROCK);
    FillRect(&world, 30, 109, 157, 109, MATERIAL_ROCK);
    FillRect(&world, 30, 20, 30, 109, MATERIAL_ROCK);
    FillRect(&world, 157, 20, 157, 109, MATERIAL_ROCK);
    before = WorldDigest(&world);

    component = WorldFindComponent(&world, &componentWorkspace, region, 60, 20,
                                   WORLD_COMPONENT_MAX_CELLS);
    CHECK(component.status == WORLD_COMPONENT_DETACHED,
          "the ring fixture reported %s", ComponentStatusName(component.status));
    CHECK((component.maximumX - component.minimumX + 1) *
                  (component.maximumY - component.minimumY + 1) >
              TERRAIN_BODY_RASTER_CAPACITY,
          "the ring no longer exceeds a body raster");
    CHECK(component.cellCount < MAX_TERRAIN_BODY_CELLS,
          "the ring was meant to fail on its bounding box, not its cell count");

    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_CELL_CAPACITY, before, 0, "extracting an oversized ring");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* A fabricated result is the one input the detector cannot vouch for, so the
   preflight has to stand on its own. */
static void test_a_malformed_component_changes_nothing(void)
{
    World world;
    WorldComponentResult component;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    before = WorldDigest(&world);

    component = (WorldComponentResult){WORLD_COMPONENT_DETACHED, 0, 0, 0, 0, 0, 0};
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting an empty component");

    component = (WorldComponentResult){WORLD_COMPONENT_DETACHED, 4, 40, 30, 39, 35, 4};
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting inverted bounds");

    component = (WorldComponentResult){WORLD_COMPONENT_DETACHED, 4, 40, 30, 200, 35, 4};
    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting bounds outside the world");

    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(NULL, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_INVALID, before, 0, "extracting from no world");
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* The detector runs at one moment and extraction at another. If the world moved
   on in between, the recorded cells are stale and copying them would put a hole
   inside a body of rock. */
static void test_a_component_the_world_has_moved_past_changes_nothing(void)
{
    World world;
    WorldComponentResult component;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);

    component = WorldFindComponent(&world, &componentWorkspace,
                                   WholeWorld(&world), 41, 34,
                                   WORLD_COMPONENT_MAX_CELLS);
    CHECK(component.status == WORLD_COMPONENT_DETACHED, "the fixture reported %s",
          ComponentStatusName(component.status));

    /* Something else eats one of its cells between the search and the commit. */
    WorldSetCell(&world, 43, 32, MATERIAL_EMPTY);
    before = WorldDigest(&world);

    CheckExtractionRefused(
        &world, &terrain,
        TerrainExtractComponent(&world, &terrain, &componentWorkspace, component),
        TERRAIN_EXTRACT_WORLD_CHANGED, before, 0,
        "extracting a component the world moved past");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 8 * 6 - 1,
          "the stale extraction removed rock anyway: %d cells left",
          CountMaterial(&world, MATERIAL_ROCK));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* Extraction, then a reset: the bodies go with the world they were cut from,
   and the store is reusable afterwards. */
static void test_reset_after_extraction_returns_the_store_to_empty(void)
{
    World world;
    TerrainExtractResult extracted;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));

    DynamicTerrainReset(&terrain);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "reset left %d bodies after an extraction",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    CHECK(DynamicTerrainGet(&terrain, extracted.body) == NULL,
          "an extracted body's handle survived the reset");
    /* Outcome counters are session figures and outlive a reset. */
    CHECK(DynamicTerrainStatistics(&terrain)->extractionsSucceeded == 1,
          "reset erased the extraction counter");

    /* And the store still works. */
    FillRect(&world, 60, 30, 67, 35, MATERIAL_ROCK);
    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 61, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK,
          "extraction after a reset reported %s",
          TerrainExtractStatusName(extracted.status));
    DynamicTerrainUnload(&terrain);
    WorldUnload(&world);
}

/* --- terrain body kinematics -------------------------------------------- */

#define KINEMATIC_STEP (1.0f / 60.0f)

/* A solid block, finalised, so mass, centre of mass and inertia are real. */
static TerrainBodyHandle MakeKinematicBody(DynamicTerrainSystem *system, int width,
                                           int height, Vector2 position)
{
    TerrainBodyHandle handle = DynamicTerrainAllocBody(system, width, height);
    TerrainBody *body;

    FillBody(system, handle, 0, 0, width - 1, height - 1, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(system, handle);
    body = DynamicTerrainGet(system, handle);
    if (body != NULL) {
        body->position = position;
    }
    return handle;
}

/* Most of these tests want to watch one effect at a time. */
static DynamicTerrainConfig QuietConfig(void)
{
    DynamicTerrainConfig config = DynamicTerrainDefaultConfig();

    config.gravity = 0.0f;
    config.linearDamping = 0.0f;
    config.angularDamping = 0.0f;
    return config;
}

static void TickBodies(DynamicTerrainSystem *system, int count)
{
    int step;

    for (step = 0; step < count; ++step) {
        TerrainPhysicsUpdate(system, NULL, KINEMATIC_STEP);
    }
}

static void test_a_moving_body_travels_at_its_velocity(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){100.0f, 50.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){60.0f, -30.0f}, 0.0f);

    TickBodies(&terrain, 60);
    body = DynamicTerrainGetConst(&terrain, handle);
    /* Sixty steps of 1/60 s at 60 and -30 cells per second. */
    CHECK(fabsf(body->position.x - 160.0f) < 0.01f &&
              fabsf(body->position.y - 20.0f) < 0.01f,
          "body ended at %.3f,%.3f instead of 160,20", (double)body->position.x,
          (double)body->position.y);
    DynamicTerrainUnload(&terrain);
}

/* Gravity is an acceleration, so it must not care what a body weighs. */
static void test_gravity_accelerates_every_body_equally(void)
{
    TerrainBodyHandle light;
    TerrainBodyHandle heavy;
    DynamicTerrainConfig config = QuietConfig();
    const TerrainBody *lightBody;
    const TerrainBody *heavyBody;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    config.gravity = 120.0f;
    terrain.config = config;

    light = DynamicTerrainAllocBody(&terrain, 1, 1);
    DynamicTerrainSetCell(&terrain, light, 0, 0, MATERIAL_ICE, -14.0f);
    DynamicTerrainFinalizeBody(&terrain, light);
    heavy = MakeKinematicBody(&terrain, 8, 8, (Vector2){0.0f, 0.0f});
    DynamicTerrainGet(&terrain, light)->position = (Vector2){0.0f, 0.0f};

    lightBody = DynamicTerrainGetConst(&terrain, light);
    heavyBody = DynamicTerrainGetConst(&terrain, heavy);
    CHECK(heavyBody->mass > lightBody->mass * 10.0f,
          "the fixture needs a real mass difference: %.3f vs %.3f",
          (double)heavyBody->mass, (double)lightBody->mass);

    TickBodies(&terrain, 60);
    CHECK(fabsf(lightBody->velocity.y - 120.0f) < 0.01f,
          "after a second of gravity the light body falls at %.3f, not 120",
          (double)lightBody->velocity.y);
    CHECK(fabsf(lightBody->velocity.y - heavyBody->velocity.y) < 0.001f &&
              fabsf(lightBody->position.y - heavyBody->position.y) < 0.001f,
          "mass changed the fall: %.4f vs %.4f cells",
          (double)lightBody->position.y, (double)heavyBody->position.y);
    DynamicTerrainUnload(&terrain);
}

static void test_damping_depends_on_time_and_not_on_step_count(void)
{
    TerrainBodyHandle coarse;
    TerrainBodyHandle fine;
    DynamicTerrainConfig config = QuietConfig();
    int step;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    config.linearDamping = 1.5f;
    terrain.config = config;
    coarse = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    fine = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    DynamicTerrainSetVelocity(&terrain, coarse, (Vector2){100.0f, 0.0f}, 0.0f);
    DynamicTerrainSetVelocity(&terrain, fine, (Vector2){100.0f, 0.0f}, 0.0f);

    /* One second, integrated two different ways. A per-call multiplier would
       leave these far apart; exp(-k*dt) leaves them equal. */
    DynamicTerrainGet(&terrain, fine)->awake = true;
    for (step = 0; step < 10; ++step) {
        TerrainBody *asleep = DynamicTerrainGet(&terrain, fine);

        asleep->awake = false;
        TerrainPhysicsUpdate(&terrain, NULL, 0.1f);
        asleep->awake = true;
    }
    {
        TerrainBody *other = DynamicTerrainGet(&terrain, coarse);

        other->awake = false;
        for (step = 0; step < 100; ++step) {
            TerrainPhysicsUpdate(&terrain, NULL, 0.01f);
        }
        other->awake = true;
    }

    CHECK(fabsf(DynamicTerrainGetConst(&terrain, coarse)->velocity.x -
                DynamicTerrainGetConst(&terrain, fine)->velocity.x) < 0.05f,
          "damping is step-count dependent: %.4f after ten steps vs %.4f after "
          "a hundred",
          (double)DynamicTerrainGetConst(&terrain, coarse)->velocity.x,
          (double)DynamicTerrainGetConst(&terrain, fine)->velocity.x);
    DynamicTerrainUnload(&terrain);
}

static void test_angular_velocity_turns_a_body(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){100.0f, 50.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){0.0f, 0.0f}, 1.0f);

    TickBodies(&terrain, 60);
    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(fabsf(body->angle - 1.0f) < 0.01f,
          "a second at one radian per second reached %.4f", (double)body->angle);
    /* Rotation must not move the centre of mass. */
    CHECK(fabsf(body->position.x - 100.0f) < 0.001f &&
              fabsf(body->position.y - 50.0f) < 0.001f,
          "spinning moved the body to %.3f,%.3f", (double)body->position.x,
          (double)body->position.y);
    DynamicTerrainUnload(&terrain);
}

static void test_angular_damping_slows_a_spin(void)
{
    TerrainBodyHandle handle;
    DynamicTerrainConfig config = QuietConfig();

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    config.angularDamping = 2.0f;
    terrain.config = config;
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    /* Below maximumAngularSpeed, so the ceiling does not quietly change what
       this test is measuring. */
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){0.0f, 0.0f}, 2.0f);
    CHECK(DynamicTerrainGetConst(&terrain, handle)->angularVelocity == 2.0f,
          "the fixture spin was clamped before the test began");

    TickBodies(&terrain, 60);
    /* exp(-2) of the original, give or take the step. */
    CHECK(fabsf(DynamicTerrainGetConst(&terrain, handle)->angularVelocity -
                2.0f * expf(-2.0f)) < 0.05f,
          "a second of damping left %.4f rad/s instead of %.4f",
          (double)DynamicTerrainGetConst(&terrain, handle)->angularVelocity,
          (double)(2.0f * expf(-2.0f)));
    DynamicTerrainUnload(&terrain);
}

/* The transform is the contract every later system reads, so it has to survive
   a round trip and it has to agree with the placement extraction chose. */
static void test_the_body_transform_round_trips(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;
    Vector2 world;
    Vector2 local;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 8, 6, (Vector2){44.0f, 33.0f});
    body = DynamicTerrainGet(&terrain, handle);

    /* Unrotated, the centre of mass sits exactly at the body's position. */
    world = TerrainBodyLocalToWorld(body, body->centerOfMass.x,
                                    body->centerOfMass.y);
    CHECK(fabsf(world.x - 44.0f) < 0.001f && fabsf(world.y - 33.0f) < 0.001f,
          "the centre of mass maps to %.3f,%.3f instead of the position",
          (double)world.x, (double)world.y);

    /* A quarter turn, then a round trip through both directions. */
    body->angle = PI * 0.5f;
    world = TerrainBodyLocalToWorld(body, 1.5f, 2.5f);
    local = TerrainBodyWorldToLocal(body, world.x, world.y);
    CHECK(fabsf(local.x - 1.5f) < 0.001f && fabsf(local.y - 2.5f) < 0.001f,
          "the transform does not round trip: 1.5,2.5 -> %.3f,%.3f -> %.3f,%.3f",
          (double)world.x, (double)world.y, (double)local.x, (double)local.y);

    /* Rotating about the centre of mass keeps its distance from the body's
       origin, which is what makes the origin choice worth having. */
    world = TerrainBodyLocalToWorld(body, 0.0f, 0.0f);
    CHECK(fabsf(sqrtf((world.x - 44.0f) * (world.x - 44.0f) +
                      (world.y - 33.0f) * (world.y - 33.0f)) -
                sqrtf(body->centerOfMass.x * body->centerOfMass.x +
                      body->centerOfMass.y * body->centerOfMass.y)) < 0.001f,
          "rotation changed a cell's distance from the centre of mass");
    DynamicTerrainUnload(&terrain);
}

static void test_kinematics_are_deterministic(void)
{
    DynamicTerrainSystem first;
    DynamicTerrainSystem second;
    TerrainBodyHandle a;
    TerrainBodyHandle b;
    int step;

    CHECK(DynamicTerrainInit(&first), "dynamic terrain allocation failed");
    CHECK(DynamicTerrainInit(&second), "dynamic terrain allocation failed");
    a = MakeKinematicBody(&first, 6, 4, (Vector2){10.0f, 20.0f});
    b = MakeKinematicBody(&second, 6, 4, (Vector2){10.0f, 20.0f});
    DynamicTerrainSetVelocity(&first, a, (Vector2){33.0f, -17.0f}, 2.5f);
    DynamicTerrainSetVelocity(&second, b, (Vector2){33.0f, -17.0f}, 2.5f);

    for (step = 0; step < 240; ++step) {
        TerrainPhysicsUpdate(&first, NULL, KINEMATIC_STEP);
        TerrainPhysicsUpdate(&second, NULL, KINEMATIC_STEP);
        if (step == 90) {
            DynamicTerrainApplyImpulse(&first, a, (Vector2){400.0f, -200.0f},
                                       (Vector2){12.0f, 30.0f});
            DynamicTerrainApplyImpulse(&second, b, (Vector2){400.0f, -200.0f},
                                       (Vector2){12.0f, 30.0f});
        }
    }

    CHECK(DynamicTerrainGetConst(&first, a)->position.x ==
                  DynamicTerrainGetConst(&second, b)->position.x &&
              DynamicTerrainGetConst(&first, a)->position.y ==
                  DynamicTerrainGetConst(&second, b)->position.y &&
              DynamicTerrainGetConst(&first, a)->angle ==
                  DynamicTerrainGetConst(&second, b)->angle,
          "two identical runs diverged: %.6f,%.6f @ %.6f vs %.6f,%.6f @ %.6f",
          (double)DynamicTerrainGetConst(&first, a)->position.x,
          (double)DynamicTerrainGetConst(&first, a)->position.y,
          (double)DynamicTerrainGetConst(&first, a)->angle,
          (double)DynamicTerrainGetConst(&second, b)->position.x,
          (double)DynamicTerrainGetConst(&second, b)->position.y,
          (double)DynamicTerrainGetConst(&second, b)->angle);
    DynamicTerrainUnload(&first);
    DynamicTerrainUnload(&second);
}

/* --- sleep and wake ----------------------------------------------------- */

static void test_a_still_body_falls_asleep(void)
{
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    CHECK(DynamicTerrainGetConst(&terrain, handle)->awake,
          "a new body is not awake");

    /* Less than the quiet time: still awake. */
    TickBodies(&terrain, 20);
    CHECK(DynamicTerrainGetConst(&terrain, handle)->awake,
          "the body slept before its quiet time elapsed");

    TickBodies(&terrain, 20);
    CHECK(!DynamicTerrainGetConst(&terrain, handle)->awake,
          "a still body never fell asleep");
    CHECK(DynamicTerrainStatistics(&terrain)->sleepingBodies == 1 &&
              DynamicTerrainStatistics(&terrain)->awakeBodies == 0,
          "counters read %d awake / %d sleeping",
          DynamicTerrainStatistics(&terrain)->awakeBodies,
          DynamicTerrainStatistics(&terrain)->sleepingBodies);
    DynamicTerrainUnload(&terrain);
}

static void test_a_sleeping_body_keeps_its_transform_and_stops_integrating(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;
    Vector2 restingPlace;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    TickBodies(&terrain, 40);
    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(!body->awake, "the body did not fall asleep");
    restingPlace = body->position;

    /* Gravity switched on under a sleeping body must not move it: a sleeping
       body is skipped entirely, which is the whole point. */
    terrain.config.gravity = 120.0f;
    TickBodies(&terrain, 120);
    CHECK(body->position.x == restingPlace.x && body->position.y == restingPlace.y,
          "a sleeping body drifted to %.3f,%.3f", (double)body->position.x,
          (double)body->position.y);
    CHECK(body->velocity.x == 0.0f && body->velocity.y == 0.0f,
          "a sleeping body kept a velocity");

    DynamicTerrainWakeBody(&terrain, handle);
    CHECK(body->awake, "waking did not take");
    TickBodies(&terrain, 60);
    CHECK(body->position.y > restingPlace.y + 10.0f,
          "a woken body did not resume falling: %.3f", (double)body->position.y);
    DynamicTerrainUnload(&terrain);
}

static void test_a_moving_or_spinning_body_stays_awake(void)
{
    TerrainBodyHandle moving;
    TerrainBodyHandle spinning;
    TerrainBodyHandle falling;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    moving = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    spinning = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    DynamicTerrainSetVelocity(&terrain, moving, (Vector2){20.0f, 0.0f}, 0.0f);
    DynamicTerrainSetVelocity(&terrain, spinning, (Vector2){0.0f, 0.0f}, 1.0f);

    TickBodies(&terrain, 120);
    CHECK(DynamicTerrainGetConst(&terrain, moving)->awake,
          "a travelling body fell asleep");
    CHECK(DynamicTerrainGetConst(&terrain, spinning)->awake,
          "a spinning body fell asleep");
    DynamicTerrainUnload(&terrain);

    /* The invariant the default config has to satisfy: a body in free fall can
       never satisfy the sleep condition, because one step of gravity already
       exceeds the linear threshold. */
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    CHECK(terrain.config.linearSleepSpeed <
              terrain.config.gravity * KINEMATIC_STEP,
          "the default sleep speed (%.3f) is not below one step of gravity "
          "(%.3f): a falling body would doze off in mid-air",
          (double)terrain.config.linearSleepSpeed,
          (double)(terrain.config.gravity * KINEMATIC_STEP));
    falling = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    TickBodies(&terrain, 300);
    CHECK(DynamicTerrainGetConst(&terrain, falling)->awake,
          "a body in free fall fell asleep");
    DynamicTerrainUnload(&terrain);
}

/* The quiet spell has to be continuous. Accumulating it across periods of
   motion would let a body that stops, moves and stops again drop off in the
   middle of the second stop, having "earned" the time while it was travelling. */
static void test_the_quiet_spell_restarts_whenever_a_body_moves(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    body = DynamicTerrainGetConst(&terrain, handle);

    /* Most of a quiet spell, but not all of it. */
    TickBodies(&terrain, 25);
    CHECK(body->awake, "the body slept too early");

    /* A shove, then quiet again for the same short while. Cumulative time would
       now exceed the delay; continuous time would not.

       The velocity is written directly rather than through
       DynamicTerrainSetVelocity, which resets the timer itself: the rule under
       test belongs to the integrator, and going through the API would prove
       only that the API works. */
    DynamicTerrainGet(&terrain, handle)->velocity.x = 40.0f;
    TickBodies(&terrain, 5);
    DynamicTerrainGet(&terrain, handle)->velocity.x = 0.0f;
    TickBodies(&terrain, 25);
    CHECK(body->awake,
          "the body counted its quiet time across a period of motion");

    /* Left alone, it still sleeps. */
    TickBodies(&terrain, 10);
    CHECK(!body->awake, "the body never slept at all");
    DynamicTerrainUnload(&terrain);
}

static void test_an_impulse_wakes_a_body_and_turns_it_about_its_centre(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    TickBodies(&terrain, 40);
    body = DynamicTerrainGet(&terrain, handle);
    CHECK(!body->awake, "the body did not fall asleep first");

    /* Straight through the centre of mass: pure translation, no spin. */
    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){body->mass * 10.0f, 0.0f},
                               body->position);
    CHECK(body->awake, "an impulse did not wake the body");
    CHECK(fabsf(body->velocity.x - 10.0f) < 0.001f,
          "an impulse of m*10 gave %.4f cells per second", (double)body->velocity.x);
    CHECK(body->angularVelocity == 0.0f,
          "an impulse through the centre of mass produced spin: %.6f",
          (double)body->angularVelocity);

    /* Off-centre: the same push now also turns it. */
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){0.0f, 0.0f}, 0.0f);
    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){body->mass * 10.0f, 0.0f},
                               (Vector2){body->position.x, body->position.y - 3.0f});
    CHECK(fabsf(body->velocity.x - 10.0f) < 0.001f,
          "an off-centre impulse changed the linear response: %.4f",
          (double)body->velocity.x);
    /* Pushing +x above the centre turns it clockwise, which is positive when
       Y grows downward. */
    CHECK(body->angularVelocity > 0.0f,
          "an off-centre impulse produced no rotation: %.6f",
          (double)body->angularVelocity);
    DynamicTerrainUnload(&terrain);
}

/* --- lifecycle and safety ----------------------------------------------- */

static void test_only_live_bodies_are_integrated(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;
    Vector2 lastSeen;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    TickBodies(&terrain, 10);
    body = DynamicTerrainGet(&terrain, handle);
    lastSeen = body->position;
    CHECK(lastSeen.y > 50.0f, "the body was meant to be falling");

    DynamicTerrainFreeBody(&terrain, handle);
    TickBodies(&terrain, 60);
    CHECK(body->position.x == lastSeen.x && body->position.y == lastSeen.y,
          "a freed body kept being integrated");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBodies == 0 &&
              DynamicTerrainStatistics(&terrain)->sleepingBodies == 0,
          "a freed body was still counted");
    DynamicTerrainUnload(&terrain);
}

static void test_reset_clears_kinetic_state(void)
{
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){80.0f, 40.0f}, 3.0f);
    TickBodies(&terrain, 30);

    DynamicTerrainReset(&terrain);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "reset left bodies alive");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBodies == 0 &&
              DynamicTerrainStatistics(&terrain)->sleepingBodies == 0,
          "reset left %d awake and %d sleeping bodies counted",
          DynamicTerrainStatistics(&terrain)->awakeBodies,
          DynamicTerrainStatistics(&terrain)->sleepingBodies);

    /* A slot reused after a reset starts from rest, not from whatever the
       previous tenant was doing. */
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){10.0f, 10.0f});
    CHECK(DynamicTerrainGetConst(&terrain, handle)->velocity.x == 0.0f &&
              DynamicTerrainGetConst(&terrain, handle)->velocity.y == 0.0f &&
              DynamicTerrainGetConst(&terrain, handle)->angularVelocity == 0.0f &&
              DynamicTerrainGetConst(&terrain, handle)->angle == 0.0f,
          "a reused slot inherited the previous body's motion");
    DynamicTerrainUnload(&terrain);
}

/* Bad input must be refused rather than turned into a NaN body, because a NaN
   transform never recovers and every later system would read it. */
static void test_the_integrator_refuses_impossible_input(void)
{
    TerrainBodyHandle handle;
    TerrainBodyHandle empty;
    TerrainBody *body;
    Vector2 before;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config = QuietConfig();
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){50.0f, 50.0f});
    body = DynamicTerrainGet(&terrain, handle);
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){10.0f, 0.0f}, 0.0f);
    before = body->position;

    /* An impossible step is a caller bug, and integrating it would hide one. */
    TerrainPhysicsUpdate(&terrain, NULL, 0.0f);
    TerrainPhysicsUpdate(&terrain, NULL, -1.0f);
    TerrainPhysicsUpdate(&terrain, NULL, 100.0f);
    TerrainPhysicsUpdate(&terrain, NULL, NAN);
    CHECK(body->position.x == before.x && body->position.y == before.y,
          "an impossible time step moved the body to %.3f,%.3f",
          (double)body->position.x, (double)body->position.y);

    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){NAN, 0.0f}, 0.0f);
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){0.0f, 0.0f}, INFINITY);
    CHECK(body->velocity.x == 10.0f && body->velocity.y == 0.0f,
          "a non-finite velocity was accepted: %.3f,%.3f",
          (double)body->velocity.x, (double)body->velocity.y);

    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){NAN, NAN},
                               body->position);
    CHECK(body->velocity.x == 10.0f, "a non-finite impulse was accepted");

    /* A massless body cannot be pushed: dividing by its mass would poison the
       transform. Extraction cannot produce one, but the guard is cheap. */
    empty = DynamicTerrainAllocBody(&terrain, 4, 4);
    DynamicTerrainFinalizeBody(&terrain, empty);
    CHECK(DynamicTerrainGetConst(&terrain, empty)->mass == 0.0f,
          "the fixture needs a massless body");
    DynamicTerrainApplyImpulse(&terrain, empty, (Vector2){100.0f, 100.0f},
                               (Vector2){0.0f, 0.0f});
    CHECK(DynamicTerrainGetConst(&terrain, empty)->velocity.x == 0.0f &&
              DynamicTerrainGetConst(&terrain, empty)->angularVelocity == 0.0f,
          "a massless body was accelerated to %.3f",
          (double)DynamicTerrainGetConst(&terrain, empty)->velocity.x);

    /* Dead handles are inert everywhere. */
    DynamicTerrainWakeBody(&terrain, TerrainBodyInvalidHandle());
    DynamicTerrainSetVelocity(&terrain, TerrainBodyInvalidHandle(),
                              (Vector2){5.0f, 5.0f}, 5.0f);
    DynamicTerrainApplyImpulse(&terrain, TerrainBodyInvalidHandle(),
                               (Vector2){5.0f, 5.0f}, (Vector2){0.0f, 0.0f});
    TerrainPhysicsUpdate(NULL, NULL, KINEMATIC_STEP);
    CHECK(body->velocity.x == 10.0f, "an invalid handle reached a real body");
    DynamicTerrainUnload(&terrain);
}

/* A speed ceiling is not tuning: it is what stops one bad impulse producing a
   body that crosses the map between two ticks. */
static void test_speeds_are_capped(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){0.0f, 0.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){1.0e6f, 0.0f}, 1.0e6f);
    body = DynamicTerrainGetConst(&terrain, handle);

    CHECK(fabsf(body->velocity.x) <= terrain.config.maximumSpeed + 0.001f,
          "speed reached %.1f past the %.1f ceiling", (double)body->velocity.x,
          (double)terrain.config.maximumSpeed);
    CHECK(fabsf(body->angularVelocity) <=
              terrain.config.maximumAngularSpeed + 0.001f,
          "spin reached %.1f past the %.1f ceiling",
          (double)body->angularVelocity,
          (double)terrain.config.maximumAngularSpeed);
    DynamicTerrainUnload(&terrain);
}

/* Integration must not touch the world; it does not even receive one. */
static void test_integration_never_touches_the_world(void)
{
    World world;
    TerrainExtractResult extracted;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK, "extraction reported %s",
          TerrainExtractStatusName(extracted.status));

    before = WorldDigest(&world);
    DynamicTerrainSetVelocity(&terrain, extracted.body, (Vector2){-500.0f, 900.0f},
                              6.0f);
    /* Long enough to carry the body far outside the world, which must be
       arithmetic and nothing more: no cell is read or written by position. */
    TickBodies(&terrain, 600);
    CHECK(WorldDigest(&world) == before,
          "integrating a body changed the world it came from");
    CHECK(DynamicTerrainGetConst(&terrain, extracted.body)->position.y > 96.0f,
          "the body was meant to leave the world for this test");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* --- terrain body vs static world --------------------------------------- */

/* Collision reads the world and never writes it; that is a `const World *` in
   the signature rather than a promise. These tests hold the behaviour that the
   type cannot: that a body lands, stops, settles and stays out of the rock. */

static void PhysicsTick(DynamicTerrainSystem *system, const World *world, int count)
{
    int step;

    for (step = 0; step < count; ++step) {
        TerrainPhysicsUpdate(system, world, KINEMATIC_STEP);
    }
}

/* A world that is empty above a flat rock floor. */
static bool BuildFloorWorld(World *world, int floorY)
{
    if (!WorldInit(world, 128, 96)) {
        return false;
    }
    FillRect(world, 0, floorY, world->width - 1, world->height - 1, MATERIAL_ROCK);
    return true;
}

/* Lowest world row any of the body's cells reaches. */
static float BodyLowestPoint(const TerrainBody *body)
{
    float lowest = body->position.y;
    int corner;

    for (corner = 0; corner < 4; ++corner) {
        Vector2 point = TerrainBodyLocalToWorld(
            body, corner & 1 ? (float)body->maximumX + 1.0f : (float)body->minimumX,
            (corner >> 1) & 1 ? (float)body->maximumY + 1.0f : (float)body->minimumY);

        if (point.y > lowest) {
            lowest = point.y;
        }
    }
    return lowest;
}

static void test_a_body_lands_on_the_floor_and_stays_out_of_it(void)
{
    World world;
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 8, 6, (Vector2){60.0f, 20.0f});
    body = DynamicTerrainGetConst(&terrain, handle);

    PhysicsTick(&terrain, &world, 240);

    /* Resting overlap is at most half a cell, because a body cell is sampled at
       its centre; see the note in terrain_physics.c. */
    CHECK(BodyLowestPoint(body) < 71.0f,
          "the body sank into the floor: its lowest point is %.3f, the floor "
          "starts at 70", (double)BodyLowestPoint(body));
    CHECK(BodyLowestPoint(body) > 68.0f,
          "the body stopped short of the floor at %.3f",
          (double)BodyLowestPoint(body));
    CHECK(body->position.y < 80.0f, "the body fell through to %.3f",
          (double)body->position.y);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_a_landed_body_settles_and_sleeps(void)
{
    World world;
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 8, 6, (Vector2){60.0f, 40.0f});
    body = DynamicTerrainGetConst(&terrain, handle);

    /* Long enough to fall, land, stop bouncing and run out the quiet time. */
    PhysicsTick(&terrain, &world, 600);
    CHECK(!body->awake,
          "a body resting on the floor never slept; it is at %.3f moving %.3f",
          (double)body->position.y, (double)body->velocity.y);
    CHECK(DynamicTerrainStatistics(&terrain)->sleepingBodies == 1,
          "the sleeping body was not counted");

    /* And it stays where it settled. */
    {
        Vector2 resting = body->position;

        PhysicsTick(&terrain, &world, 120);
        CHECK(body->position.x == resting.x && body->position.y == resting.y,
              "a sleeping body drifted to %.3f,%.3f", (double)body->position.x,
              (double)body->position.y);
    }
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_a_body_stops_against_a_wall(void)
{
    World world;
    TerrainBodyHandle handle;
    const TerrainBody *body;
    DynamicTerrainConfig config;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    config = QuietConfig();
    config.restitution = terrain.config.restitution;
    config.friction = terrain.config.friction;
    config.maximumSpeed = terrain.config.maximumSpeed;
    config.maximumAngularSpeed = terrain.config.maximumAngularSpeed;
    terrain.config = config;
    FillRect(&world, 80, 0, 84, 95, MATERIAL_ROCK);

    handle = MakeKinematicBody(&terrain, 6, 6, (Vector2){40.0f, 50.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){200.0f, 0.0f}, 0.0f);
    PhysicsTick(&terrain, &world, 120);
    body = DynamicTerrainGetConst(&terrain, handle);

    CHECK(body->position.x < 80.0f,
          "the body passed into or through the wall: x = %.3f",
          (double)body->position.x);
    CHECK(body->position.x > 60.0f, "the body never reached the wall: x = %.3f",
          (double)body->position.x);
    CHECK(body->velocity.x < 30.0f,
          "the wall did not stop the body: it is still moving at %.3f",
          (double)body->velocity.x);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* A body dropped so that only one corner lands on a ledge must turn: an
   off-centre contact that produced no spin would be an AABB pretending to be a
   physical body. */
static void test_an_off_centre_landing_turns_the_body(void)
{
    World world;
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* A narrow pillar under one end of a wide slab. */
    FillRect(&world, 40, 60, 43, 95, MATERIAL_ROCK);
    handle = MakeKinematicBody(&terrain, 24, 3, (Vector2){52.0f, 40.0f});
    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->angularVelocity == 0.0f, "the fixture starts spinning");

    PhysicsTick(&terrain, &world, 90);
    CHECK(fabsf(body->angle) > 0.02f,
          "a slab landing on one corner did not tip: angle %.5f",
          (double)body->angle);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_friction_slows_a_body_sliding_along_the_floor(void)
{
    World world;
    TerrainBodyHandle slippery;
    TerrainBodyHandle grippy;
    float slipperySpeed;
    float grippySpeed;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config.friction = 0.0f;
    slippery = MakeKinematicBody(&terrain, 8, 4, (Vector2){30.0f, 66.0f});
    DynamicTerrainSetVelocity(&terrain, slippery, (Vector2){80.0f, 0.0f}, 0.0f);
    /* Short enough that the body is still on open floor: running it into the
       far border would stop both cases for a reason that is not friction. */
    PhysicsTick(&terrain, &world, 40);
    slipperySpeed = DynamicTerrainGetConst(&terrain, slippery)->velocity.x;
    DynamicTerrainUnload(&terrain);

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config.friction = 0.9f;
    grippy = MakeKinematicBody(&terrain, 8, 4, (Vector2){30.0f, 66.0f});
    DynamicTerrainSetVelocity(&terrain, grippy, (Vector2){80.0f, 0.0f}, 0.0f);
    PhysicsTick(&terrain, &world, 40);
    grippySpeed = DynamicTerrainGetConst(&terrain, grippy)->velocity.x;

    CHECK(grippySpeed < slipperySpeed - 5.0f,
          "friction did not slow the slide: %.3f with friction against %.3f "
          "without", (double)grippySpeed, (double)slipperySpeed);
    CHECK(grippySpeed >= -1.0f, "friction reversed the slide to %.3f",
          (double)grippySpeed);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* The fastest the body is ever seen travelling upward. Sampling one fixed tick
   would miss the rebound entirely for a body that has already settled. */
static float PeakRebound(DynamicTerrainSystem *system, const World *world,
                         TerrainBodyHandle handle, int ticks)
{
    float peak = 0.0f;
    int step;

    for (step = 0; step < ticks; ++step) {
        float rising;

        TerrainPhysicsUpdate(system, world, KINEMATIC_STEP);
        rising = -DynamicTerrainGetConst(system, handle)->velocity.y;
        if (rising > peak) {
            peak = rising;
        }
    }
    return peak;
}

static void test_restitution_controls_the_bounce(void)
{
    World world;
    TerrainBodyHandle handle;
    float defaultRise;
    float bouncyRise;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 6, 4, (Vector2){60.0f, 40.0f});
    defaultRise = PeakRebound(&terrain, &world, handle, 120);
    DynamicTerrainUnload(&terrain);

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config.restitution = 0.8f;
    handle = MakeKinematicBody(&terrain, 6, 4, (Vector2){60.0f, 40.0f});
    bouncyRise = PeakRebound(&terrain, &world, handle, 120);

    CHECK(bouncyRise > defaultRise + 5.0f,
          "restitution changed nothing: %.3f upward by default against %.3f at "
          "0.8", (double)defaultRise, (double)bouncyRise);
    /* Rock is not rubber. */
    CHECK(defaultRise < 20.0f,
          "the default body bounced off the floor at %.3f cells per second",
          (double)defaultRise);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* The substep budget exists to make tunnelling impossible rather than
   unlikely, and the shipped speed ceilings are chosen against it. */
static void test_the_shipped_config_cannot_tunnel(void)
{
    DynamicTerrainConfig config = DynamicTerrainDefaultConfig();

    CHECK(TerrainPhysicsConfigIsSafe(&config, TERRAIN_BODY_MAX_BOUNDING_RADIUS,
                                     KINEMATIC_STEP),
          "the default speed ceilings (%.1f linear, %.1f angular) let the "
          "largest possible body outrun %d substeps of %.2f cells",
          (double)config.maximumSpeed, (double)config.maximumAngularSpeed,
          TERRAIN_MAX_SUBSTEPS, (double)TERRAIN_COLLISION_SUBSTEP_DISTANCE);
    /* And the check is not vacuous. */
    config.maximumSpeed = 100000.0f;
    CHECK(!TerrainPhysicsConfigIsSafe(&config, TERRAIN_BODY_MAX_BOUNDING_RADIUS,
                                      KINEMATIC_STEP),
          "the safety check accepts any speed at all");
}

static void test_a_fast_body_cannot_cross_a_thin_wall(void)
{
    World world;
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config.gravity = 0.0f;
    /* One cell thick: the thinnest thing a body can be asked not to cross. */
    FillRect(&world, 90, 0, 90, 95, MATERIAL_ROCK);

    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){20.0f, 50.0f});
    DynamicTerrainSetVelocity(&terrain, handle,
                              (Vector2){terrain.config.maximumSpeed, 0.0f}, 0.0f);
    PhysicsTick(&terrain, &world, 60);
    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->position.x < 91.0f,
          "a body at the speed ceiling crossed a one-cell wall: x = %.3f",
          (double)body->position.x);
    DynamicTerrainUnload(&terrain);

    /* Downward, through a one-cell floor, which is the case gravity makes
       common. */
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 4, 4, (Vector2){40.0f, 10.0f});
    DynamicTerrainSetVelocity(&terrain, handle,
                              (Vector2){0.0f, terrain.config.maximumSpeed}, 0.0f);
    FillRect(&world, 0, 60, 127, 60, MATERIAL_ROCK);
    PhysicsTick(&terrain, &world, 60);
    body = DynamicTerrainGetConst(&terrain, handle);
    CHECK(body->position.y < 61.0f,
          "a body at the speed ceiling fell through a one-cell floor: y = %.3f",
          (double)body->position.y);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Transformed coordinates can land anywhere, including far outside the map.
   Every world read has to survive that, which is what the sanitizers are here
   to confirm. */
static void test_a_body_outside_the_world_is_safe(void)
{
    World world;
    TerrainBodyHandle handle;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    FillRect(&world, 0, 40, 63, 47, MATERIAL_ROCK);

    /* Straddling the left edge: half its samples have negative coordinates. */
    handle = MakeKinematicBody(&terrain, 8, 8, (Vector2){0.0f, 20.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){-30.0f, 0.0f}, 1.0f);
    PhysicsTick(&terrain, &world, 120);

    /* A body cannot fly out of the map on its own — the border reads as rock,
       exactly as it does for the player — so one is placed outside by hand,
       just inside the kill margin so that this test is about coordinate safety
       and not about cleanup. Every sample's coordinates are then negative and
       large, which is the case the bounds checks exist for. */
    DynamicTerrainGet(&terrain, handle)->position =
        (Vector2){-terrain.config.killBoundsMargin * 0.5f, -50.0f};
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){0.0f, 0.0f}, 2.0f);
    PhysicsTick(&terrain, &world, 60);
    CHECK(DynamicTerrainGetConst(&terrain, handle) != NULL,
          "a body inside the kill margin was destroyed");
    CHECK(DynamicTerrainGetConst(&terrain, handle)->position.x ==
              DynamicTerrainGetConst(&terrain, handle)->position.x &&
          DynamicTerrainGetConst(&terrain, handle)->position.y ==
              DynamicTerrainGetConst(&terrain, handle)->position.y,
          "a body outside the world acquired a NaN position");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_collision_never_changes_the_world(void)
{
    World world;
    TerrainBodyHandle handle;
    uint64_t before;
    int activeBefore;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    Tick(&world, 3);
    before = WorldDigest(&world);
    activeBefore = world.activeChunkCount;

    handle = MakeKinematicBody(&terrain, 10, 8, (Vector2){60.0f, 20.0f});
    DynamicTerrainSetVelocity(&terrain, handle, (Vector2){40.0f, 200.0f}, 2.0f);
    PhysicsTick(&terrain, &world, 400);

    CHECK(WorldDigest(&world) == before,
          "collision changed the terrain it was only meant to read");
    CHECK(world.activeChunkCount == activeBefore,
          "collision woke chunks: %d active, expected %d", world.activeChunkCount,
          activeBefore);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_only_live_bodies_collide(void)
{
    World world;
    TerrainBodyHandle handle;
    TerrainBody *body;
    Vector2 lastSeen;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 6, 6, (Vector2){60.0f, 20.0f});
    PhysicsTick(&terrain, &world, 10);
    body = DynamicTerrainGet(&terrain, handle);
    lastSeen = body->position;

    DynamicTerrainFreeBody(&terrain, handle);
    PhysicsTick(&terrain, &world, 120);
    CHECK(body->position.x == lastSeen.x && body->position.y == lastSeen.y,
          "a freed body kept colliding and moved to %.3f,%.3f",
          (double)body->position.x, (double)body->position.y);
    CHECK(DynamicTerrainStatistics(&terrain)->collisionBodies == 0,
          "a freed body was counted as colliding");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_collision_is_deterministic(void)
{
    World first;
    World second;
    DynamicTerrainSystem systemA;
    DynamicTerrainSystem systemB;
    TerrainBodyHandle a;
    TerrainBodyHandle b;
    int step;

    CHECK(BuildFloorWorld(&first, 70), "world allocation failed");
    CHECK(BuildFloorWorld(&second, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&systemA), "dynamic terrain allocation failed");
    CHECK(DynamicTerrainInit(&systemB), "dynamic terrain allocation failed");

    a = MakeKinematicBody(&systemA, 9, 5, (Vector2){50.0f, 15.0f});
    b = MakeKinematicBody(&systemB, 9, 5, (Vector2){50.0f, 15.0f});
    DynamicTerrainSetVelocity(&systemA, a, (Vector2){70.0f, 30.0f}, 1.3f);
    DynamicTerrainSetVelocity(&systemB, b, (Vector2){70.0f, 30.0f}, 1.3f);

    for (step = 0; step < 300; ++step) {
        TerrainPhysicsUpdate(&systemA, &first, KINEMATIC_STEP);
        TerrainPhysicsUpdate(&systemB, &second, KINEMATIC_STEP);
    }

    CHECK(DynamicTerrainGetConst(&systemA, a)->position.x ==
                  DynamicTerrainGetConst(&systemB, b)->position.x &&
              DynamicTerrainGetConst(&systemA, a)->position.y ==
                  DynamicTerrainGetConst(&systemB, b)->position.y &&
              DynamicTerrainGetConst(&systemA, a)->angle ==
                  DynamicTerrainGetConst(&systemB, b)->angle,
          "two identical collision runs diverged: %.6f,%.6f @ %.6f vs "
          "%.6f,%.6f @ %.6f",
          (double)DynamicTerrainGetConst(&systemA, a)->position.x,
          (double)DynamicTerrainGetConst(&systemA, a)->position.y,
          (double)DynamicTerrainGetConst(&systemA, a)->angle,
          (double)DynamicTerrainGetConst(&systemB, b)->position.x,
          (double)DynamicTerrainGetConst(&systemB, b)->position.y,
          (double)DynamicTerrainGetConst(&systemB, b)->angle);
    WorldUnload(&first);
    WorldUnload(&second);
    DynamicTerrainUnload(&systemA);
    DynamicTerrainUnload(&systemB);
}

/* Contacts are capped, and the cap has to be a quality decision rather than a
   safety one: a body lying along a long floor produces far more overlaps than
   the set can hold. */
static void test_the_contact_cap_is_never_exceeded(void)
{
    World world;
    TerrainBodyHandle handle;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    /* Wide enough that its underside alone touches sixty cells. */
    handle = MakeKinematicBody(&terrain, 60, 4, (Vector2){60.0f, 40.0f});
    PhysicsTick(&terrain, &world, 300);

    CHECK(DynamicTerrainStatistics(&terrain)->maxContactsObserved > 0,
          "the wide slab never touched the floor");
    CHECK(DynamicTerrainStatistics(&terrain)->maxContactsObserved <=
              MAX_TERRAIN_CONTACTS_PER_BODY,
          "a body produced %d contacts, past the cap of %d",
          DynamicTerrainStatistics(&terrain)->maxContactsObserved,
          MAX_TERRAIN_CONTACTS_PER_BODY);
    CHECK(BodyLowestPoint(DynamicTerrainGetConst(&terrain, handle)) < 71.0f,
          "the capped contact set let a wide slab sink to %.3f",
          (double)BodyLowestPoint(DynamicTerrainGetConst(&terrain, handle)));
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Substeps are bounded, which is what makes the cost of a tick knowable. */
static void test_substeps_stay_inside_their_budget(void)
{
    World world;
    TerrainBodyHandle handle;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 8, 8, (Vector2){60.0f, 20.0f});
    DynamicTerrainSetVelocity(&terrain, handle,
                              (Vector2){terrain.config.maximumSpeed, 0.0f},
                              terrain.config.maximumAngularSpeed);
    TerrainPhysicsUpdate(&terrain, &world, KINEMATIC_STEP);

    CHECK(DynamicTerrainStatistics(&terrain)->collisionSubsteps >= 1,
          "no substeps were counted");
    CHECK(DynamicTerrainStatistics(&terrain)->collisionSubsteps <=
              TERRAIN_MAX_SUBSTEPS,
          "one body used %d substeps, past the cap of %d",
          DynamicTerrainStatistics(&terrain)->collisionSubsteps,
          TERRAIN_MAX_SUBSTEPS);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* --- dynamic terrain budgets and lifecycle ------------------------------ */

/* Every budget here answers a worst case that automatic detach will otherwise
   walk straight into: a series of explosions creating fragments faster than
   anything retires them. What is bounded is how many bodies exist, how much of
   them there is, and how many are awake — never how old they are. */

/* Consistency between the awake counter and the bodies themselves. The counter
   is maintained at every place `awake` changes, and a budget drawn against a
   counter that has drifted would be worse than no budget at all. */
static void CheckAwakeAccounting(DynamicTerrainSystem *system, const char *when)
{
    int awake = 0;
    int active = 0;
    int occupied = 0;
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (!system->bodies[slot].active) {
            continue;
        }
        ++active;
        occupied += system->bodies[slot].cellCount;
        if (system->bodies[slot].awake) {
            ++awake;
        }
    }
    CHECK(system->awakeCount == awake, "%s: counter says %d awake, %d are",
          when, system->awakeCount, awake);
    CHECK(DynamicTerrainStatistics(system)->activeBodies == active,
          "%s: counter says %d active, %d are", when,
          DynamicTerrainStatistics(system)->activeBodies, active);
    CHECK(DynamicTerrainStatistics(system)->dynamicCellsUsed == occupied,
          "%s: counter says %d cells used, %d are", when,
          DynamicTerrainStatistics(system)->dynamicCellsUsed, occupied);
    CHECK(DynamicTerrainStatistics(system)->awakeBodies +
              DynamicTerrainStatistics(system)->sleepingBodies == active,
          "%s: awake and sleeping do not add up to active", when);
}

static void test_body_slots_are_bounded_and_reusable(void)
{
    TerrainBodyHandle handles[MAX_TERRAIN_BODIES];
    TerrainBodyHandle overflow;
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        handles[index] = DynamicTerrainAllocBody(&terrain, 4, 4);
        CHECK(DynamicTerrainGet(&terrain, handles[index]) != NULL,
              "slot %d could not be allocated", index);
    }
    CheckAwakeAccounting(&terrain, "at capacity");

    overflow = DynamicTerrainAllocBody(&terrain, 4, 4);
    CHECK(DynamicTerrainGet(&terrain, overflow) == NULL,
          "the manager allocated past its body budget");
    CHECK(DynamicTerrainStatistics(&terrain)->allocationFailures == 1,
          "a refused allocation was not counted: %d",
          DynamicTerrainStatistics(&terrain)->allocationFailures);
    /* A refusal must leave everything else exactly as it was. */
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == MAX_TERRAIN_BODIES,
          "a refused allocation disturbed the live bodies");
    CheckAwakeAccounting(&terrain, "after a refusal");

    DynamicTerrainFreeBody(&terrain, handles[7]);
    CHECK(DynamicTerrainGet(&terrain,
                            DynamicTerrainAllocBody(&terrain, 4, 4)) != NULL,
          "a freed slot was not reused");
    CheckAwakeAccounting(&terrain, "after reuse");
    DynamicTerrainUnload(&terrain);
}

/* The cell budget bounds work, not memory: the raster arena is allocated once
   whatever happens, but every occupied cell is one collision may test and one
   the renderer will draw. */
static void test_the_dynamic_cell_budget_is_counted_and_enforced(void)
{
    World world;
    TerrainBodyHandle handle;
    TerrainExtractResult extracted;
    uint64_t before;
    int used;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    handle = DynamicTerrainAllocBody(&terrain, 10, 10);
    FillBody(&terrain, handle, 0, 0, 9, 9, MATERIAL_ROCK, 20.0f);
    used = DynamicTerrainStatistics(&terrain)->dynamicCellsUsed;
    CHECK(used == 100, "a hundred filled cells counted as %d", used);
    DynamicTerrainFinalizeBody(&terrain, handle);
    CHECK(DynamicTerrainStatistics(&terrain)->dynamicCellsUsed == 100,
          "finalize disagreed with the running count: %d",
          DynamicTerrainStatistics(&terrain)->dynamicCellsUsed);

    DynamicTerrainFreeBody(&terrain, handle);
    CHECK(DynamicTerrainStatistics(&terrain)->dynamicCellsUsed == 0,
          "freeing a body did not return its cells: %d left",
          DynamicTerrainStatistics(&terrain)->dynamicCellsUsed);

    /* With the budget spent, extraction must refuse — and refuse atomically. */
    FillRect(&world, 40, 30, 47, 35, MATERIAL_ROCK);
    handle = DynamicTerrainAllocBody(&terrain, 64, 60);
    FillBody(&terrain, handle, 0, 0, 63, 59, MATERIAL_ROCK, 20.0f);
    terrain.config.maxDynamicCells =
        DynamicTerrainStatistics(&terrain)->dynamicCellsUsed + 10;
    before = WorldDigest(&world);

    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_CELL_CAPACITY,
          "extraction past the cell budget reported %s",
          TerrainExtractStatusName(extracted.status));
    CHECK(WorldDigest(&world) == before,
          "a budget refusal changed the world");
    CHECK(DynamicTerrainStatistics(&terrain)->cellCapacityFailures == 1,
          "the cell budget refusal was not counted: %d",
          DynamicTerrainStatistics(&terrain)->cellCapacityFailures);
    CheckAwakeAccounting(&terrain, "after a budget refusal");

    /* Freeing the hog lets the same extraction through. */
    DynamicTerrainFreeBody(&terrain, handle);
    extracted = ExtractAt(&world, &terrain, WholeWorld(&world), 41, 34);
    CHECK(extracted.status == TERRAIN_EXTRACT_OK,
          "extraction still refused after the budget was freed: %s",
          TerrainExtractStatusName(extracted.status));
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_the_awake_budget_is_honoured(void)
{
    TerrainBodyHandle handles[MAX_TERRAIN_BODIES];
    int index;
    int awakeLimit;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    terrain.config.maxAwakeBodies = 4;
    awakeLimit = terrain.config.maxAwakeBodies;

    for (index = 0; index < 8; ++index) {
        handles[index] = DynamicTerrainAllocBody(&terrain, 4, 4);
    }
    /* Allocation wakes a body, and the first four filled the budget; the rest
       were created asleep rather than refused, because a body that exists but
       is not moving is still a body. */
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 8,
          "the awake budget refused an allocation it should not have");
    CheckAwakeAccounting(&terrain, "after filling the awake budget");

    /* Waking one more is refused, deterministically and countably. */
    CHECK(!DynamicTerrainWakeBody(&terrain, handles[7]),
          "waking past the budget succeeded");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBudgetRefusals > 0,
          "a refused wake was not counted");
    CHECK(!DynamicTerrainGetConst(&terrain, handles[7])->awake,
          "a refused body woke anyway");

    /* And an impulse on a refused body must not leave it holding a velocity it
       is not allowed to use. */
    DynamicTerrainApplyImpulse(&terrain, handles[7], (Vector2){500.0f, 0.0f},
                               DynamicTerrainGetConst(&terrain, handles[7])->position);
    CHECK(DynamicTerrainGetConst(&terrain, handles[7])->velocity.x == 0.0f,
          "a sleeping body kept an impulse it could not act on: %.3f",
          (double)DynamicTerrainGetConst(&terrain, handles[7])->velocity.x);

    /* Freeing an awake body gives its slot in the budget back. */
    DynamicTerrainFreeBody(&terrain, handles[0]);
    CHECK(DynamicTerrainWakeBody(&terrain, handles[7]),
          "the budget was not released by freeing an awake body");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBodies == awakeLimit,
          "the awake count is %d, expected %d",
          DynamicTerrainStatistics(&terrain)->awakeBodies, awakeLimit);
    CheckAwakeAccounting(&terrain, "after releasing the budget");
    DynamicTerrainUnload(&terrain);
}

/* Sleeping is what gives the awake budget back, so it has to be observable
   through the collision path and not just in isolation. */
static void test_sleeping_on_the_ground_releases_the_awake_budget(void)
{
    World world;
    TerrainBodyHandle handle;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 8, 6, (Vector2){60.0f, 40.0f});
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBodies == 1,
          "a new body is not awake");

    PhysicsTick(&terrain, &world, 600);
    CHECK(!DynamicTerrainGetConst(&terrain, handle)->awake,
          "a body resting on the floor never slept");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBodies == 0 &&
              DynamicTerrainStatistics(&terrain)->sleepingBodies == 1,
          "the settled body is counted as %d awake / %d sleeping",
          DynamicTerrainStatistics(&terrain)->awakeBodies,
          DynamicTerrainStatistics(&terrain)->sleepingBodies);
    CheckAwakeAccounting(&terrain, "after settling");

    /* And an impulse brings it back. */
    CHECK(DynamicTerrainWakeBody(&terrain, handle), "the settled body would not wake");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBodies == 1,
          "waking did not reclaim the awake slot");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* A body that has left the map can never touch anything again, so it would fall
   for ever and hold an awake slot nothing could reclaim. */
static void test_a_body_lost_outside_the_world_is_destroyed(void)
{
    World world;
    TerrainBodyHandle lost;
    TerrainBodyHandle nearby;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");

    lost = MakeKinematicBody(&terrain, 4, 4, (Vector2){64.0f, 48.0f});
    nearby = MakeKinematicBody(&terrain, 4, 4, (Vector2){64.0f, 48.0f});
    /* One well past the kill margin, one just inside it. */
    DynamicTerrainGet(&terrain, lost)->position =
        (Vector2){-terrain.config.killBoundsMargin * 4.0f, 48.0f};
    DynamicTerrainGet(&terrain, nearby)->position =
        (Vector2){-terrain.config.killBoundsMargin * 0.5f, 48.0f};

    TerrainPhysicsUpdate(&terrain, &world, KINEMATIC_STEP);

    CHECK(DynamicTerrainGet(&terrain, lost) == NULL,
          "a body far outside the world survived");
    CHECK(DynamicTerrainGet(&terrain, nearby) != NULL,
          "a body inside the kill margin was destroyed with it");
    CHECK(DynamicTerrainStatistics(&terrain)->bodiesRemovedOutOfBounds == 1,
          "the removal was not counted: %d",
          DynamicTerrainStatistics(&terrain)->bodiesRemovedOutOfBounds);
    CheckAwakeAccounting(&terrain, "after cleanup");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Cleanup is a world-safety rule, not a camera one. A body resting quietly in a
   corner of the map is exactly what the player should find when they come
   back, however long they have been away and wherever they were looking. */
static void test_an_offscreen_body_is_never_destroyed(void)
{
    World world;
    TerrainBodyHandle handle;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 6, 4, (Vector2){10.0f, 40.0f});

    /* Twenty seconds of simulation, far from anything a camera would show. */
    PhysicsTick(&terrain, &world, 1200);
    CHECK(DynamicTerrainGet(&terrain, handle) != NULL,
          "a settled body inside the world was destroyed for being idle");
    CHECK(DynamicTerrainStatistics(&terrain)->bodiesRemovedOutOfBounds == 0,
          "an in-world body was counted as lost");
    CHECK(!DynamicTerrainGetConst(&terrain, handle)->awake,
          "the body should have settled by now");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_world_bounds_follow_the_body(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;
    Vector2 minimum;
    Vector2 maximum;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeKinematicBody(&terrain, 8, 4, (Vector2){50.0f, 50.0f});
    body = DynamicTerrainGet(&terrain, handle);

    CHECK(TerrainBodyWorldBounds(body, &minimum, &maximum),
          "a live body reported no bounds");
    CHECK(fabsf(maximum.x - minimum.x - 8.0f) < 0.001f &&
              fabsf(maximum.y - minimum.y - 4.0f) < 0.001f,
          "an unrotated 8x4 body measures %.3f x %.3f",
          (double)(maximum.x - minimum.x), (double)(maximum.y - minimum.y));
    CHECK(fabsf((minimum.x + maximum.x) * 0.5f - 50.0f) < 0.001f,
          "the box is not centred on the body: %.3f",
          (double)((minimum.x + maximum.x) * 0.5f));

    /* A quarter turn swaps the extents. */
    body->angle = PI * 0.5f;
    CHECK(TerrainBodyWorldBounds(body, &minimum, &maximum), "bounds failed");
    CHECK(fabsf(maximum.x - minimum.x - 4.0f) < 0.01f &&
              fabsf(maximum.y - minimum.y - 8.0f) < 0.01f,
          "a rotated body measures %.3f x %.3f",
          (double)(maximum.x - minimum.x), (double)(maximum.y - minimum.y));

    /* An empty body has no extent to report. */
    DynamicTerrainFreeBody(&terrain, handle);
    CHECK(!TerrainBodyWorldBounds(body, &minimum, &maximum),
          "a freed body still reported bounds");
    DynamicTerrainUnload(&terrain);
}

static void test_reset_returns_every_budget(void)
{
    World world;
    TerrainBodyHandle handle;
    int index;

    CHECK(BuildFloorWorld(&world, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    for (index = 0; index < 6; ++index) {
        handle = MakeKinematicBody(&terrain, 8, 6, (Vector2){40.0f, 30.0f});
        CHECK(DynamicTerrainGet(&terrain, handle) != NULL, "allocation failed");
    }
    PhysicsTick(&terrain, &world, 30);

    /* One refusal on the record, so the test can say what Reset does with it.
       The body is settled first: waking is only ever refused to a body that is
       actually asleep. */
    DynamicTerrainGet(&terrain, handle)->velocity = (Vector2){0.0f, 0.0f};
    DynamicTerrainGet(&terrain, handle)->angularVelocity = 0.0f;
    for (index = 0; index < 64 && DynamicTerrainGetConst(&terrain, handle)->awake;
         ++index) {
        DynamicTerrainSettleBody(&terrain, DynamicTerrainGet(&terrain, handle),
                                 KINEMATIC_STEP);
    }
    CHECK(!DynamicTerrainGetConst(&terrain, handle)->awake,
          "the body would not settle");
    terrain.config.maxAwakeBodies = 0;
    CHECK(!DynamicTerrainWakeBody(&terrain, handle), "the budget did not refuse");
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBudgetRefusals == 1,
          "the refusal was not counted: %d",
          DynamicTerrainStatistics(&terrain)->awakeBudgetRefusals);

    DynamicTerrainReset(&terrain);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0 &&
              DynamicTerrainStatistics(&terrain)->awakeBodies == 0 &&
              DynamicTerrainStatistics(&terrain)->sleepingBodies == 0 &&
              DynamicTerrainStatistics(&terrain)->dynamicCellsUsed == 0 &&
              DynamicTerrainStatistics(&terrain)->allocatedDynamicCells == 0,
          "reset left usage behind: %d bodies, %d awake, %d cells used, %d "
          "reserved", DynamicTerrainStatistics(&terrain)->activeBodies,
          DynamicTerrainStatistics(&terrain)->awakeBodies,
          DynamicTerrainStatistics(&terrain)->dynamicCellsUsed,
          DynamicTerrainStatistics(&terrain)->allocatedDynamicCells);
    CheckAwakeAccounting(&terrain, "after reset");
    /* Peaks are what the session demanded and deliberately survive. */
    CHECK(DynamicTerrainStatistics(&terrain)->peakBodies == 6 &&
              DynamicTerrainStatistics(&terrain)->peakAwakeBodies == 6,
          "reset erased the peaks: %d bodies, %d awake",
          DynamicTerrainStatistics(&terrain)->peakBodies,
          DynamicTerrainStatistics(&terrain)->peakAwakeBodies);
    CHECK(DynamicTerrainStatistics(&terrain)->peakDynamicCells >= 6 * 48,
          "peak cells were erased: %d",
          DynamicTerrainStatistics(&terrain)->peakDynamicCells);
    /* Refusals are session history for the same reason the peaks are: they say
       what the world demanded, not what is left after clearing it. */
    CHECK(DynamicTerrainStatistics(&terrain)->awakeBudgetRefusals == 1,
          "reset erased the refusal count: %d",
          DynamicTerrainStatistics(&terrain)->awakeBudgetRefusals);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* The same scenario twice must retire, refuse and settle the same bodies at the
   same moments: lifecycle decisions are simulation state like any other. */
static void test_lifecycle_decisions_are_deterministic(void)
{
    World first;
    World second;
    DynamicTerrainSystem systemA;
    DynamicTerrainSystem systemB;
    int step;
    int index;

    CHECK(BuildFloorWorld(&first, 70), "world allocation failed");
    CHECK(BuildFloorWorld(&second, 70), "world allocation failed");
    CHECK(DynamicTerrainInit(&systemA), "dynamic terrain allocation failed");
    CHECK(DynamicTerrainInit(&systemB), "dynamic terrain allocation failed");
    systemA.config.maxAwakeBodies = 5;
    systemB.config.maxAwakeBodies = 5;

    for (index = 0; index < 10; ++index) {
        TerrainBodyHandle a = MakeKinematicBody(&systemA, 5, 4,
                                                (Vector2){20.0f + (float)index * 9.0f,
                                                          30.0f});
        TerrainBodyHandle b = MakeKinematicBody(&systemB, 5, 4,
                                                (Vector2){20.0f + (float)index * 9.0f,
                                                          30.0f});

        DynamicTerrainSetVelocity(&systemA, a, (Vector2){(float)index * 3.0f, 0.0f},
                                  0.4f);
        DynamicTerrainSetVelocity(&systemB, b, (Vector2){(float)index * 3.0f, 0.0f},
                                  0.4f);
    }
    for (step = 0; step < 400; ++step) {
        TerrainPhysicsUpdate(&systemA, &first, KINEMATIC_STEP);
        TerrainPhysicsUpdate(&systemB, &second, KINEMATIC_STEP);
    }

    CHECK(DynamicTerrainStatistics(&systemA)->activeBodies ==
                  DynamicTerrainStatistics(&systemB)->activeBodies &&
              DynamicTerrainStatistics(&systemA)->awakeBodies ==
                  DynamicTerrainStatistics(&systemB)->awakeBodies &&
              DynamicTerrainStatistics(&systemA)->awakeBudgetRefusals ==
                  DynamicTerrainStatistics(&systemB)->awakeBudgetRefusals &&
              DynamicTerrainStatistics(&systemA)->bodiesRemovedOutOfBounds ==
                  DynamicTerrainStatistics(&systemB)->bodiesRemovedOutOfBounds &&
              DynamicTerrainStatistics(&systemA)->dynamicCellsUsed ==
                  DynamicTerrainStatistics(&systemB)->dynamicCellsUsed,
          "two identical runs made different lifecycle decisions");
    WorldUnload(&first);
    WorldUnload(&second);
    DynamicTerrainUnload(&systemA);
    DynamicTerrainUnload(&systemB);
}

/* --- automatic detachment ------------------------------------------------ */

/* The whole point of this layer is what it does *not* do: it never goes looking
   for loose terrain. Checks run only where a destructive operation just removed
   structural material, so every test below either causes damage or proves that
   the absence of damage causes no work. */

static TerrainDetachSystem detach;

/* The showcase shape, and the one the manual acceptance run uses:

       ########   block
          #       pillar
       ########   ground

   Returns the number of cells in the block. */
static int BuildPillarScene(World *world, int blockLeft, int blockRight,
                            int blockTop, int blockBottom, int pillarX,
                            int groundTop)
{
    FillRect(world, 0, groundTop, world->width - 1, world->height - 1,
             MATERIAL_ROCK);
    FillRect(world, pillarX, blockBottom + 1, pillarX, groundTop - 1,
             MATERIAL_ROCK);
    FillRect(world, blockLeft, blockTop, blockRight, blockBottom, MATERIAL_ROCK);
    return (blockRight - blockLeft + 1) * (blockBottom - blockTop + 1);
}

static int RunDetach(World *world, DynamicTerrainSystem *bodies)
{
    return TerrainDetachProcess(&detach, world, bodies, NULL);
}

static void test_an_explosion_under_a_block_detaches_it(void)
{
    World world;
    int blockCells;
    int solidBefore;
    int solidAfter;
    const TerrainBody *body = NULL;
    int slot;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    blockCells = BuildPillarScene(&world, 52, 69, 64, 69, 60, 80);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK);

    /* Nothing has happened yet, so nothing may be checked. */
    CHECK(RunDetach(&world, &terrain) == 0, "a quiet world produced a body");
    CHECK(detach.stats.detachChecks == 0,
          "the detector ran without any destruction: %d checks",
          detach.stats.detachChecks);

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    CHECK(world.destructionCount == 1, "the blast logged %d regions",
          world.destructionCount);

    CHECK(RunDetach(&world, &terrain) == 1, "the block did not come loose");
    CHECK(detach.stats.autoDetachSucceeded == 1, "extraction was not counted");
    CHECK(world.destructionCount == 0, "the damage log was not drained");

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (terrain.bodies[slot].active) {
            body = &terrain.bodies[slot];
        }
    }
    CHECK(body != NULL, "no live body after a successful detach");
    /* The block plus whatever of the pillar stayed attached to it. */
    CHECK(body->cellCount >= blockCells,
          "the body holds %d cells, the block alone had %d", body->cellCount,
          blockCells);
    CHECK(body->cellCount <= blockCells + 4,
          "the body swallowed more than the block and its stub: %d",
          body->cellCount);

    /* Every cell the body holds left the static world, and no other cell did.
       Solid cells now = before - blast - body. */
    solidAfter = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(solidAfter == solidBefore - detach.stats.autoDetachCells -
                            (solidBefore - solidAfter -
                             detach.stats.autoDetachCells),
          "cell bookkeeping is inconsistent");
    {
        int x;
        int y;
        int survivors = 0;

        for (y = 64; y <= 69; ++y) {
            for (x = 52; x <= 69; ++x) {
                if (WorldGetCell(&world, x, y) != MATERIAL_EMPTY) {
                    ++survivors;
                }
            }
        }
        CHECK(survivors == 0, "%d block cells were left in the static world",
              survivors);
    }
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Damage alone is not a reason to detach anything. */
static void test_damage_that_leaves_the_support_standing_detaches_nothing(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    (void)BuildPillarScene(&world, 52, 69, 64, 69, 60, 80);

    /* A bite out of the block's far corner. The pillar is untouched. */
    WorldDestroyCircle(&world, 53, 65, 3, 0.0f);
    before = WorldDigest(&world);

    CHECK(RunDetach(&world, &terrain) == 0, "an anchored block was torn out");
    CHECK(detach.stats.detachChecks > 0, "the blast was not checked at all");
    CHECK(detach.stats.autoDetachRejectedAnchored > 0,
          "the block was not recognised as anchored");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "a body appeared from anchored terrain");
    CHECK(WorldDigest(&world) == before, "a refusal changed the world");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_drilling_through_a_support_detaches_the_section_above(void)
{
    World world;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    (void)BuildPillarScene(&world, 52, 69, 64, 69, 60, 80);

    /* A cut the width of the drill, straight through the pillar. */
    CHECK(WorldDrillCircle(&world, 60, 75, 2) > 0, "the drill cut nothing");
    CHECK(world.destructionCount == 1, "the drill logged %d regions",
          world.destructionCount);
    CHECK(RunDetach(&world, &terrain) == 1, "the cut section did not come loose");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 1,
          "the drill produced %d bodies",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* A fragment the search window cannot contain is not a fragment as far as this
   layer is concerned. It reads as UNKNOWN and stays exactly where it is. */
static void test_a_fragment_that_escapes_the_search_window_stays_static(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 400, 200), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    FillRect(&world, 0, 180, 399, 199, MATERIAL_ROCK);
    /* A beam three times wider than the window, on one pillar. */
    FillRect(&world, 10, 100, 380, 101, MATERIAL_ROCK);
    FillRect(&world, 200, 102, 200, 179, MATERIAL_ROCK);

    WorldDestroyCircle(&world, 200, 140, 5, 0.0f);
    before = WorldDigest(&world);

    CHECK(RunDetach(&world, &terrain) == 0,
          "a beam wider than the search window was extracted");
    CHECK(detach.stats.autoDetachRejectedUnknown > 0,
          "the escaping component was not reported unknown");
    CHECK(WorldDigest(&world) == before, "the world changed anyway");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_a_fragment_below_the_minimum_size_stays_static(void)
{
    World world;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    /* A two-by-two chip on a pillar: four cells, below the threshold. */
    (void)BuildPillarScene(&world, 60, 61, 68, 69, 60, 80);

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    CHECK(RunDetach(&world, &terrain) == 0, "a four-cell chip became a body");
    CHECK(detach.stats.autoDetachRejectedTooSmall > 0,
          "the chip was not rejected for its size");
    CHECK(WorldGetCell(&world, 60, 68) == MATERIAL_ROCK,
          "the chip was removed from the world anyway");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_a_fragment_above_the_maximum_size_stays_static(void)
{
    World world;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    /* 45x30 is 1350 cells, comfortably past the default ceiling of 1024. */
    (void)BuildPillarScene(&world, 20, 64, 30, 59, 60, 80);

    WorldDestroyCircle(&world, 60, 70, 4, 0.0f);
    CHECK(RunDetach(&world, &terrain) == 0, "an oversized slab became a body");
    CHECK(detach.stats.autoDetachRejectedTooLarge > 0,
          "the slab was not rejected for its size");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) > 1300,
          "the slab left the static world");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* One cell past the ceiling. The detector proves this one detached — it is
   allowed one cell more than policy accepts — so the refusal below is this
   module's decision and not the detector's. */
static void test_a_fragment_one_cell_past_the_ceiling_stays_static(void)
{
    World world;
    int blockCells;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    /* 10x10 block plus the one-cell pillar stub the blast leaves attached. */
    blockCells = BuildPillarScene(&world, 55, 64, 60, 69, 60, 80);
    CHECK(blockCells == 100, "the fixture holds %d cells", blockCells);
    detach.config.maximumBodyCells = blockCells;

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    CHECK(RunDetach(&world, &terrain) == 0,
          "a fragment one cell over the ceiling was extracted");
    CHECK(detach.stats.autoDetachRejectedTooLarge > 0,
          "the fragment was not refused for its size");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) >= blockCells,
          "the fragment left the static world");

    /* One cell of headroom is all it takes. The same area is pointed at again
       through the ordinary logging API rather than by cutting more terrain, so
       nothing about the fragment changes between the two attempts. */
    CHECK(RunDetach(&world, &terrain) == 0, "the drained log ran again");
    detach.config.maximumBodyCells = blockCells + 1;
    WorldRecordDestruction(&world, 59, 70, 61, 79);
    CHECK(RunDetach(&world, &terrain) == 1,
          "the same fragment was refused with room to spare");
    CHECK(DynamicTerrainStatistics(&terrain)->dynamicCellsUsed == blockCells + 1,
          "the body holds %d cells, expected %d",
          DynamicTerrainStatistics(&terrain)->dynamicCellsUsed, blockCells + 1);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Every budget of EF-DYN-010 applies here, and a refusal leaves terrain
   standing rather than deleting it. */
static void test_a_full_body_manager_leaves_the_fragment_static(void)
{
    World world;
    uint64_t before;
    int index;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    (void)BuildPillarScene(&world, 52, 69, 64, 69, 60, 80);
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        CHECK(DynamicTerrainGet(&terrain, DynamicTerrainAllocBody(&terrain, 2, 2)) !=
                  NULL, "filling the manager failed at %d", index);
    }

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    before = WorldDigest(&world);
    CHECK(RunDetach(&world, &terrain) == 0, "extraction succeeded with no slots");
    CHECK(detach.stats.autoDetachRejectedBudget > 0,
          "the budget refusal was not counted");
    CHECK(WorldDigest(&world) == before,
          "a budget refusal changed the static world");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_a_full_cell_budget_leaves_the_world_unchanged(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    (void)BuildPillarScene(&world, 52, 69, 64, 69, 60, 80);
    terrain.config.maxDynamicCells = 4;

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    before = WorldDigest(&world);
    CHECK(RunDetach(&world, &terrain) == 0, "extraction ignored the cell budget");
    CHECK(detach.stats.autoDetachRejectedBudget > 0,
          "the cell budget refusal was not counted");
    CHECK(WorldDigest(&world) == before, "a refused extraction changed the world");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* One blast, two independent islands. */
static void test_one_blast_can_detach_two_islands(void)
{
    World world;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    FillRect(&world, 0, 80, 127, 95, MATERIAL_ROCK);
    /* Two blocks on two pillars, both inside one blast. */
    FillRect(&world, 44, 64, 55, 69, MATERIAL_ROCK);
    FillRect(&world, 50, 70, 50, 79, MATERIAL_ROCK);
    FillRect(&world, 72, 64, 83, 69, MATERIAL_ROCK);
    FillRect(&world, 77, 70, 77, 79, MATERIAL_ROCK);

    WorldDestroyCircle(&world, 63, 75, 16, 0.0f);
    CHECK(world.destructionCount == 1, "the blast logged %d regions",
          world.destructionCount);
    CHECK(RunDetach(&world, &terrain) == 2, "one blast produced %d bodies",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 2,
          "two islands became %d bodies",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Chunks are a simulation schedule, not a structural boundary. A fragment
   straddling one must behave exactly like any other. */
static void test_a_fragment_across_a_chunk_boundary_still_detaches(void)
{
    World world;
    int boundary = WORLD_CHUNK_SIZE * 2;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    (void)BuildPillarScene(&world, boundary - 9, boundary + 8, 64, 69, boundary,
                           80);
    CHECK(boundary % WORLD_CHUNK_SIZE == 0, "the fixture is not on a boundary");

    WorldDestroyCircle(&world, boundary, 75, 4, 0.0f);
    CHECK(RunDetach(&world, &terrain) == 1,
          "a fragment across a chunk boundary was not detached");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Ordinary simulation must never reach the detector. A world full of falling
   sand is exactly the case a full-world scan would make unplayable. */
static void test_ordinary_simulation_never_runs_the_detector(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    FillRect(&world, 0, 80, 127, 95, MATERIAL_ROCK);
    FillRect(&world, 20, 20, 100, 40, MATERIAL_SAND);
    FillRect(&world, 20, 45, 100, 55, MATERIAL_WATER);

    for (step = 0; step < 120; ++step) {
        WorldUpdate(&world);
        CHECK(world.destructionCount == 0,
              "ordinary simulation logged destruction at step %d", step);
        (void)RunDetach(&world, &terrain);
    }
    CHECK(detach.stats.detachChecks == 0,
          "the detector ran %d times without a destructive event",
          detach.stats.detachChecks);
    CHECK(detach.stats.regionsProcessed == 0, "a region was processed anyway");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "settling material produced a body");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* The bound the whole design exists for: a check costs at most one cell more
   than the largest body policy accepts, whatever the world is made of. Damage
   cut into the middle of a solid landmass is the case that would be a
   fourteen-million-cell flood fill without it. */
static void test_a_check_never_explores_past_its_cell_limit(void)
{
    World world;
    int perCheck;

    CHECK(WorldInit(&world, 400, 200), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    /* Solid from side to side and top to bottom: every seed belongs to one mass
       far larger than any search may explore. */
    FillRect(&world, 0, 0, 399, 199, MATERIAL_ROCK);
    detach.config.maximumBodyCells = 64;

    WorldDestroyCircle(&world, 200, 100, 6, 0.0f);
    CHECK(RunDetach(&world, &terrain) == 0, "solid rock produced a body");
    CHECK(detach.stats.detachChecks > 0, "nothing was checked");

    perCheck = detach.stats.detachCellsExplored / detach.stats.detachChecks;
    CHECK(perCheck <= detach.config.maximumBodyCells + 1,
          "a check explored %d cells on average, the limit is %d", perCheck,
          detach.config.maximumBodyCells + 1);
    CHECK(detach.stats.detachChecks <= detach.config.maxCandidatesPerRegion,
          "%d checks ran, the cap is %d", detach.stats.detachChecks,
          detach.config.maxCandidatesPerRegion);
    /* And the whole call, not just one check, stays inside the product of the
       two caps — the claim the module's header makes. */
    CHECK(detach.stats.detachCellsExplored <=
              detach.config.maxCandidatesPerRegion *
                  (detach.config.maximumBodyCells + 1),
          "one call explored %d cells, the bound is %d",
          detach.stats.detachCellsExplored,
          detach.config.maxCandidatesPerRegion *
              (detach.config.maximumBodyCells + 1));
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Same world, same damage, same result — including which body got which slot. */
static void test_automatic_detachment_is_deterministic(void)
{
    World first;
    World second;
    DynamicTerrainSystem terrainA;
    DynamicTerrainSystem terrainB;
    TerrainDetachSystem detachA;
    TerrainDetachSystem detachB;
    int slot;

    CHECK(WorldInit(&first, 128, 96) && WorldInit(&second, 128, 96),
          "world allocation failed");
    CHECK(DynamicTerrainInit(&terrainA) && DynamicTerrainInit(&terrainB),
          "dynamic terrain allocation failed");
    TerrainDetachInit(&detachA);
    TerrainDetachInit(&detachB);
    FillRect(&first, 0, 80, 127, 95, MATERIAL_ROCK);
    FillRect(&first, 44, 64, 55, 69, MATERIAL_ROCK);
    FillRect(&first, 50, 70, 50, 79, MATERIAL_ROCK);
    FillRect(&first, 72, 64, 83, 69, MATERIAL_ROCK);
    FillRect(&first, 77, 70, 77, 79, MATERIAL_ROCK);
    FillRect(&second, 0, 80, 127, 95, MATERIAL_ROCK);
    FillRect(&second, 44, 64, 55, 69, MATERIAL_ROCK);
    FillRect(&second, 50, 70, 50, 79, MATERIAL_ROCK);
    FillRect(&second, 72, 64, 83, 69, MATERIAL_ROCK);
    FillRect(&second, 77, 70, 77, 79, MATERIAL_ROCK);

    WorldDestroyCircle(&first, 63, 75, 16, 0.0f);
    WorldDestroyCircle(&second, 63, 75, 16, 0.0f);
    CHECK(TerrainDetachProcess(&detachA, &first, &terrainA, NULL) ==
              TerrainDetachProcess(&detachB, &second, &terrainB, NULL),
          "the same damage produced a different number of bodies");
    CHECK(WorldDigest(&first) == WorldDigest(&second),
          "the same damage left two different worlds");

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *a = &terrainA.bodies[slot];
        const TerrainBody *b = &terrainB.bodies[slot];

        CHECK(a->active == b->active, "slot %d differs in occupancy", slot);
        if (!a->active) {
            continue;
        }
        CHECK(a->generation == b->generation && a->cellCount == b->cellCount &&
                  a->position.x == b->position.x &&
                  a->position.y == b->position.y,
              "slot %d holds a different body", slot);
    }
    CHECK(detachA.stats.detachChecks == detachB.stats.detachChecks &&
              detachA.stats.autoDetachSucceeded == detachB.stats.autoDetachSucceeded,
          "the two runs made different decisions");
    WorldUnload(&first);
    WorldUnload(&second);
    DynamicTerrainUnload(&terrainA);
    DynamicTerrainUnload(&terrainB);
}

/* A component is extracted once, and what it took is exactly what left. */
static void test_detachment_conserves_every_cell_it_moves(void)
{
    World world;
    int solidBefore;
    int solidAfter;
    int blastCells;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    (void)BuildPillarScene(&world, 52, 69, 64, 69, 60, 80);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK);

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    blastCells = solidBefore - CountMaterial(&world, MATERIAL_ROCK);
    CHECK(blastCells > 0, "the blast removed nothing");

    CHECK(RunDetach(&world, &terrain) == 1, "nothing came loose");
    solidAfter = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(solidBefore - blastCells - solidAfter == detach.stats.autoDetachCells,
          "the world lost %d cells but the body reports %d",
          solidBefore - blastCells - solidAfter, detach.stats.autoDetachCells);
    CHECK(DynamicTerrainStatistics(&terrain)->dynamicCellsUsed ==
              detach.stats.autoDetachCells,
          "the body holds %d cells, extraction reported %d",
          DynamicTerrainStatistics(&terrain)->dynamicCellsUsed,
          detach.stats.autoDetachCells);

    /* Running again with no new damage must do nothing at all. */
    CHECK(RunDetach(&world, &terrain) == 0, "a second pass extracted again");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 1,
          "a duplicate body appeared");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Aggregation is what keeps a drill that cuts many cells a tick from asking for
   many searches. */
static void test_overlapping_damage_aggregates_into_one_region(void)
{
    World world;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    FillRect(&world, 0, 40, 127, 95, MATERIAL_ROCK);

    WorldDestroyCircle(&world, 60, 60, 3, 0.0f);
    WorldDestroyCircle(&world, 63, 60, 3, 0.0f);
    WorldDestroyCircle(&world, 66, 60, 3, 0.0f);
    CHECK(world.destructionCount == 1, "three overlapping cuts logged %d regions",
          world.destructionCount);

    /* Far enough apart to stay separate. */
    WorldDestroyCircle(&world, 10, 60, 3, 0.0f);
    CHECK(world.destructionCount == 2, "a distant cut merged: %d regions",
          world.destructionCount);

    /* A cut in open air severs nothing and must not ask for a search. */
    WorldDestroyCircle(&world, 60, 10, 5, 0.0f);
    CHECK(world.destructionCount == 2,
          "a blast in open air logged damage: %d regions",
          world.destructionCount);
    WorldUnload(&world);
}

/* Aggregation has a ceiling. Two cuts that touch but together span more than a
   search window can cover must stay two entries: merging them would produce a
   box nothing ever looks at, losing both instead of keeping two that work. */
static void test_touching_damage_too_wide_to_merge_stays_separate(void)
{
    World world;

    CHECK(WorldInit(&world, 400, 200), "world allocation failed");
    FillRect(&world, 0, 40, 399, 199, MATERIAL_ROCK);

    WorldDestroyCircle(&world, 100, 100, 35, 0.0f);
    CHECK(world.destructionCount == 1, "the first cut logged %d regions",
          world.destructionCount);
    /* Adjacent to the first, so the boxes touch; together they span 142 cells,
       past WORLD_DESTRUCTION_MAX_SPAN. */
    WorldDestroyCircle(&world, 171, 100, 35, 0.0f);
    CHECK(world.destructionCount == 2,
          "two cuts spanning %d cells were merged into %d region(s)",
          142, world.destructionCount);
    CHECK(world.destruction[0].maximumX - world.destruction[0].minimumX + 1 <=
              WORLD_DESTRUCTION_MAX_SPAN,
          "an entry outgrew the aggregation limit: %d cells",
          world.destruction[0].maximumX - world.destruction[0].minimumX + 1);
    WorldUnload(&world);
}

/* Damage recorded against a world that no longer exists would send the next
   check to coordinates now describing completely different terrain. */
static void test_regenerating_the_world_drops_its_damage_log(void)
{
    World world;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    FillRect(&world, 0, 40, 127, 95, MATERIAL_ROCK);
    WorldDestroyCircle(&world, 60, 60, 5, 0.0f);
    CHECK(world.destructionCount == 1, "the cut was not logged");

    WorldGenerate(&world, 0x11223344u);
    CHECK(world.destructionCount == 0,
          "a regenerated world kept %d damage regions",
          world.destructionCount);
    CHECK(world.destructionDropped == 0, "the overflow counter survived");
    WorldUnload(&world);
}

/* Repeated destruction, the way a player with a cooldown actually produces it.
   Nothing here may grow without bound: not the damage log, not the work per
   tick, not the bodies. */
static void test_repeated_destruction_stays_bounded(void)
{
    World world;
    int round;
    int worstChecksPerCall = 0;

    CHECK(WorldInit(&world, 400, 200), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    FillRect(&world, 0, 0, 399, 199, MATERIAL_ROCK);

    for (round = 0; round < 40; ++round) {
        int before = detach.stats.detachChecks;
        int x = 40 + (round * 37) % 320;
        int y = 40 + (round * 53) % 120;

        WorldDestroyCircle(&world, x, y, 9, 0.0f);
        CHECK(world.destructionCount <= MAX_WORLD_DESTRUCTION_REGIONS,
              "the damage log holds %d regions", world.destructionCount);
        (void)RunDetach(&world, &terrain);
        CHECK(world.destructionCount == 0, "round %d left the log full", round);
        if (detach.stats.detachChecks - before > worstChecksPerCall) {
            worstChecksPerCall = detach.stats.detachChecks - before;
        }
        WorldUpdate(&world);
    }
    CHECK(worstChecksPerCall <= MAX_WORLD_DESTRUCTION_REGIONS *
                                    detach.config.maxCandidatesPerRegion,
          "one call ran %d checks, the bound is %d", worstChecksPerCall,
          MAX_WORLD_DESTRUCTION_REGIONS * detach.config.maxCandidatesPerRegion);
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies <= MAX_TERRAIN_BODIES,
          "the body manager overflowed");
    CHECK(DynamicTerrainStatistics(&terrain)->dynamicCellsUsed <=
              terrain.config.maxDynamicCells,
          "the cell budget was exceeded: %d",
          DynamicTerrainStatistics(&terrain)->dynamicCellsUsed);
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* The full path, through GameUpdate rather than around it. */
static void test_the_game_loop_detaches_terrain_by_itself(void)
{
    GameState game;
    GameConfig config = GameDefaultConfig();
    GameEventBuffer events;
    GameInput input = {0};
    int frame;
    bool sawEvent = false;

    config.worldWidth = 256;
    config.worldHeight = 128;
    config.seed = 0x5eedu;
    CHECK(GameInit(&game, config), "game allocation failed");
    /* The generated world would leave the block joined to the hillside behind
       it, so the scene is built in cleared ground. */
    FillRect(&game.world, 0, 0, game.world.width - 1, game.world.height - 1,
             MATERIAL_EMPTY);
    (void)BuildPillarScene(&game.world, 52, 69, 64, 69, 60, 80);
    /* Well away from the scene: a player resolving a collision inside the block
       would be a second author of the world state under test. */
    game.player.position = (Vector2){200.0f, 40.0f};

    WorldDestroyCircle(&game.world, 60, 75, 4, 0.0f);
    for (frame = 0; frame < 8; ++frame) {
        int index;

        GameUpdate(&game, &input, config.fixedStep, &events);
        for (index = 0; index < (int)events.count; ++index) {
            if (events.events[index].type == GAME_EVENT_TERRAIN_DETACHED) {
                sawEvent = true;
            }
        }
    }
    CHECK(game.detach.stats.autoDetachSucceeded == 1,
          "the game loop detached %d fragments",
          game.detach.stats.autoDetachSucceeded);
    CHECK(sawEvent, "no GAME_EVENT_TERRAIN_DETACHED reached presentation");
    CHECK(DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies == 1,
          "the game loop holds %d bodies",
          DynamicTerrainStatistics(&game.dynamicTerrain)->activeBodies);
    GameUnload(&game);
}

/* --- ability impulses on terrain bodies ---------------------------------- */

/* The physics here is the body's own: impulse divided by mass, torque from the
   lever arm divided by inertia. What these tests hold is that abilities feed it
   sensible numbers and that mass is never argued away. */

static TerrainImpulseSystem impulses;

/* A solid rock block whose mass and inertia come from the ordinary finalize
   path, so nothing below is measured against a hand-made body. */
static TerrainBodyHandle MakeRockBlock(DynamicTerrainSystem *system, int width,
                                       int height, Vector2 position)
{
    TerrainBodyHandle handle = DynamicTerrainAllocBody(system, width, height);

    FillBody(system, handle, 0, 0, width - 1, height - 1, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(system, handle);
    DynamicTerrainGet(system, handle)->position = position;
    return handle;
}

static void test_an_impulse_changes_velocity_by_impulse_over_mass(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;
    float mass;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeRockBlock(&terrain, 8, 8, (Vector2){50.0f, 50.0f});
    body = DynamicTerrainGetConst(&terrain, handle);
    mass = body->mass;
    CHECK(mass > 0.0f, "a finalized rock block has no mass");

    /* Straight through the centre of mass: all push, no spin. */
    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){mass * 3.0f, 0.0f},
                               body->position);
    CHECK(fabsf(body->velocity.x - 3.0f) < 0.001f,
          "impulse of 3*m gave %.4f cells/s instead of 3",
          (double)body->velocity.x);
    CHECK(fabsf(body->angularVelocity) < 0.0001f,
          "an impulse through the centre of mass span the body at %.5f rad/s",
          (double)body->angularVelocity);

    /* Twice the impulse, twice the change. */
    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){mass * 6.0f, 0.0f},
                               body->position);
    CHECK(fabsf(body->velocity.x - 9.0f) < 0.001f,
          "doubling the impulse gave %.4f instead of 9",
          (double)body->velocity.x);
    DynamicTerrainUnload(&terrain);
}

static void test_a_heavier_body_moves_less_under_the_same_impulse(void)
{
    TerrainBodyHandle light;
    TerrainBodyHandle heavy;
    const TerrainBody *lightBody;
    const TerrainBody *heavyBody;
    Vector2 impulse = {9000.0f, 0.0f};
    float ratio;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    light = MakeRockBlock(&terrain, 4, 4, (Vector2){40.0f, 40.0f});
    heavy = MakeRockBlock(&terrain, 16, 16, (Vector2){90.0f, 40.0f});
    lightBody = DynamicTerrainGetConst(&terrain, light);
    heavyBody = DynamicTerrainGetConst(&terrain, heavy);
    /* Sixteen times the cells, so sixteen times the mass. */
    CHECK(fabsf(heavyBody->mass / lightBody->mass - 16.0f) < 0.01f,
          "the heavy block is %.2f times the light one",
          (double)(heavyBody->mass / lightBody->mass));

    DynamicTerrainApplyImpulse(&terrain, light, impulse, lightBody->position);
    DynamicTerrainApplyImpulse(&terrain, heavy, impulse, heavyBody->position);
    ratio = lightBody->velocity.x / heavyBody->velocity.x;
    CHECK(fabsf(ratio - 16.0f) < 0.05f,
          "the light block moved %.2f times as fast, expected 16", (double)ratio);
    DynamicTerrainUnload(&terrain);
}

static void test_an_off_centre_impulse_spins_the_body(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;
    float above;
    float below;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeRockBlock(&terrain, 10, 10, (Vector2){60.0f, 60.0f});
    body = DynamicTerrainGetConst(&terrain, handle);

    /* Pushed sideways above the centre of mass. */
    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){4000.0f, 0.0f},
                               (Vector2){body->position.x,
                                         body->position.y - 4.0f});
    above = body->angularVelocity;
    CHECK(fabsf(above) > 0.001f, "an off-centre push produced no rotation");

    /* The mirror image below it must turn the other way, and by as much. */
    DynamicTerrainUnload(&terrain);
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = MakeRockBlock(&terrain, 10, 10, (Vector2){60.0f, 60.0f});
    body = DynamicTerrainGetConst(&terrain, handle);
    DynamicTerrainApplyImpulse(&terrain, handle, (Vector2){4000.0f, 0.0f},
                               (Vector2){body->position.x,
                                         body->position.y + 4.0f});
    below = body->angularVelocity;
    CHECK(above * below < 0.0f,
          "pushing above and below turned the body the same way: %.4f and %.4f",
          (double)above, (double)below);
    CHECK(fabsf(fabsf(above) - fabsf(below)) < 0.001f,
          "mirrored pushes gave |%.4f| and |%.4f|", (double)above,
          (double)below);
    DynamicTerrainUnload(&terrain);
}

static void test_a_body_with_more_inertia_spins_less(void)
{
    TerrainBodyHandle small;
    TerrainBodyHandle large;
    const TerrainBody *smallBody;
    const TerrainBody *largeBody;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    small = MakeRockBlock(&terrain, 6, 6, (Vector2){40.0f, 40.0f});
    large = MakeRockBlock(&terrain, 24, 24, (Vector2){100.0f, 40.0f});
    smallBody = DynamicTerrainGetConst(&terrain, small);
    largeBody = DynamicTerrainGetConst(&terrain, large);
    CHECK(largeBody->inertia > smallBody->inertia * 100.0f,
          "the large block's inertia is only %.1f times the small one's",
          (double)(largeBody->inertia / smallBody->inertia));

    /* Same impulse, same lever arm, so only inertia differs. */
    DynamicTerrainApplyImpulse(&terrain, small, (Vector2){5000.0f, 0.0f},
                               (Vector2){smallBody->position.x,
                                         smallBody->position.y - 2.0f});
    DynamicTerrainApplyImpulse(&terrain, large, (Vector2){5000.0f, 0.0f},
                               (Vector2){largeBody->position.x,
                                         largeBody->position.y - 2.0f});
    CHECK(fabsf(smallBody->angularVelocity) >
              fabsf(largeBody->angularVelocity) * 10.0f,
          "the small block span at %.4f and the large one at %.4f",
          (double)smallBody->angularVelocity,
          (double)largeBody->angularVelocity);
    DynamicTerrainUnload(&terrain);
}

/* --- blasts -------------------------------------------------------------- */

static TerrainBlast ExplosionBlast(Vector2 origin, float momentum)
{
    TerrainBlast blast = {TERRAIN_BLAST_RADIAL, origin, {0.0f, 0.0f},
                          ABILITY_EXPLOSION_SHOCK_RADIUS, 0.0f, momentum, 0.0f};

    return blast;
}

static TerrainBlast ForceBlast(Vector2 origin, Vector2 direction, float momentum)
{
    TerrainBlast blast = {TERRAIN_BLAST_CONE, origin, direction,
                          ABILITY_FORCE_LENGTH, ABILITY_FORCE_SPREAD_COSINE,
                          momentum, 0.0f};

    return blast;
}

static void test_an_explosion_throws_a_body_outward(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    /* Up and to the right of the blast. */
    handle = MakeRockBlock(&terrain, 6, 6, (Vector2){70.0f, 40.0f});
    body = DynamicTerrainGetConst(&terrain, handle);

    CHECK(TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast((Vector2){50.0f, 60.0f},
                                                  ABILITY_EXPLOSION_BODY_IMPULSE)),
          "the blast was refused");
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, NULL) == 1,
          "the blast reached %d bodies",
          impulses.stats.bodyImpulseApplications);
    CHECK(impulses.blastCount == 0, "the queue was not drained");

    CHECK(body->velocity.x > 0.0f && body->velocity.y < 0.0f,
          "the body was thrown to (%.2f, %.2f), expected up and right",
          (double)body->velocity.x, (double)body->velocity.y);
    CHECK(impulses.stats.bodiesAffectedByExplosion == 1,
          "the explosion counter reads %d",
          impulses.stats.bodiesAffectedByExplosion);
    DynamicTerrainUnload(&terrain);
}

static void test_a_body_outside_the_blast_radius_is_untouched(void)
{
    TerrainBodyHandle near;
    TerrainBodyHandle far;
    const TerrainBody *nearBody;
    const TerrainBody *farBody;
    Vector2 origin = {50.0f, 50.0f};
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    near = MakeRockBlock(&terrain, 5, 5, (Vector2){65.0f, 50.0f});
    far = MakeRockBlock(&terrain, 5, 5,
                        (Vector2){50.0f + ABILITY_EXPLOSION_SHOCK_RADIUS + 40.0f,
                                  50.0f});
    nearBody = DynamicTerrainGetConst(&terrain, near);
    farBody = DynamicTerrainGetConst(&terrain, far);
    /* Settled, so the blast's effect on it — none — is visible in its state and
       not hidden behind the awake flag every new body starts with. */
    for (index = 0; index < 64 && farBody->awake; ++index) {
        DynamicTerrainSettleBody(&terrain, DynamicTerrainGet(&terrain, far),
                                 KINEMATIC_STEP);
    }
    CHECK(!farBody->awake, "the far fixture would not settle");

    (void)TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast(origin,
                                                  ABILITY_EXPLOSION_BODY_IMPULSE));
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, NULL) == 1,
          "the blast reached the wrong number of bodies");
    CHECK(nearBody->velocity.x > 0.0f, "the near body was not thrown");
    CHECK(farBody->velocity.x == 0.0f && farBody->velocity.y == 0.0f &&
              farBody->angularVelocity == 0.0f,
          "a body outside the radius moved at (%.3f, %.3f)",
          (double)farBody->velocity.x, (double)farBody->velocity.y);
    CHECK(!farBody->awake, "a body outside the radius was woken");

    /* And again with a body that is awake, so the radius is what refuses it
       rather than the threshold that protects sleeping bodies. An awake body
       outside the reach must come out with exactly the motion it went in
       with — not slowed, and above all not pulled inward. */
    {
        TerrainBodyHandle moving = MakeRockBlock(&terrain, 5, 5,
                                                 (Vector2){origin.x +
                                                               ABILITY_EXPLOSION_SHOCK_RADIUS +
                                                               60.0f,
                                                           50.0f});
        const TerrainBody *movingBody = DynamicTerrainGetConst(&terrain, moving);

        DynamicTerrainSetVelocity(&terrain, moving, (Vector2){4.0f, 0.0f}, 0.25f);
        CHECK(movingBody->awake, "the moving fixture is not awake");
        (void)TerrainImpulseQueueBlast(&impulses,
                                       ExplosionBlast(origin,
                                                      ABILITY_EXPLOSION_BODY_IMPULSE));
        (void)TerrainImpulseApply(&impulses, &terrain, NULL, NULL);
        CHECK(movingBody->velocity.x == 4.0f && movingBody->velocity.y == 0.0f &&
                  movingBody->angularVelocity == 0.25f,
              "an awake body outside the radius was disturbed: (%.3f, %.3f, %.3f)",
              (double)movingBody->velocity.x, (double)movingBody->velocity.y,
              (double)movingBody->angularVelocity);
    }
    DynamicTerrainUnload(&terrain);
}

static void test_a_closer_body_is_thrown_harder(void)
{
    TerrainBodyHandle near;
    TerrainBodyHandle far;
    const TerrainBody *nearBody;
    const TerrainBody *farBody;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    /* Same size, same mass, different distance: falloff is the only variable. */
    near = MakeRockBlock(&terrain, 5, 5, (Vector2){60.0f, 50.0f});
    far = MakeRockBlock(&terrain, 5, 5, (Vector2){85.0f, 50.0f});
    nearBody = DynamicTerrainGetConst(&terrain, near);
    farBody = DynamicTerrainGetConst(&terrain, far);

    (void)TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast((Vector2){50.0f, 50.0f},
                                                  ABILITY_EXPLOSION_BODY_IMPULSE));
    (void)TerrainImpulseApply(&impulses, &terrain, NULL, NULL);
    CHECK(nearBody->velocity.x > farBody->velocity.x,
          "the near body reached %.2f and the far one %.2f",
          (double)nearBody->velocity.x, (double)farBody->velocity.x);
    CHECK(farBody->velocity.x > 0.0f, "the far body was not thrown at all");
    DynamicTerrainUnload(&terrain);
}

static void test_force_pushes_along_its_cone_and_spins_off_centre_bodies(void)
{
    TerrainBodyHandle ahead;
    TerrainBodyHandle aside;
    const TerrainBody *aheadBody;
    const TerrainBody *asideBody;
    Vector2 origin = {40.0f, 60.0f};
    Vector2 direction = {1.0f, 0.0f};

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    ahead = MakeRockBlock(&terrain, 6, 6, (Vector2){70.0f, 60.0f});
    /* Behind the blast, so outside the cone however close it is. */
    aside = MakeRockBlock(&terrain, 6, 6, (Vector2){14.0f, 60.0f});
    aheadBody = DynamicTerrainGetConst(&terrain, ahead);
    asideBody = DynamicTerrainGetConst(&terrain, aside);

    (void)TerrainImpulseQueueBlast(&impulses,
                                   ForceBlast(origin, direction,
                                              ABILITY_FORCE_BODY_IMPULSE));
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, NULL) == 1,
          "the cone reached %d bodies", impulses.stats.bodiesAffectedByForce);
    CHECK(aheadBody->velocity.x > 0.0f, "the body in front was not pushed");
    CHECK(asideBody->velocity.x == 0.0f && asideBody->velocity.y == 0.0f,
          "a body behind the cone moved at (%.3f, %.3f)",
          (double)asideBody->velocity.x, (double)asideBody->velocity.y);
    CHECK(impulses.stats.bodiesAffectedByForce == 1,
          "the force counter reads %d", impulses.stats.bodiesAffectedByForce);

    /* Off the cone's axis, so the blow lands away from the centre of mass. */
    DynamicTerrainUnload(&terrain);
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    ahead = MakeRockBlock(&terrain, 6, 6, (Vector2){70.0f, 52.0f});
    aheadBody = DynamicTerrainGetConst(&terrain, ahead);
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ForceBlast(origin, direction,
                                              ABILITY_FORCE_BODY_IMPULSE));
    (void)TerrainImpulseApply(&impulses, &terrain, NULL, NULL);
    CHECK(fabsf(aheadBody->angularVelocity) > 0.001f,
          "an off-axis blow produced no rotation: %.5f rad/s",
          (double)aheadBody->angularVelocity);
    DynamicTerrainUnload(&terrain);
}

static void test_force_is_stopped_by_terrain_in_the_way(void)
{
    World world;
    TerrainBodyHandle handle;
    const TerrainBody *body;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    handle = MakeRockBlock(&terrain, 6, 6, (Vector2){70.0f, 60.0f});
    body = DynamicTerrainGetConst(&terrain, handle);

    /* A wall between the blast and the body. */
    FillRect(&world, 55, 40, 56, 80, MATERIAL_ROCK);
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ForceBlast((Vector2){40.0f, 60.0f},
                                              (Vector2){1.0f, 0.0f},
                                              ABILITY_FORCE_BODY_IMPULSE));
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, &world) == 0,
          "the blow reached through a wall");
    CHECK(impulses.stats.bodiesOccluded == 1, "the occlusion was not counted");
    CHECK(body->velocity.x == 0.0f, "the shielded body moved");

    /* The same blow with the wall gone lands. */
    FillRect(&world, 55, 40, 56, 80, MATERIAL_EMPTY);
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ForceBlast((Vector2){40.0f, 60.0f},
                                              (Vector2){1.0f, 0.0f},
                                              ABILITY_FORCE_BODY_IMPULSE));
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, &world) == 1,
          "the same blow without the wall did nothing");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* Waking costs an awake slot and restarts the quiet spell, so it needs a
   reason: an impulse too weak to push the body past the speed the sleep rule
   calls "at rest" leaves it asleep. */
static void test_a_meaningful_impulse_wakes_a_sleeping_body(void)
{
    TerrainBodyHandle small;
    TerrainBodyHandle huge;
    const TerrainBody *smallBody;
    const TerrainBody *hugeBody;
    Vector2 origin = {50.0f, 50.0f};
    int step;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    small = MakeRockBlock(&terrain, 5, 5, (Vector2){58.0f, 50.0f});
    huge = MakeRockBlock(&terrain, 40, 40, (Vector2){58.0f, 50.0f});
    smallBody = DynamicTerrainGetConst(&terrain, small);
    hugeBody = DynamicTerrainGetConst(&terrain, huge);
    for (step = 0; step < 64 && (smallBody->awake || hugeBody->awake); ++step) {
        DynamicTerrainSettleBody(&terrain, DynamicTerrainGet(&terrain, small),
                                 KINEMATIC_STEP);
        DynamicTerrainSettleBody(&terrain, DynamicTerrainGet(&terrain, huge),
                                 KINEMATIC_STEP);
    }
    CHECK(!smallBody->awake && !hugeBody->awake, "the fixtures would not settle");

    /* Enough to move the small block well past the sleep speed and nowhere near
       enough for the block sixty-four times its mass. */
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast(origin, 600.0f));
    (void)TerrainImpulseApply(&impulses, &terrain, NULL, NULL);
    CHECK(smallBody->awake, "a meaningful impulse did not wake the small body");
    CHECK(!hugeBody->awake,
          "an impulse worth %.4f cells/s woke a huge sleeping body",
          (double)(600.0f / hugeBody->mass));
    CHECK(impulses.stats.bodiesLeftSleeping == 1,
          "the refusal to wake was not counted: %d",
          impulses.stats.bodiesLeftSleeping);

    /* A blast worth the same speed to the huge block does wake it. */
    (void)TerrainImpulseQueueBlast(
        &impulses,
        ExplosionBlast(origin, hugeBody->mass * terrain.config.linearSleepSpeed *
                                   4.0f));
    (void)TerrainImpulseApply(&impulses, &terrain, NULL, NULL);
    CHECK(hugeBody->awake, "a real blast did not wake the huge body");
    DynamicTerrainUnload(&terrain);
}

static void test_a_refused_blast_and_a_dead_body_are_safe(void)
{
    TerrainBodyHandle handle;
    TerrainBlast bad = ExplosionBlast((Vector2){10.0f, 10.0f}, 1000.0f);
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    handle = MakeRockBlock(&terrain, 4, 4, (Vector2){12.0f, 12.0f});

    bad.radius = 0.0f;
    CHECK(!TerrainImpulseQueueBlast(&impulses, bad), "a zero-radius blast queued");
    bad = ExplosionBlast((Vector2){10.0f, 10.0f}, 1000.0f);
    bad.momentum = -1.0f;
    CHECK(!TerrainImpulseQueueBlast(&impulses, bad), "a negative blast queued");
    bad = ExplosionBlast((Vector2){10.0f, 10.0f}, 1000.0f);
    bad.origin.x = 0.0f / 0.0f;
    CHECK(!TerrainImpulseQueueBlast(&impulses, bad), "a NaN blast queued");
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, NULL) == 0,
          "a refused blast was applied anyway");

    /* A blast exactly on the body's centre of mass must not normalise a zero
       vector, and the body must come out with a finite transform. */
    DynamicTerrainFreeBody(&terrain, handle);
    handle = MakeRockBlock(&terrain, 4, 4, (Vector2){30.0f, 30.0f});
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast(DynamicTerrainGetConst(
                                                      &terrain, handle)->position,
                                                  ABILITY_EXPLOSION_BODY_IMPULSE));
    (void)TerrainImpulseApply(&impulses, &terrain, NULL, NULL);
    {
        const TerrainBody *body = DynamicTerrainGetConst(&terrain, handle);

        CHECK(body->velocity.x == body->velocity.x &&
                  body->velocity.y == body->velocity.y &&
                  body->angularVelocity == body->angularVelocity,
              "a point-blank blast produced a non-finite velocity");
        CHECK(body->velocity.y < 0.0f,
              "a point-blank blast did not use the deterministic fallback: "
              "(%.3f, %.3f)", (double)body->velocity.x, (double)body->velocity.y);
    }

    /* Freed slots are skipped, and the queue survives an empty world. */
    DynamicTerrainFreeBody(&terrain, handle);
    for (index = 0; index < 3; ++index) {
        (void)TerrainImpulseQueueBlast(&impulses,
                                       ExplosionBlast((Vector2){30.0f, 30.0f},
                                                      1000.0f));
    }
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, NULL) == 0,
          "a blast found a body in an empty manager");
    CHECK(TerrainImpulseApply(NULL, &terrain, NULL, NULL) == 0, "a NULL system applied");
    DynamicTerrainUnload(&terrain);
}

static void test_a_blast_never_changes_the_static_world(void)
{
    World world;
    uint64_t before;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulses);
    FillRect(&world, 0, 70, 127, 95, MATERIAL_ROCK);
    FillRect(&world, 30, 40, 40, 50, MATERIAL_SAND);
    (void)MakeRockBlock(&terrain, 6, 6, (Vector2){60.0f, 50.0f});
    before = WorldDigest(&world);

    (void)TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast((Vector2){50.0f, 50.0f},
                                                  ABILITY_EXPLOSION_BODY_IMPULSE));
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ForceBlast((Vector2){50.0f, 50.0f},
                                              (Vector2){1.0f, 0.0f},
                                              ABILITY_FORCE_BODY_IMPULSE));
    (void)TerrainImpulseApply(&impulses, &terrain, NULL, &world);
    CHECK(WorldDigest(&world) == before, "applying impulses changed the world");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

static void test_blast_results_are_deterministic(void)
{
    DynamicTerrainSystem terrainA;
    DynamicTerrainSystem terrainB;
    TerrainImpulseSystem impulsesA;
    TerrainImpulseSystem impulsesB;
    int index;
    int slot;

    CHECK(DynamicTerrainInit(&terrainA) && DynamicTerrainInit(&terrainB),
          "dynamic terrain allocation failed");
    TerrainImpulseInit(&impulsesA);
    TerrainImpulseInit(&impulsesB);
    for (index = 0; index < 6; ++index) {
        Vector2 at = {30.0f + (float)index * 17.0f, 40.0f + (float)index * 5.0f};

        (void)MakeRockBlock(&terrainA, 4 + index, 4 + index, at);
        (void)MakeRockBlock(&terrainB, 4 + index, 4 + index, at);
    }
    for (index = 0; index < 3; ++index) {
        Vector2 origin = {40.0f + (float)index * 20.0f, 45.0f};

        (void)TerrainImpulseQueueBlast(&impulsesA,
                                       ExplosionBlast(origin, 12000.0f));
        (void)TerrainImpulseQueueBlast(&impulsesB,
                                       ExplosionBlast(origin, 12000.0f));
    }
    CHECK(TerrainImpulseApply(&impulsesA, &terrainA, NULL, NULL) ==
              TerrainImpulseApply(&impulsesB, &terrainB, NULL, NULL),
          "the same blasts reached a different number of bodies");

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *a = &terrainA.bodies[slot];
        const TerrainBody *b = &terrainB.bodies[slot];

        CHECK(a->active == b->active, "slot %d differs in occupancy", slot);
        if (!a->active) {
            continue;
        }
        CHECK(a->velocity.x == b->velocity.x && a->velocity.y == b->velocity.y &&
                  a->angularVelocity == b->angularVelocity,
              "slot %d ended at (%.6f, %.6f, %.6f) and (%.6f, %.6f, %.6f)", slot,
              (double)a->velocity.x, (double)a->velocity.y,
              (double)a->angularVelocity, (double)b->velocity.x,
              (double)b->velocity.y, (double)b->angularVelocity);
    }
    DynamicTerrainUnload(&terrainA);
    DynamicTerrainUnload(&terrainB);
}

/* The acceptance scenario, end to end: one explosion cuts a support, the
   fragment becomes a body, and the same explosion throws it. */
static void test_an_explosion_throws_the_fragment_it_just_freed(void)
{
    World world;
    const TerrainBody *body = NULL;
    int slot;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDetachInit(&detach);
    TerrainImpulseInit(&impulses);
    /* The block sits to one side of the pillar, so the blast lands off its
       centre of mass and the throw carries a spin. */
    (void)BuildPillarScene(&world, 46, 63, 64, 69, 60, 80);

    WorldDestroyCircle(&world, 60, 75, 4, 0.0f);
    /* Exactly the order the fixed step uses: detach, then the blast. */
    CHECK(RunDetach(&world, &terrain) == 1, "the block did not come loose");
    (void)TerrainImpulseQueueBlast(&impulses,
                                   ExplosionBlast((Vector2){60.5f, 75.5f},
                                                  ABILITY_EXPLOSION_BODY_IMPULSE));
    CHECK(TerrainImpulseApply(&impulses, &terrain, NULL, &world) == 1,
          "the blast missed the fragment it had just freed");

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (terrain.bodies[slot].active) {
            body = &terrain.bodies[slot];
        }
    }
    CHECK(body != NULL, "no body after the detach");
    CHECK(body->velocity.y < 0.0f,
          "the freed fragment was not thrown upward: %.3f",
          (double)body->velocity.y);
    CHECK(fabsf(body->angularVelocity) > 0.001f,
          "an off-centre blast gave the fragment no spin: %.5f",
          (double)body->angularVelocity);
    CHECK(body->awake, "the thrown fragment is asleep");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* The whole path, through GameUpdate: the explosion ability alone has to sever,
   detach and throw. */
static void test_the_explosion_ability_throws_terrain_by_itself(void)
{
    GameState game;
    GameConfig config = GameDefaultConfig();
    GameEventBuffer events;
    GameInput input = {0};
    int frame;
    const TerrainBody *body = NULL;
    int slot;

    config.worldWidth = 256;
    config.worldHeight = 128;
    config.seed = 0xb1a57u;
    CHECK(GameInit(&game, config), "game allocation failed");
    FillRect(&game.world, 0, 0, game.world.width - 1, game.world.height - 1,
             MATERIAL_EMPTY);
    /* The block stands well clear of the explosion's core radius: the point is
       that the blast severs the pillar and throws what falls, not that it blows
       the block apart. It is still inside the shockwave's reach. */
    (void)BuildPillarScene(&game.world, 46, 63, 40, 50, 60, 110);
    game.player.position = (Vector2){200.0f, 40.0f};

    for (frame = 0; frame < 6; ++frame) {
        memset(input.ability, 0, sizeof(input.ability));
        input.ability[ABILITY_EXPLOSION] = frame == 0;
        input.aimWorld = (Vector2){60.5f, 85.5f};
        GameUpdate(&game, &input, config.fixedStep, &events);
    }
    CHECK(game.detach.stats.autoDetachSucceeded == 1,
          "the ability detached %d fragments",
          game.detach.stats.autoDetachSucceeded);
    CHECK(game.impulses.stats.bodyImpulseApplications >= 1,
          "no impulse reached the fragment: %d applications",
          game.impulses.stats.bodyImpulseApplications);

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (game.dynamicTerrain.bodies[slot].active) {
            body = &game.dynamicTerrain.bodies[slot];
        }
    }
    CHECK(body != NULL, "the ability produced no body");
    CHECK(body->velocity.x == body->velocity.x &&
              body->velocity.y == body->velocity.y,
          "the thrown body has a non-finite velocity");
    GameUnload(&game);
}

/* --- carving and fracture ------------------------------------------------ */

static TerrainDamageSystem damage;

/* Mirrors the overlap test the interaction layer uses, so a test can ask "is
   the player inside this body" without reaching into that module's internals. */
static bool PlayerOverlapsBody(const DynamicTerrainSystem *system,
                               TerrainBodyHandle handle, Vector2 at, float radius)
{
    const TerrainBody *body = DynamicTerrainGetConst(system, handle);
    Vector2 local;
    int localY;

    if (body == NULL) {
        return false;
    }
    local = TerrainBodyWorldToLocal(body, at.x, at.y);
    for (localY = 0; localY < body->height; ++localY) {
        int localX;

        for (localX = 0; localX < body->width; ++localX) {
            float nearestX;
            float nearestY;
            float dx;
            float dy;

            if (DynamicTerrainCellAt(system, handle, localX, localY) ==
                MATERIAL_EMPTY) {
                continue;
            }
            nearestX = local.x < (float)localX ? (float)localX
                       : (local.x > (float)localX + 1.0f ? (float)localX + 1.0f
                                                         : local.x);
            nearestY = local.y < (float)localY ? (float)localY
                       : (local.y > (float)localY + 1.0f ? (float)localY + 1.0f
                                                         : local.y);
            dx = local.x - nearestX;
            dy = local.y - nearestY;
            if (dx * dx + dy * dy < radius * radius - 0.0001f) {
                return true;
            }
        }
    }
    return false;
}

static int LiveBodyCount(const DynamicTerrainSystem *system)
{
    int count = 0;
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (system->bodies[slot].active) {
            ++count;
        }
    }
    return count;
}

static void test_carving_a_body_removes_cells_and_bumps_its_revision(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;
    uint32_t revision;
    int before;
    int removed;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 12, 12, (Vector2){60.0f, 60.0f});
    body = DynamicTerrainGetConst(&terrain, handle);
    before = body->cellCount;
    revision = body->rasterRevision;

    removed = TerrainDamageCarveCircle(&damage, &terrain, handle,
                                       body->position, 3.0f);
    CHECK(removed > 0, "the carve removed nothing");
    CHECK(body->cellCount == before - removed,
          "the body holds %d cells after losing %d of %d", body->cellCount,
          removed, before);
    CHECK(body->rasterRevision != revision,
          "the raster changed without bumping its revision");
    CHECK(damage.stats.cellsCarved == removed, "the carve was not counted");

    /* A carve that lands on nothing must not pretend to have done anything. */
    revision = body->rasterRevision;
    CHECK(TerrainDamageCarveCircle(&damage, &terrain, handle,
                                   (Vector2){600.0f, 600.0f}, 3.0f) == 0,
          "a carve far from the body removed cells");
    CHECK(body->rasterRevision == revision, "an empty carve bumped the revision");
    DynamicTerrainUnload(&terrain);
}

/* The correction that makes carving usable: the centre of mass moves inside the
   raster, so the body has to move the other way or every surviving cell would
   be dragged across the world. */
static void test_carving_updates_mass_without_moving_what_is_left(void)
{
    TerrainBodyHandle handle;
    TerrainBody *body;
    Vector2 markBefore;
    Vector2 markAfter;
    Vector2 centreBefore;
    Vector2 positionBefore;
    float massBefore;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 14, 14, (Vector2){70.0f, 50.0f});
    body = DynamicTerrainGet(&terrain, handle);
    body->angle = 0.6f;
    /* Spinning, so the centre of mass is not the only thing that moves: the
       point it moves to was already travelling, and the body's velocity has to
       become that point's velocity. */
    body->angularVelocity = 2.0f;
    body->velocity = (Vector2){0.0f, 0.0f};
    massBefore = body->mass;
    centreBefore = body->centerOfMass;
    positionBefore = body->position;
    /* A cell far from the bite, whose world position must not change. */
    markBefore = TerrainBodyLocalToWorld(body, 0.5f, 0.5f);

    CHECK(TerrainDamageCarveCircle(&damage, &terrain, handle,
                                   TerrainBodyLocalToWorld(body, 12.5f, 12.5f),
                                   3.5f) > 0,
          "the corner carve removed nothing");
    CHECK(body->mass < massBefore, "mass did not fall after losing cells");
    CHECK(fabsf(body->centerOfMass.x - centreBefore.x) > 0.05f ||
              fabsf(body->centerOfMass.y - centreBefore.y) > 0.05f,
          "the centre of mass did not move after an off-centre bite");

    markAfter = TerrainBodyLocalToWorld(body, 0.5f, 0.5f);
    CHECK(fabsf(markAfter.x - markBefore.x) < 0.01f &&
              fabsf(markAfter.y - markBefore.y) < 0.01f,
          "a surviving cell moved from (%.3f, %.3f) to (%.3f, %.3f)",
          (double)markBefore.x, (double)markBefore.y, (double)markAfter.x,
          (double)markAfter.y);
    /* The body started at rest, so its velocity now has to be exactly the
       velocity of the point its centre of mass moved to — both components. The
       relation is between two things the test can see, not a copy of how the
       correction is written. */
    {
        float shiftX = body->position.x - positionBefore.x;
        float shiftY = body->position.y - positionBefore.y;

        CHECK(fabsf(shiftX) > 0.001f || fabsf(shiftY) > 0.001f,
              "the centre of mass did not move at all");
        CHECK(fabsf(body->velocity.x - (-2.0f * shiftY)) < 0.001f,
              "vx is %.4f, expected %.4f", (double)body->velocity.x,
              (double)(-2.0f * shiftY));
        CHECK(fabsf(body->velocity.y - (2.0f * shiftX)) < 0.001f,
              "vy is %.4f, expected %.4f", (double)body->velocity.y,
              (double)(2.0f * shiftX));
    }
    DynamicTerrainUnload(&terrain);
}

static void test_a_body_carved_away_entirely_is_freed(void)
{
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 5, 5, (Vector2){40.0f, 40.0f});

    CHECK(TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   DynamicTerrainGetConst(&terrain,
                                                          handle)->position,
                                   20.0f) == 25,
          "the carve did not take the whole body");
    CHECK(DynamicTerrainGetConst(&terrain, handle) == NULL,
          "an emptied body was not freed");
    CHECK(DynamicTerrainStatistics(&terrain)->activeBodies == 0,
          "the manager still holds %d bodies",
          DynamicTerrainStatistics(&terrain)->activeBodies);
    CHECK(damage.stats.bodiesEmptied == 1, "the emptied body was not counted");
    /* And every later call on the dead handle is a no-op rather than a crash. */
    CHECK(TerrainDamageCarveCircle(&damage, &terrain, handle,
                                   (Vector2){40.0f, 40.0f}, 3.0f) == 0,
          "carving a freed body did something");
    CHECK(TerrainDamageFracture(&damage, &terrain, handle) == 0,
          "fracturing a freed body did something");
    DynamicTerrainUnload(&terrain);
}

/* A cut across a slab leaves two pieces, and both stay exactly where they
   were. */
static void test_a_cut_through_a_slab_splits_it_in_two(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *original;
    const TerrainBody *piece = NULL;
    Vector2 leftMark;
    Vector2 rightMark;
    int slot;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 20, 8, (Vector2){80.0f, 50.0f});
    original = DynamicTerrainGetConst(&terrain, handle);
    leftMark = TerrainBodyLocalToWorld(original, 1.5f, 4.5f);
    rightMark = TerrainBodyLocalToWorld(original, 18.5f, 4.5f);

    /* Off centre on purpose: a cut down the middle leaves two halves of exactly
       the same size, and then which one keeps the slot stops being a decision
       anything could test. */
    CHECK(TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   TerrainBodyLocalToWorld(original, 7.0f, 4.0f),
                                   5.0f) > 0,
          "the cut removed nothing");
    CHECK(LiveBodyCount(&terrain) == 2, "the slab became %d bodies",
          LiveBodyCount(&terrain));
    CHECK(damage.stats.fractureSplits == 1, "the split was not counted");
    CHECK(damage.stats.fragmentsCreated == 1, "%d fragments were created",
          damage.stats.fragmentsCreated);

    /* The larger half kept the original slot, so the handle still names
       something the caller would recognise. */
    CHECK(original != NULL && original->active, "the original slot was released");
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (terrain.bodies[slot].active && &terrain.bodies[slot] != original) {
            piece = &terrain.bodies[slot];
        }
    }
    CHECK(piece != NULL, "no second body after the split");
    CHECK(original->cellCount > piece->cellCount,
          "the smaller half kept the slot: %d against %d", original->cellCount,
          piece->cellCount);

    /* Neither half moved. Local coordinates survive the split, so the same
       raster cell has to land on the same world point. */
    {
        Vector2 leftNow = TerrainBodyLocalToWorld(original, 1.5f, 4.5f);

        Vector2 rightNow = TerrainBodyLocalToWorld(piece, 18.5f, 4.5f);

        CHECK(fabsf(leftNow.x - leftMark.x) < 0.01f &&
                  fabsf(leftNow.y - leftMark.y) < 0.01f,
              "the kept half moved to (%.3f, %.3f) from (%.3f, %.3f)",
              (double)leftNow.x, (double)leftNow.y, (double)leftMark.x,
              (double)leftMark.y);
        CHECK(fabsf(rightNow.x - rightMark.x) < 0.01f &&
                  fabsf(rightNow.y - rightMark.y) < 0.01f,
              "the new half moved to (%.3f, %.3f) from (%.3f, %.3f)",
              (double)rightNow.x, (double)rightNow.y, (double)rightMark.x,
              (double)rightMark.y);
    }
    CHECK(piece->angle == original->angle, "the pieces disagree about rotation");
    DynamicTerrainUnload(&terrain);
}

/* A piece leaving a spinning body carries the speed the point it left from
   already had, or the halves would drift apart wrongly. */
static void test_a_split_piece_inherits_the_motion_of_where_it_was(void)
{
    TerrainBodyHandle handle;
    TerrainBody *original;
    const TerrainBody *piece = NULL;
    int slot;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 20, 8, (Vector2){80.0f, 50.0f});
    original = DynamicTerrainGet(&terrain, handle);
    original->velocity = (Vector2){5.0f, -2.0f};
    original->angularVelocity = 1.5f;

    CHECK(TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   TerrainBodyLocalToWorld(original, 10.0f, 4.0f),
                                   5.0f) > 0,
          "the cut removed nothing");
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (terrain.bodies[slot].active && &terrain.bodies[slot] != original) {
            piece = &terrain.bodies[slot];
        }
    }
    CHECK(piece != NULL, "no second body after the split");
    CHECK(piece->angularVelocity == 1.5f, "the piece spins at %.3f, not 1.5",
          (double)piece->angularVelocity);
    /* The right-hand piece sat to the right of the centre of a body turning
       clockwise, so it was already moving downward faster than the body as a
       whole, and it has to leave that way. */
    CHECK(piece->velocity.y > original->velocity.y,
          "the piece left at %.3f but the remainder is at %.3f",
          (double)piece->velocity.y, (double)original->velocity.y);
    DynamicTerrainUnload(&terrain);
}

static void test_fracture_is_deterministic(void)
{
    DynamicTerrainSystem first;
    DynamicTerrainSystem second;
    TerrainDamageSystem damageA;
    TerrainDamageSystem damageB;
    TerrainBodyHandle a;
    TerrainBodyHandle b;
    int slot;

    CHECK(DynamicTerrainInit(&first) && DynamicTerrainInit(&second),
          "dynamic terrain allocation failed");
    TerrainDamageInit(&damageA);
    TerrainDamageInit(&damageB);
    a = MakeRockBlock(&first, 24, 10, (Vector2){70.0f, 44.0f});
    b = MakeRockBlock(&second, 24, 10, (Vector2){70.0f, 44.0f});

    (void)TerrainDamageApplyCircle(&damageA, &first, a,
                                   TerrainBodyLocalToWorld(
                                       DynamicTerrainGetConst(&first, a),
                                       12.0f, 5.0f),
                                   6.0f);
    (void)TerrainDamageApplyCircle(&damageB, &second, b,
                                   TerrainBodyLocalToWorld(
                                       DynamicTerrainGetConst(&second, b),
                                       12.0f, 5.0f),
                                   6.0f);

    CHECK(damageA.stats.fragmentsCreated == damageB.stats.fragmentsCreated,
          "two identical cuts made %d and %d fragments",
          damageA.stats.fragmentsCreated, damageB.stats.fragmentsCreated);
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *left = &first.bodies[slot];
        const TerrainBody *right = &second.bodies[slot];

        CHECK(left->active == right->active, "slot %d differs in occupancy",
              slot);
        if (!left->active) {
            continue;
        }
        CHECK(left->cellCount == right->cellCount &&
                  left->position.x == right->position.x &&
                  left->position.y == right->position.y &&
                  left->mass == right->mass,
              "slot %d holds different bodies after the same cut", slot);
    }
    DynamicTerrainUnload(&first);
    DynamicTerrainUnload(&second);
}

/* No slot for a piece is not a reason to lose terrain: it stays part of the
   body it was already part of. */
static void test_fracture_without_a_free_slot_keeps_the_body_whole(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;
    int before;
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 20, 8, (Vector2){80.0f, 50.0f});
    body = DynamicTerrainGetConst(&terrain, handle);
    for (index = 1; index < MAX_TERRAIN_BODIES; ++index) {
        CHECK(DynamicTerrainGet(&terrain,
                                DynamicTerrainAllocBody(&terrain, 2, 2)) != NULL,
              "filling the manager failed at %d", index);
    }
    before = body->cellCount;

    CHECK(TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   TerrainBodyLocalToWorld(body, 10.0f, 4.0f),
                                   5.0f) > 0,
          "the cut removed nothing");
    CHECK(damage.stats.fragmentsRefusedByBudget == 1,
          "the refusal was not counted: %d",
          damage.stats.fragmentsRefusedByBudget);
    CHECK(damage.stats.fragmentsCreated == 0, "a fragment was created anyway");
    CHECK(body->cellCount > 0 && body->cellCount < before,
          "the body holds %d cells, was %d", body->cellCount, before);
    CHECK(LiveBodyCount(&terrain) == MAX_TERRAIN_BODIES,
          "the manager holds %d bodies", LiveBodyCount(&terrain));
    DynamicTerrainUnload(&terrain);
}

/* A cut can leave a chip that is not worth a body of its own. It is dropped,
   not spawned: a slab cut in half should not also litter the world with a
   dozen two-cell rocks, each holding a slot and a texture. */
static void test_a_chip_too_small_to_be_a_body_is_dropped(void)
{
    TerrainBodyHandle handle;
    const TerrainBody *body;
    int before;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = DynamicTerrainAllocBody(&terrain, 20, 10);
    /* A slab with a two-cell nub hanging off it by a single cell of neck. */
    FillBody(&terrain, handle, 0, 0, 15, 7, MATERIAL_ROCK, 20.0f);
    FillBody(&terrain, handle, 16, 4, 18, 4, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainGet(&terrain, handle)->position = (Vector2){70.0f, 50.0f};
    body = DynamicTerrainGetConst(&terrain, handle);
    before = body->cellCount;
    CHECK(before == 16 * 8 + 3, "the fixture holds %d cells", before);

    /* Exactly the neck. */
    CHECK(TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   TerrainBodyLocalToWorld(body, 16.5f, 4.5f),
                                   0.8f) == 1,
          "the cut did not take exactly the neck");
    CHECK(LiveBodyCount(&terrain) == 1, "a chip became a body: %d live",
          LiveBodyCount(&terrain));
    CHECK(damage.stats.fragmentsTooSmall == 1,
          "the dropped chip was not counted: %d",
          damage.stats.fragmentsTooSmall);
    CHECK(damage.stats.fragmentsCreated == 0, "a fragment was created anyway");
    /* And the chip is gone from the body it fell off, not left floating. */
    CHECK(body->cellCount == 16 * 8, "the body holds %d cells, expected %d",
          body->cellCount, 16 * 8);
    CHECK(DynamicTerrainCellAt(&terrain, handle, 17, 4) == MATERIAL_EMPTY,
          "the chip is still attached to the raster");
    DynamicTerrainUnload(&terrain);
}

/* --- player against bodies ----------------------------------------------- */

static TerrainInteractionSystem interaction;

static void test_the_player_cannot_stay_inside_a_body(void)
{
    Player player;
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    handle = MakeRockBlock(&terrain, 14, 14, (Vector2){60.0f, 60.0f});
    DynamicTerrainGet(&terrain, handle)->angle = 0.4f;

    /* Just clipping the body's left edge, the way one frame of movement leaves
       them. Placed through the transform rather than by eye, so the rotation is
       part of what is being tested rather than something the fixture dodges. */
    PlayerInit(&player, (Vector2){0.0f, 0.0f});
    player.position = TerrainBodyLocalToWorld(
        DynamicTerrainGetConst(&terrain, handle), -0.4f, 7.0f);
    CHECK(PlayerOverlapsBody(&terrain, handle, player.position, player.radius),
          "the fixture does not start overlapping");
    player.velocity = (Vector2){12.0f, 0.0f};

    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){200.0f, 200.0f}, false, KINEMATIC_STEP);
    CHECK(interaction.stats.contacts > 0, "the overlap was not detected");
    CHECK(!PlayerOverlapsBody(&terrain, handle, player.position, player.radius),
          "the player is still inside the body at (%.3f, %.3f)",
          (double)player.position.x, (double)player.position.y);
    /* Pushed away from the body, and slowed by hitting it. */
    CHECK(player.velocity.x < 12.0f, "the player was not slowed: %.3f",
          (double)player.velocity.x);
    DynamicTerrainUnload(&terrain);
}

static void test_a_small_body_is_shoved_more_easily_than_a_huge_one(void)
{
    Player player;
    TerrainBodyHandle small;
    TerrainBodyHandle huge;
    float smallSpeed;
    float hugeSpeed;
    float playerAfterSmall;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    small = MakeRockBlock(&terrain, 4, 4, (Vector2){60.0f, 60.0f});
    PlayerInit(&player, (Vector2){60.0f - 2.0f - player.radius, 60.0f});
    PlayerInit(&player, (Vector2){56.0f, 60.0f});
    player.velocity = (Vector2){40.0f, 0.0f};
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){200.0f, 200.0f}, false, KINEMATIC_STEP);
    smallSpeed = DynamicTerrainGetConst(&terrain, small)->velocity.x;
    playerAfterSmall = player.velocity.x;
    CHECK(smallSpeed > 0.0f, "the small body was not pushed: %.3f",
          (double)smallSpeed);

    DynamicTerrainUnload(&terrain);
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    huge = MakeRockBlock(&terrain, 32, 32, (Vector2){60.0f, 60.0f});
    PlayerInit(&player, (Vector2){44.0f, 60.0f});
    player.velocity = (Vector2){40.0f, 0.0f};
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){200.0f, 200.0f}, false, KINEMATIC_STEP);
    hugeSpeed = DynamicTerrainGetConst(&terrain, huge)->velocity.x;

    CHECK(smallSpeed > hugeSpeed * 4.0f,
          "the small body reached %.3f and the huge one %.3f",
          (double)smallSpeed, (double)hugeSpeed);
    /* And the player is stopped harder by the thing that would not move. */
    CHECK(player.velocity.x < playerAfterSmall,
          "the player left the huge body at %.3f and the small one at %.3f",
          (double)player.velocity.x, (double)playerAfterSmall);
    DynamicTerrainUnload(&terrain);
}

static void test_grab_only_takes_a_body_within_reach(void)
{
    Player player;
    TerrainBodyHandle near;
    TerrainBodyHandle far;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    PlayerInit(&player, (Vector2){50.0f, 50.0f});
    near = MakeRockBlock(&terrain, 5, 5, (Vector2){66.0f, 50.0f});
    far = MakeRockBlock(&terrain, 5, 5,
                        (Vector2){50.0f + interaction.config.grabDistance +
                                      60.0f, 50.0f});

    /* Aimed at the far one, which is out of reach. Nothing is taken — and in
       particular not the near body, which is within arm's reach but is plainly
       not what the player is pointing at. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             DynamicTerrainGetConst(&terrain, far)->position,
                             true, KINEMATIC_STEP);
    CHECK(!TerrainInteractionIsHolding(&interaction, &terrain),
          "a body out of reach was grabbed");
    CHECK(interaction.stats.grabs == 0, "an out-of-reach grab was counted");
    CHECK(DynamicTerrainGetConst(&terrain, far)->velocity.x == 0.0f &&
              DynamicTerrainGetConst(&terrain, near)->velocity.x == 0.0f,
          "a refused grab moved something");

    /* Released, then aimed at the near one. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){50.0f, 50.0f}, false, KINEMATIC_STEP);
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             DynamicTerrainGetConst(&terrain, near)->position,
                             true, KINEMATIC_STEP);
    CHECK(TerrainInteractionIsHolding(&interaction, &terrain),
          "a body within reach was not grabbed");
    CHECK(interaction.held.index == near.index &&
              interaction.held.generation == near.generation,
          "the hold landed on the wrong body");
    DynamicTerrainUnload(&terrain);
}

/* The hold pulls; it never places. A body that teleported to the cursor would
   pass through everything between here and there. */
static void test_a_held_body_is_pulled_rather_than_placed(void)
{
    Player player;
    TerrainBodyHandle handle;
    const TerrainBody *body;
    Vector2 start;
    Vector2 aim = {80.0f, 50.0f};
    int step;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    PlayerInit(&player, (Vector2){50.0f, 50.0f});
    handle = MakeRockBlock(&terrain, 5, 5, (Vector2){50.0f, 68.0f});
    body = DynamicTerrainGetConst(&terrain, handle);
    start = body->position;

    /* Taken hold of by pointing at it, then dragged by pointing elsewhere —
       which is how the control is meant to be used. */
    TerrainInteractionUpdate(&interaction, &player, &terrain, start, true,
                             KINEMATIC_STEP);
    CHECK(TerrainInteractionIsHolding(&interaction, &terrain), "no hold started");
    TerrainInteractionUpdate(&interaction, &player, &terrain, aim, true,
                             KINEMATIC_STEP);
    /* One step buys velocity, not arrival. */
    CHECK(body->position.x == start.x && body->position.y == start.y,
          "the body moved without being integrated — it was placed, not pulled");
    CHECK(fabsf(body->velocity.x) > 0.0f || fabsf(body->velocity.y) > 0.0f,
          "the hold applied no motion at all");

    /* Given time and integration it does arrive, and it arrives moving. */
    for (step = 0; step < 240; ++step) {
        TerrainInteractionUpdate(&interaction, &player, &terrain, aim, true,
                                 KINEMATIC_STEP);
        DynamicTerrainIntegrateBody(&terrain, DynamicTerrainGet(&terrain, handle),
                                    KINEMATIC_STEP);
    }
    CHECK(body->position.x > start.x + 5.0f,
          "the held body only reached %.2f from %.2f", (double)body->position.x,
          (double)start.x);
    DynamicTerrainUnload(&terrain);
}

static void test_releasing_throws_a_light_body_further_than_a_heavy_one(void)
{
    Player player;
    TerrainBodyHandle handle;
    Vector2 aim = {64.0f, 50.0f};
    float lightSpeed;
    float heavySpeed;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    PlayerInit(&player, (Vector2){50.0f, 50.0f});
    handle = MakeRockBlock(&terrain, 4, 4, (Vector2){64.0f, 50.0f});
    TerrainInteractionUpdate(&interaction, &player, &terrain, aim, true,
                             KINEMATIC_STEP);
    CHECK(TerrainInteractionIsHolding(&interaction, &terrain), "no hold started");
    DynamicTerrainGet(&terrain, handle)->velocity = (Vector2){0.0f, 0.0f};
    TerrainInteractionUpdate(&interaction, &player, &terrain, aim, false,
                             KINEMATIC_STEP);
    lightSpeed = DynamicTerrainGetConst(&terrain, handle)->velocity.x;
    CHECK(interaction.stats.throws == 1, "the throw was not counted");
    CHECK(lightSpeed > 0.0f, "the light body was not thrown: %.3f",
          (double)lightSpeed);

    DynamicTerrainUnload(&terrain);
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    handle = MakeRockBlock(&terrain, 16, 16, (Vector2){64.0f, 50.0f});
    TerrainInteractionUpdate(&interaction, &player, &terrain, aim, true,
                             KINEMATIC_STEP);
    DynamicTerrainGet(&terrain, handle)->velocity = (Vector2){0.0f, 0.0f};
    TerrainInteractionUpdate(&interaction, &player, &terrain, aim, false,
                             KINEMATIC_STEP);
    heavySpeed = DynamicTerrainGetConst(&terrain, handle)->velocity.x;

    CHECK(lightSpeed > heavySpeed * 4.0f,
          "the light body left at %.3f and the heavy one at %.3f",
          (double)lightSpeed, (double)heavySpeed);
    DynamicTerrainUnload(&terrain);
}

/* A hold cannot outlive the thing being held. */
static void test_a_hold_ends_safely_when_the_body_is_destroyed(void)
{
    Player player;
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    TerrainDamageInit(&damage);
    PlayerInit(&player, (Vector2){50.0f, 50.0f});
    handle = MakeRockBlock(&terrain, 5, 5, (Vector2){64.0f, 50.0f});
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){64.0f, 50.0f}, true, KINEMATIC_STEP);
    CHECK(TerrainInteractionIsHolding(&interaction, &terrain), "no hold started");

    (void)TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   (Vector2){64.0f, 50.0f}, 20.0f);
    CHECK(DynamicTerrainGetConst(&terrain, handle) == NULL,
          "the body survived being carved away");
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){64.0f, 50.0f}, true, KINEMATIC_STEP);
    CHECK(!TerrainInteractionIsHolding(&interaction, &terrain),
          "the hold survived the body");
    CHECK(interaction.stats.lostHolds == 1, "the lost hold was not counted");
    DynamicTerrainUnload(&terrain);
}

/* The whole chain the feature exists for, driven the way the game drives it. */
static void test_an_explosion_carves_and_splits_a_moving_body(void)
{
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    TerrainImpulseInit(&impulses);
    handle = MakeRockBlock(&terrain, 24, 7, (Vector2){70.0f, 50.0f});
    DynamicTerrainGet(&terrain, handle)->velocity = (Vector2){3.0f, 0.0f};

    (void)TerrainImpulseQueueBlast(&impulses, (TerrainBlast){
        TERRAIN_BLAST_RADIAL, {70.0f, 50.0f}, {0.0f, 0.0f},
        ABILITY_EXPLOSION_SHOCK_RADIUS, 0.0f, ABILITY_EXPLOSION_BODY_IMPULSE,
        ABILITY_EXPLOSION_BODY_CARVE});
    (void)TerrainImpulseApply(&impulses, &terrain, &damage, NULL);

    CHECK(impulses.stats.bodiesCarved >= 1, "the blast carved nothing");
    CHECK(LiveBodyCount(&terrain) == 2, "the blast left %d bodies",
          LiveBodyCount(&terrain));
    /* Both halves were thrown, not just the one that kept the slot. */
    CHECK(impulses.stats.bodyImpulseApplications == 2,
          "%d bodies were pushed", impulses.stats.bodyImpulseApplications);
    {
        int slot;

        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            const TerrainBody *body = &terrain.bodies[slot];

            if (!body->active) {
                continue;
            }
            CHECK(body->velocity.x == body->velocity.x &&
                      body->velocity.y == body->velocity.y,
                  "slot %d ended with a non-finite velocity", slot);
        }
    }
    DynamicTerrainUnload(&terrain);
}

/* A boosting player crosses ten cells in a frame. Testing only where they ended
   up would let them cross a thin slab and never touch it. */
static void test_a_fast_player_cannot_cross_a_thin_body(void)
{
    Player player;
    TerrainBodyHandle handle;
    Vector2 wallAt = {80.0f, 60.0f};
    int step;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    /* Two cells thick and tall enough that the player cannot go round it. */
    handle = DynamicTerrainAllocBody(&terrain, 2, 40);
    FillBody(&terrain, handle, 0, 0, 1, 39, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainGet(&terrain, handle)->position = wallAt;

    PlayerInit(&player, (Vector2){40.0f, 60.0f});
    /* Established as the starting point, so the next update has a path to walk
       rather than a first frame to take on trust. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){0.0f, 0.0f}, false, KINEMATIC_STEP);
    CHECK(player.position.x < wallAt.x, "the fixture starts on the wrong side");

    /* A whole boost-speed frame in one go: from well left of the slab to well
       right of it. */
    for (step = 0; step < 4; ++step) {
        player.position.x += 26.0f;
        player.velocity = (Vector2){620.0f, 0.0f};
        TerrainInteractionUpdate(&interaction, &player, &terrain,
                                 (Vector2){0.0f, 0.0f}, false, KINEMATIC_STEP);
    }
    CHECK(interaction.stats.contacts > 0,
          "the player crossed the slab without touching it");
    CHECK(interaction.stats.sweptFrames > 0, "the movement was not swept");
    CHECK(player.position.x < wallAt.x + 2.0f,
          "the player ended at %.2f, on the far side of a slab at %.2f",
          (double)player.position.x, (double)wallAt.x);
    DynamicTerrainUnload(&terrain);
}

/* The beam burns what it reaches and nothing behind it. */
static void test_the_laser_burns_only_the_first_thing_it_reaches(void)
{
    World world;
    TerrainBodyHandle handle;
    uint64_t before;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events;
    bool requested[ABILITY_COUNT];
    int index;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    AbilitiesInit(&abilities, 0x51u);
    ParticlesInit(&particles, 0x52u);
    GameEventsClear(&events);
    for (index = 0; index < ABILITY_COUNT; ++index) {
        requested[index] = false;
    }
    requested[ABILITY_LASER] = true;

    /* player -> body -> wall. The wall must come through untouched.
       The body is wide enough that the beam cannot bore all the way through it
       in the frames below: once it does, reaching the wall is correct, and the
       test would be measuring the wrong thing. */
    FillRect(&world, 80, 40, 82, 80, MATERIAL_ROCK);
    handle = MakeRockBlock(&terrain, 16, 16, (Vector2){50.0f, 60.0f});
    before = WorldDigest(&world);
    for (index = 0; index < 12; ++index) {
        AbilitiesUpdate(&abilities, &world, &terrain, &damage, NULL, &particles,
                        &events, (Vector2){20.0f, 60.0f},
                        (Vector2){120.0f, 60.0f}, KINEMATIC_STEP, requested);
    }
    CHECK(damage.stats.cellsCarved > 0, "the beam did not cut the body");
    CHECK(WorldDigest(&world) == before,
          "the wall behind the body was burned anyway");

    /* player -> wall -> body. Now the body is the one that must be spared. */
    DynamicTerrainUnload(&terrain);
    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainDamageInit(&damage);
    handle = MakeRockBlock(&terrain, 6, 6, (Vector2){100.0f, 60.0f});
    for (index = 0; index < 12; ++index) {
        AbilitiesUpdate(&abilities, &world, &terrain, &damage, NULL, &particles,
                        &events, (Vector2){20.0f, 60.0f},
                        (Vector2){120.0f, 60.0f}, KINEMATIC_STEP, requested);
    }
    CHECK(damage.stats.cellsCarved == 0,
          "the beam cut a body standing behind a wall: %d cells",
          damage.stats.cellsCarved);
    CHECK(DynamicTerrainGetConst(&terrain, handle)->cellCount == 36,
          "the shielded body lost cells");
    WorldUnload(&world);
    DynamicTerrainUnload(&terrain);
}

/* A segment can clip the corner of a rotated cell over a chord far shorter than
   the cell is wide. A whole-cell march steps straight over it. */
static void test_the_body_raycast_finds_a_thin_rotated_body(void)
{
    TerrainBodyHandle handle;
    TerrainBodyHandle hit;
    Vector2 at;
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 1, 24);
    FillBody(&terrain, handle, 0, 0, 0, 23, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainGet(&terrain, handle)->position = (Vector2){60.0f, 60.0f};

    /* Turned through a range of angles: at every one of them a beam straight
       through the middle has to find the blade. */
    for (index = 0; index < 16; ++index) {
        DynamicTerrainGet(&terrain, handle)->angle =
            (float)index / 16.0f * PI - PI * 0.5f;
        CHECK(DynamicTerrainRaycast(&terrain, (Vector2){20.0f, 60.0f},
                                    (Vector2){120.0f, 60.0f}, &hit, &at),
              "the beam missed a blade at angle %.3f",
              (double)DynamicTerrainGetConst(&terrain, handle)->angle);
        CHECK(hit.index == handle.index && hit.generation == handle.generation,
              "the beam hit the wrong body");
        CHECK(fabsf(at.y - 60.0f) < 1.5f, "the hit is off the beam: %.3f",
              (double)at.y);
    }

    /* And a beam that genuinely misses still reports a miss. */
    CHECK(!DynamicTerrainRaycast(&terrain, (Vector2){20.0f, 5.0f},
                                 (Vector2){120.0f, 5.0f}, &hit, &at),
          "a beam nowhere near the body reported a hit");
    DynamicTerrainUnload(&terrain);
}

/* The property the march's step size actually has to satisfy: it must never
   step over a piece of material longer than the step itself.

   One cell turned forty-five degrees is a diamond, and a beam crossing it off
   centre passes through a chord much shorter than the cell is wide. Every
   offset below leaves a chord of at least half a cell, which no march worth
   the name may miss. */
static void test_the_raycast_never_steps_over_material(void)
{
    TerrainBodyHandle handle;
    TerrainBodyHandle hit;
    Vector2 at;
    Vector2 centre = {60.0f, 60.0f};
    int index;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    handle = DynamicTerrainAllocBody(&terrain, 1, 1);
    FillBody(&terrain, handle, 0, 0, 0, 0, MATERIAL_ROCK, 20.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainGet(&terrain, handle)->position = centre;
    DynamicTerrainGet(&terrain, handle)->angle = PI * 0.25f;

    /* Swept across the diamond and, for each offset, across where the march
       happens to start. Whether a coarse march lands inside a short chord is a
       question of phase as much as of length, so a single start position can
       flatter a step size that is really too big. */
    for (index = 0; index <= 30; ++index) {
        float offset = -0.45f + (float)index * 0.03f;
        int phase;

        for (phase = 0; phase < 8; ++phase) {
            Vector2 from = {20.0f + (float)phase * 0.125f, centre.y + offset};

            CHECK(DynamicTerrainRaycast(&terrain, from,
                                        (Vector2){100.0f, centre.y + offset},
                                        &hit, &at),
                  "the march stepped over material at offset %.3f, phase %d",
                  (double)offset, phase);
        }
    }
    /* Well clear of the diamond, it correctly finds nothing. */
    CHECK(!DynamicTerrainRaycast(&terrain, (Vector2){20.0f, centre.y + 1.2f},
                                 (Vector2){100.0f, centre.y + 1.2f}, &hit, &at),
          "the march found material outside the rotated cell");
    DynamicTerrainUnload(&terrain);
}

/* The aim can cross a body's bounding box through a hole in it. A hold anchored
   there would put the spring on nothing. */
static void test_a_grab_never_lands_on_empty_raster(void)
{
    Player player;
    TerrainBodyHandle handle;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    PlayerInit(&player, (Vector2){50.0f, 60.0f});
    /* A ring: its bounding box is full, its middle is not. */
    handle = DynamicTerrainAllocBody(&terrain, 16, 16);
    FillBody(&terrain, handle, 0, 0, 15, 15, MATERIAL_ROCK, 20.0f);
    FillBody(&terrain, handle, 5, 5, 10, 10, MATERIAL_EMPTY, 0.0f);
    DynamicTerrainFinalizeBody(&terrain, handle);
    DynamicTerrainGet(&terrain, handle)->position = (Vector2){64.0f, 60.0f};

    /* Aimed straight at the hole. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             DynamicTerrainGetConst(&terrain, handle)->position,
                             true, KINEMATIC_STEP);
    if (TerrainInteractionIsHolding(&interaction, &terrain)) {
        int cellX = (int)floorf(interaction.holdLocalPoint.x);
        int cellY = (int)floorf(interaction.holdLocalPoint.y);

        CHECK(DynamicTerrainCellAt(&terrain, interaction.held, cellX, cellY) !=
                  MATERIAL_EMPTY,
              "the hold landed on an empty cell at (%d, %d)", cellX, cellY);
    }
    /* Aimed at the rim, it takes hold of the rim. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){64.0f, 60.0f}, false, KINEMATIC_STEP);
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             TerrainBodyLocalToWorld(
                                 DynamicTerrainGetConst(&terrain, handle), 0.5f,
                                 8.5f),
                             true, KINEMATIC_STEP);
    CHECK(TerrainInteractionIsHolding(&interaction, &terrain),
          "the rim could not be grabbed");
    CHECK(DynamicTerrainCellAt(&terrain, interaction.held,
                               (int)floorf(interaction.holdLocalPoint.x),
                               (int)floorf(interaction.holdLocalPoint.y)) !=
              MATERIAL_EMPTY,
          "the hold on the rim landed on an empty cell");
    DynamicTerrainUnload(&terrain);
}

/* Cutting off the side being held must never leave the spring anchored to a
   cell that has left the body. */
static void test_a_hold_follows_or_ends_when_its_side_is_cut_away(void)
{
    Player player;
    TerrainBodyHandle handle;
    Vector2 heldLocal;

    CHECK(DynamicTerrainInit(&terrain), "dynamic terrain allocation failed");
    TerrainInteractionInit(&interaction);
    TerrainDamageInit(&damage);
    PlayerInit(&player, (Vector2){50.0f, 60.0f});
    handle = MakeRockBlock(&terrain, 24, 8, (Vector2){62.0f, 60.0f});

    /* Held by the far right end, which the cut below will separate. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             TerrainBodyLocalToWorld(
                                 DynamicTerrainGetConst(&terrain, handle), 22.5f,
                                 4.0f),
                             true, KINEMATIC_STEP);
    CHECK(TerrainInteractionIsHolding(&interaction, &terrain), "no hold started");
    heldLocal = interaction.holdLocalPoint;
    CHECK(heldLocal.x > 18.0f, "the hold is not on the right-hand end: %.2f",
          (double)heldLocal.x);

    /* A cut nearer the right, so the held end is the smaller piece: it breaks
       away into a body of its own while the handle stays with the larger
       remainder. That is the case where a hold has to follow its cell or end. */
    CHECK(TerrainDamageApplyCircle(&damage, &terrain, handle,
                                   TerrainBodyLocalToWorld(
                                       DynamicTerrainGetConst(&terrain, handle),
                                       17.0f, 4.0f),
                                   5.0f) > 0,
          "the cut removed nothing");
    CHECK(LiveBodyCount(&terrain) == 2, "the slab became %d bodies",
          LiveBodyCount(&terrain));

    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){62.0f, 60.0f}, true, KINEMATIC_STEP);
    if (TerrainInteractionIsHolding(&interaction, &terrain)) {
        /* Followed the cell onto the piece that took it. */
        CHECK(DynamicTerrainCellAt(&terrain, interaction.held,
                                   (int)floorf(interaction.holdLocalPoint.x),
                                   (int)floorf(interaction.holdLocalPoint.y)) !=
                  MATERIAL_EMPTY,
              "the hold survived on an empty cell");
        CHECK(interaction.stats.transferredHolds == 1,
              "the transfer was not counted");
    } else {
        CHECK(interaction.stats.lostHolds == 1,
              "the hold ended without being counted");
    }
    /* Either way, one more update must be harmless. */
    TerrainInteractionUpdate(&interaction, &player, &terrain,
                             (Vector2){62.0f, 60.0f}, true, KINEMATIC_STEP);
    DynamicTerrainUnload(&terrain);
}

/* --- high-speed movement -------------------------------------------------- */

#define MOVEMENT_STEP (1.0f / 60.0f)

static float PlayerSpeed(const Player *player)
{
    return sqrtf(player->velocity.x * player->velocity.x +
                 player->velocity.y * player->velocity.y);
}

/* Flies `steps` frames with a fixed input, holding the player in place.

   The velocity model is what these tests are about, and letting the player
   actually travel would put them into the world's edge — which reads as rock —
   long before a boost has finished building. Pinning the position leaves the
   velocity untouched and keeps the world small. */
static void FlyPlayer(Player *player, World *world, Vector2 input, bool boost,
                      int steps)
{
    Vector2 at = player->position;
    int step;

    for (step = 0; step < steps; ++step) {
        PlayerUpdate(player, world, input, boost, MOVEMENT_STEP);
        player->position = at;
    }
}

static void test_thrust_builds_speed_rather_than_setting_it(void)
{
    World world;
    Player player;
    float first;
    float second;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){40.0f, 64.0f});

    PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, false, MOVEMENT_STEP);
    player.position = (Vector2){40.0f, 64.0f};
    first = PlayerSpeed(&player);
    CHECK(first > 0.0f && first < player.maxSpeed * 0.5f,
          "one frame of thrust reached %.2f of a %.2f limit", (double)first,
          (double)player.maxSpeed);

    FlyPlayer(&player, &world, (Vector2){1.0f, 0.0f}, false, 8);
    second = PlayerSpeed(&player);
    CHECK(second > first, "speed did not keep building: %.2f then %.2f",
          (double)first, (double)second);

    /* And it settles at the limit rather than overshooting it. */
    FlyPlayer(&player, &world, (Vector2){1.0f, 0.0f}, false, 240);
    CHECK(PlayerSpeed(&player) <= player.maxSpeed + 0.01f,
          "cruise speed %.2f exceeds the limit %.2f",
          (double)PlayerSpeed(&player), (double)player.maxSpeed);
    CHECK(PlayerSpeed(&player) > player.maxSpeed * 0.9f,
          "cruise speed %.2f never reached the limit %.2f",
          (double)PlayerSpeed(&player), (double)player.maxSpeed);
    WorldUnload(&world);
}

/* The one thing momentum has to guarantee: a reversal passes through zero
   rather than jumping across it. */
static void test_a_reversal_brakes_through_zero(void)
{
    World world;
    Player player;
    float top;
    int step;
    bool crossed = false;
    float previous;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){60.0f, 64.0f});
    FlyPlayer(&player, &world, (Vector2){1.0f, 0.0f}, true, 300);
    top = player.velocity.x;
    CHECK(top > player.boostStageOneSpeed,
          "the fixture never got up to speed: %.2f", (double)top);

    previous = top;
    for (step = 0; step < 300; ++step) {
        Vector2 at = player.position;

        PlayerUpdate(&player, &world, (Vector2){-1.0f, 0.0f}, false,
                     MOVEMENT_STEP);
        player.position = at;
        /* Never more than one frame's worth of braking in one frame, and never
           a sign flip that skips the middle. */
        CHECK(player.velocity.x <= previous + 0.001f,
              "velocity rose while braking: %.3f after %.3f",
              (double)player.velocity.x, (double)previous);
        if (previous > 0.0f && player.velocity.x <= 0.0f) {
            /* The turn has to happen at a standstill, not across one. Crossing
               from a fifth of the top speed straight into the negatives would
               be the instant flip momentum exists to forbid; crossing from
               almost nothing is simply what stopping looks like. */
            CHECK(previous < top * 0.05f,
                  "the reversal jumped from %.2f straight to %.2f, with a top "
                  "speed of %.2f", (double)previous, (double)player.velocity.x,
                  (double)top);
            crossed = true;
        }
        previous = player.velocity.x;
    }
    CHECK(crossed, "the player never turned around at all");
    CHECK(player.velocity.x < 0.0f, "the player did not end up going the other way");
    WorldUnload(&world);
}

/* Braking beats coasting: the same reversal input has to shed speed faster than
   simply letting go would. */
static void test_braking_is_stronger_than_letting_go(void)
{
    World world;
    Player braking;
    Player coasting;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&braking, (Vector2){60.0f, 64.0f});
    FlyPlayer(&braking, &world, (Vector2){1.0f, 0.0f}, true, 300);
    coasting = braking;

    /* One frame each. Comparing against a coasting twin isolates the input's
       own contribution: drag and the speed clamp act on both alike, so what is
       left between them is exactly what pressing back was worth. */
    FlyPlayer(&braking, &world, (Vector2){-1.0f, 0.0f}, false, 1);
    FlyPlayer(&coasting, &world, (Vector2){0.0f, 0.0f}, false, 1);
    {
        float fromInput = coasting.velocity.x - braking.velocity.x;
        float plainThrust = braking.acceleration * MOVEMENT_STEP;

        CHECK(fromInput > 0.0f, "pressing back did not slow the player");
        /* And it is worth more than an ordinary push, which is the whole point
           of braking authority: speed has to be sheddable faster than it was
           gained. */
        CHECK(fromInput > plainThrust * 1.5f,
              "braking was worth %.3f, barely more than a plain push of %.3f",
              (double)fromInput, (double)plainThrust);
    }

    /* Over a longer burn it does actually bring the player down. */
    FlyPlayer(&braking, &world, (Vector2){-1.0f, 0.0f}, false, 29);
    FlyPlayer(&coasting, &world, (Vector2){0.0f, 0.0f}, false, 29);
    CHECK(braking.velocity.x < coasting.velocity.x - 20.0f,
          "braking reached %.2f and coasting %.2f — the input barely mattered",
          (double)braking.velocity.x, (double)coasting.velocity.x);
    WorldUnload(&world);
}

/* Braking stops at a standstill. A brake strong enough to overshoot in one
   frame would read as an instant reversal, which is the one thing momentum is
   there to make impossible. */
static void test_braking_cannot_overshoot_into_reverse(void)
{
    World world;
    Player player;
    float budget;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){128.0f, 64.0f});
    /* Slower than one frame of braking is worth, so an unclamped brake would
       carry the velocity straight past zero. */
    budget = player.acceleration * player.brakingAuthority * MOVEMENT_STEP;
    player.velocity = (Vector2){budget * 0.4f, 0.0f};
    CHECK(player.velocity.x > 1.0f,
          "the fixture is too slow to exercise the decomposition: %.3f",
          (double)player.velocity.x);

    FlyPlayer(&player, &world, (Vector2){-1.0f, 0.0f}, false, 1);
    CHECK(player.velocity.x >= 0.0f,
          "one frame of braking threw the player into reverse at %.3f",
          (double)player.velocity.x);
    WorldUnload(&world);
}

/* Speed costs steering. It must cost some of it, and it must never cost all. */
static void test_steering_is_weaker_at_speed_but_never_gone(void)
{
    World world;
    Player slow;
    Player fast;
    float slowTurn;
    float fastTurn;

    CHECK(WorldInit(&world, 1024, 256), "world allocation failed");
    PlayerInit(&slow, (Vector2){60.0f, 128.0f});
    PlayerInit(&fast, (Vector2){60.0f, 128.0f});
    /* Both travelling right, one at cruise and one at the top of the boost. */
    FlyPlayer(&slow, &world, (Vector2){1.0f, 0.0f}, false, 240);
    FlyPlayer(&fast, &world, (Vector2){1.0f, 0.0f}, true, 400);
    CHECK(PlayerSpeed(&fast) > PlayerSpeed(&slow) * 3.0f,
          "the two fixtures are too close in speed: %.1f and %.1f",
          (double)PlayerSpeed(&slow), (double)PlayerSpeed(&fast));

    /* One frame of pure sideways thrust each. */
    slowTurn = slow.velocity.y;
    fastTurn = fast.velocity.y;
    FlyPlayer(&slow, &world, (Vector2){0.0f, -1.0f}, false, 1);
    FlyPlayer(&fast, &world, (Vector2){0.0f, -1.0f}, false, 1);
    slowTurn = slow.velocity.y - slowTurn;
    fastTurn = fast.velocity.y - fastTurn;

    CHECK(fastTurn < 0.0f, "the fast player could not steer at all: %.4f",
          (double)fastTurn);
    /* Substantially weaker, not weaker by a hair. The speed clamp shaves a
       fraction off a fast player's whole velocity vector every frame, which is
       enough to make a strict inequality pass even if steering never changed at
       all — so the margin has to be bigger than that effect. */
    CHECK(fabsf(fastTurn) < fabsf(slowTurn) * 0.6f,
          "steering did not weaken with speed: %.4f slow, %.4f fast",
          (double)slowTurn, (double)fastTurn);
    /* Never a total loss of control: the configured floor is a fraction, and a
       fraction is not zero. */
    CHECK(fabsf(fastTurn) > fabsf(slowTurn) * fast.turnAuthorityAtHighSpeed * 0.5f,
          "steering collapsed at speed: %.4f against %.4f",
          (double)fastTurn, (double)slowTurn);
    WorldUnload(&world);
}

static void test_boost_stages_are_reached_and_survive_release(void)
{
    World world;
    Player player;
    float top;

    CHECK(WorldInit(&world, 2048, 256), "world allocation failed");
    PlayerInit(&player, (Vector2){60.0f, 128.0f});

    FlyPlayer(&player, &world, (Vector2){1.0f, 0.0f}, true, 30);
    CHECK(player.boostStage == PLAYER_BOOST_STAGE_ONE, "stage one was not entered");
    FlyPlayer(&player, &world, (Vector2){1.0f, 0.0f}, true, 90);
    CHECK(player.boostStage == PLAYER_BOOST_STAGE_TWO, "stage two was not reached");
    FlyPlayer(&player, &world, (Vector2){1.0f, 0.0f}, true, 140);
    CHECK(player.boostStage == PLAYER_BOOST_STAGE_THREE,
          "stage three was not reached");
    top = PlayerSpeed(&player);
    CHECK(top > player.boostStageTwoSpeed,
          "stage three tops out at %.1f, below stage two's %.1f", (double)top,
          (double)player.boostStageTwoSpeed);

    /* Letting go drops the stage but not the momentum. */
    FlyPlayer(&player, &world, (Vector2){0.0f, 0.0f}, false, 1);
    CHECK(player.boostStage == PLAYER_BOOST_NONE, "the stage survived release");
    CHECK(PlayerSpeed(&player) > top * 0.9f,
          "releasing boost threw away the speed: %.1f of %.1f",
          (double)PlayerSpeed(&player), (double)top);

    /* And the excess bleeds off rather than vanishing. */
    FlyPlayer(&player, &world, (Vector2){0.0f, 0.0f}, false, 120);
    CHECK(PlayerSpeed(&player) < top * 0.6f,
          "the excess speed never bled off: %.1f of %.1f",
          (double)PlayerSpeed(&player), (double)top);
    WorldUnload(&world);
}

/* Not boosting means not drilling, so a wall is a wall however fast it is hit. */
static void test_a_fast_player_cannot_cross_a_thin_wall(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    /* Two cells thick, floor to ceiling. */
    FillRect(&world, 300, 0, 301, 127, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){60.0f, 64.0f});
    player.velocity = (Vector2){player.boostMaxSpeed, 0.0f};

    for (step = 0; step < 60; ++step) {
        /* No boost: the drill is what is allowed through a wall, and it is not
           running. */
        player.velocity.x = fmaxf(player.velocity.x, player.boostMaxSpeed);
        PlayerUpdate(&player, &world, (Vector2){0.0f, 0.0f}, false,
                     MOVEMENT_STEP);
        CHECK(player.position.x < 300.0f + player.radius + 1.0f,
              "the player reached x=%.2f, past a wall at 300",
              (double)player.position.x);
    }
    CHECK(player.impactStrength >= 0.0f, "impact strength went negative");
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == 2 * 128,
          "the wall lost cells to a player who was not drilling");
    WorldUnload(&world);
}

/* A diagonal cut is the case a purely horizontal or vertical drill test never
   exercises: the tunnel has to keep up with a trajectory that is neither. */
static void test_a_diagonal_drill_does_not_stall(void)
{
    World world;
    Player player;
    Vector2 start;
    int step;
    int drilled = 0;

    CHECK(WorldInit(&world, 400, 400), "world allocation failed");
    FillRect(&world, 40, 40, 360, 360, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 20.0f});
    start = player.position;

    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){0.7071f, 0.7071f}, true,
                     MOVEMENT_STEP);
        drilled += player.drilledCells;
        CHECK(player.drilledCells <= 4096,
              "one frame cut %d cells, which is not a bounded mutation",
              player.drilledCells);
    }
    CHECK(drilled > 0, "the diagonal drill never cut anything");
    /* Well inside the block, and moved along both axes rather than sliding
       along a face. */
    CHECK(player.position.x > start.x + 120.0f && player.position.y > start.y + 120.0f,
          "the diagonal drill stalled at (%.1f, %.1f)", (double)player.position.x,
          (double)player.position.y);
    CHECK(fabsf((player.position.x - start.x) - (player.position.y - start.y)) <
              60.0f,
          "the tunnel drifted off the diagonal: %.1f across, %.1f down",
          (double)(player.position.x - start.x),
          (double)(player.position.y - start.y));
    WorldUnload(&world);
}

static void test_the_same_flight_replays_identically(void)
{
    World first;
    World second;
    Player a;
    Player b;
    int step;

    CHECK(WorldInit(&first, 512, 256) && WorldInit(&second, 512, 256),
          "world allocation failed");
    FillRect(&first, 200, 100, 320, 200, MATERIAL_ROCK);
    FillRect(&second, 200, 100, 320, 200, MATERIAL_ROCK);
    PlayerInit(&a, (Vector2){60.0f, 150.0f});
    PlayerInit(&b, (Vector2){60.0f, 150.0f});

    for (step = 0; step < 300; ++step) {
        /* A trajectory with turns in it, so the decomposition is exercised and
           not just a straight line. */
        Vector2 input = {1.0f, step > 150 ? -0.6f : 0.4f};

        PlayerUpdate(&a, &first, input, step < 240, MOVEMENT_STEP);
        PlayerUpdate(&b, &second, input, step < 240, MOVEMENT_STEP);
    }
    CHECK(a.position.x == b.position.x && a.position.y == b.position.y,
          "two identical flights ended at (%.6f, %.6f) and (%.6f, %.6f)",
          (double)a.position.x, (double)a.position.y, (double)b.position.x,
          (double)b.position.y);
    CHECK(a.velocity.x == b.velocity.x && a.velocity.y == b.velocity.y,
          "two identical flights ended at different velocities");
    CHECK(a.boostStage == b.boostStage, "the two runs reached different stages");
    CHECK(WorldDigest(&first) == WorldDigest(&second),
          "two identical flights carved different tunnels");
    WorldUnload(&first);
    WorldUnload(&second);
}

/* Nothing a caller can do to the velocity may turn into a transform the rest of
   the game has to live with. */
static void test_movement_survives_an_absurd_velocity(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 0, 100, 255, 127, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){128.0f, 40.0f});

    player.velocity = (Vector2){1.0e7f, -1.0e7f};
    for (step = 0; step < 20; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     MOVEMENT_STEP);
        CHECK(player.position.x == player.position.x &&
                  player.position.y == player.position.y,
              "the position went non-finite at step %d", step);
        CHECK(player.velocity.x == player.velocity.x &&
                  player.velocity.y == player.velocity.y,
              "the velocity went non-finite at step %d", step);
        CHECK(player.position.x >= 0.0f && player.position.x <= 256.0f &&
                  player.position.y >= 0.0f && player.position.y <= 128.0f,
              "the player left the world at (%.2f, %.2f)",
              (double)player.position.x, (double)player.position.y);
    }
    CHECK(PlayerSpeed(&player) <= player.boostMaxSpeed + 1.0f,
          "an absurd velocity survived as %.1f", (double)PlayerSpeed(&player));
    WorldUnload(&world);
}

/* Moving through something costs speed in proportion to its density, which is
   the whole of the water transition this task asks for. */
static void test_entering_water_costs_speed(void)
{
    World world;
    Player dry;
    Player wet;

    CHECK(WorldInit(&world, 512, 256), "world allocation failed");
    PlayerInit(&dry, (Vector2){60.0f, 60.0f});
    PlayerInit(&wet, (Vector2){60.0f, 180.0f});
    /* Both pinned where they start, so "one of them is in the pool" stays true
       for the whole comparison. */
    FlyPlayer(&dry, &world, (Vector2){1.0f, 0.0f}, true, 300);
    FlyPlayer(&wet, &world, (Vector2){1.0f, 0.0f}, true, 300);
    CHECK(fabsf(PlayerSpeed(&dry) - PlayerSpeed(&wet)) < 0.01f,
          "the two fixtures did not start level");

    /* A pool in front of one of them only. */
    FillRect(&world, 0, 150, 511, 220, MATERIAL_WATER);
    FlyPlayer(&dry, &world, (Vector2){1.0f, 0.0f}, true, 30);
    FlyPlayer(&wet, &world, (Vector2){1.0f, 0.0f}, true, 30);

    CHECK(PlayerSpeed(&wet) < PlayerSpeed(&dry) * 0.85f,
          "water barely slowed the player: %.1f wet against %.1f dry",
          (double)PlayerSpeed(&wet), (double)PlayerSpeed(&dry));
    CHECK(PlayerSpeed(&wet) > 0.0f, "water stopped the player dead");
    WorldUnload(&world);
}

/* --- abilities ----------------------------------------------------------- */

static void test_ability_table_passes_its_own_validation(void)
{
    /* GameInit runs this too. It catches the mistake that matters: a one-shot
       power with no cooldown, which fires on every frame the button is held
       and so quietly becomes the held power it was not meant to be. */
    CHECK(AbilitiesValidate(), "the ability table failed validation");
}

static void test_a_one_shot_ability_respects_its_cooldown(void)
{
    World world;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events = {0};
    bool requested[ABILITY_COUNT] = {false};
    float cooldown = AbilityDefinitionAt(ABILITY_FORCE)->cooldown;
    int fired = 0;
    int step;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    AbilitiesInit(&abilities, 0xE6BEu);
    ParticlesInit(&particles, 0xE6BEu);
    requested[ABILITY_FORCE] = true;

    /* Half the cooldown's worth of frames, all of them asking to fire. */
    for (step = 0; step < 30; ++step) {
        GameEventsClear(&events);
        AbilitiesUpdate(&abilities, &world, NULL, NULL, NULL, &particles, &events,
                        (Vector2){20.0f, 35.0f}, (Vector2){150.0f, 35.0f},
                        cooldown * 0.5f, requested);
        if (AbilityStateAt(&abilities, ABILITY_FORCE)->triggered) {
            ++fired;
        }
    }
    CHECK(fired == 15, "force fired %d times in 30 frames of half-cooldown "
                       "steps instead of 15", fired);
    WorldUnload(&world);
}

static void test_a_held_ability_reports_one_start_per_hold(void)
{
    World world;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events = {0};
    bool requested[ABILITY_COUNT] = {false};
    int starts = 0;
    int activeFrames = 0;
    int step;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    FillRect(&world, 60, 30, 70, 40, MATERIAL_ROCK);
    AbilitiesInit(&abilities, 0xE6BEu);
    ParticlesInit(&particles, 0xE6BEu);

    for (step = 0; step < 12; ++step) {
        /* Held for the first six frames, released, then held again. */
        requested[ABILITY_LASER] = step < 6 || step >= 9;
        GameEventsClear(&events);
        AbilitiesUpdate(&abilities, &world, NULL, NULL, NULL, &particles, &events,
                        (Vector2){20.0f, 35.0f}, (Vector2){150.0f, 35.0f},
                        1.0f / 60.0f, requested);
        if (AbilityStateAt(&abilities, ABILITY_LASER)->triggered) {
            ++starts;
        }
        if (AbilityStateAt(&abilities, ABILITY_LASER)->active) {
            ++activeFrames;
        }
    }
    CHECK(activeFrames == 9, "a held beam ran on %d/9 requested frames",
          activeFrames);
    CHECK(starts == 2, "a beam held twice reported %d starts instead of 2",
          starts);
    WorldUnload(&world);
}

/* Every ability's knockback reaches the player through the event buffer, which
   is what lets the player module stay ignorant of which powers exist. */
static void test_ability_knockback_reaches_the_player_through_events(void)
{
    GameConfig config = GameDefaultConfig();
    GameState game;
    GameEventBuffer events = {0};
    GameInput input = {0};
    Vector2 before;

    config.worldWidth = 256;
    config.worldHeight = 144;
    config.activeRadiusX = 96.0f;
    config.activeRadiusY = 72.0f;
    config.seed = 0xE6BEu;
    CHECK(GameInit(&game, config), "game allocation failed");

    before = game.player.velocity;
    input.aimWorld = (Vector2){game.player.position.x + 40.0f,
                               game.player.position.y};
    input.ability[ABILITY_FORCE] = true;
    GameUpdate(&game, &input, config.fixedStep, &events);

    CHECK(HasGameEvent(&events, GAME_EVENT_FORCE),
          "the force ability published no event");
    CHECK(game.player.velocity.x < before.x - 50.0f,
          "force recoil did not push the player back: %.1f -> %.1f",
          before.x, game.player.velocity.x);
    GameUnload(&game);
}

/* --- material table ----------------------------------------------------- */

static void test_every_material_has_table_data(void)
{
    int material;

    for (material = 0; material < MATERIAL_COUNT; ++material) {
        const char *name = WorldMaterialName((CellMaterial)material);

        CHECK(name != NULL, "material %d has no name", material);
        CHECK(name[0] != '\0', "material %d has an empty name", material);
        /* A gap in the table is zero-filled, which reads back as the EMPTY
           entry: that is exactly the silent failure the table has to catch. */
        CHECK(material == MATERIAL_EMPTY || strcmp(name, "EMPTY") != 0,
              "material %d fell back to the EMPTY entry", material);
    }
}

static void test_material_table_passes_its_own_validation(void)
{
    /* WorldInit runs this too, so a table that fails here fails the whole game
       loudly rather than producing cells that misbehave in one rare path. */
    CHECK(MaterialsValidate(), "the material table failed validation");
}

/* The cryo rate used to be a switch statement that named four materials and
   gave everything else a default. Moving it into the table is only safe if the
   numbers that made cryo work are still the numbers the beam reads. */
static void test_every_material_carries_its_own_cryo_rate(void)
{
    int material;

    for (material = 0; material < MATERIAL_COUNT; ++material) {
        if (material == MATERIAL_EMPTY) {
            continue;
        }
        CHECK(MaterialAt((CellMaterial)material)->chillRate > 0.0f,
              "material %s cannot be chilled at all",
              WorldMaterialName((CellMaterial)material));
    }
    /* Lava relaxes back toward 900C at 8% of the gap per tick, which is about
       22 degrees a tick at its freezing point. A beam that does not clearly
       beat that finds an equilibrium above the threshold and never freezes. */
    CHECK(MaterialAt(MATERIAL_LAVA)->chillRate >
              MaterialAt(MATERIAL_WATER)->chillRate * 10.0f,
          "lava must be chilled far harder than water");
}

static void test_solidity_matches_what_blocks_the_player(void)
{
    CHECK(WorldMaterialIsSolid(MATERIAL_DIRT), "dirt must be solid");
    CHECK(WorldMaterialIsSolid(MATERIAL_ROCK), "rock must be solid");
    CHECK(WorldMaterialIsSolid(MATERIAL_SAND), "sand must be solid");
    CHECK(!WorldMaterialIsSolid(MATERIAL_EMPTY), "empty must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_WATER), "water must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_LAVA), "lava must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_STEAM), "steam must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_SMOKE), "smoke must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_FIRE), "fire must be passable");
    CHECK(!WorldMaterialIsSolid(MATERIAL_ASH), "ash must be passable");
}

/* --- movement ---------------------------------------------------------- */

static void test_sand_falls_to_the_floor_and_is_conserved(void)
{
    World world;
    int before;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 30, 4, 33, 11, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);
    Tick(&world, 300);

    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "sand count changed from %d to %d", before,
          CountMaterial(&world, MATERIAL_SAND));
    CHECK(CountMaterial(&world, MATERIAL_SAND) == 32, "expected 32 sand cells");
    CHECK(WorldGetCell(&world, 31, 47) == MATERIAL_SAND,
          "sand did not reach the floor");
    CHECK(WorldGetCell(&world, 31, 4) == MATERIAL_EMPTY,
          "sand did not leave its starting cell");
    WorldUnload(&world);
}

static void test_sand_falls_at_most_one_cell_per_tick(void)
{
    World world;
    int tick;

    CHECK(WorldInit(&world, 32, 64), "world allocation failed");
    WorldSetCell(&world, 16, 2, MATERIAL_SAND);

    /* A cell that has already moved this tick must not be simulated again, so
       a free-falling grain advances exactly one row per tick. */
    for (tick = 1; tick <= 20; ++tick) {
        WorldUpdate(&world);
        CHECK(WorldGetCell(&world, 16, 2 + tick) == MATERIAL_SAND,
              "after %d ticks the grain was not at row %d", tick, 2 + tick);
    }
    WorldUnload(&world);
}

static void test_water_spreads_sideways_and_is_conserved(void)
{
    World world;
    int before;
    int minimumX = 1 << 30;
    int maximumX = -1;
    int highest = 1 << 30;
    int x;
    int y;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 0, 47, 63, 47, MATERIAL_ROCK);
    FillRect(&world, 30, 30, 33, 40, MATERIAL_WATER);
    before = CountMaterial(&world, MATERIAL_WATER);
    Tick(&world, 400);

    CHECK(CountMaterial(&world, MATERIAL_WATER) == before,
          "water count changed from %d to %d", before,
          CountMaterial(&world, MATERIAL_WATER));

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) != MATERIAL_WATER) {
                continue;
            }
            if (x < minimumX) minimumX = x;
            if (x > maximumX) maximumX = x;
            if (y < highest) highest = y;
        }
    }

    /* The column was four cells wide and eleven tall; once it lands it must
       slump into a much wider, much shallower mound. */
    CHECK(maximumX - minimumX >= 20,
          "water spanned only x=[%d..%d]", minimumX, maximumX);
    CHECK(highest >= 40, "water piled up to row %d instead of slumping", highest);
    WorldUnload(&world);
}

/* --- thermal ----------------------------------------------------------- */

static void test_rock_becomes_lava_above_its_threshold(void)
{
    World world;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 16, 16, MATERIAL_ROCK);
    WorldSetTemperature(&world, 16, 16, 760.0f);
    WorldUpdate(&world);

    CHECK(WorldGetCell(&world, 16, 16) == MATERIAL_LAVA,
          "rock above 720C did not melt");
    WorldUnload(&world);
}

static void test_water_becomes_steam_above_its_threshold(void)
{
    World world;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    WorldSetCell(&world, 16, 16, MATERIAL_WATER);
    WorldSetTemperature(&world, 16, 16, 120.0f);
    WorldUpdate(&world);

    CHECK(WorldGetCell(&world, 16, 16) == MATERIAL_STEAM,
          "water above 108C did not boil");
    WorldUnload(&world);
}

static void test_water_and_lava_react_into_steam_and_rock(void)
{
    World world;
    bool reactionSeen = false;
    int tick;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    FillRect(&world, 0, 31, 31, 31, MATERIAL_ROCK);
    WorldSetCell(&world, 16, 20, MATERIAL_WATER);
    WorldSetCell(&world, 17, 20, MATERIAL_LAVA);

    for (tick = 0; tick < 60 && !reactionSeen; ++tick) {
        WorldUpdate(&world);
        reactionSeen = world.reactionCount > 0;
    }

    CHECK(reactionSeen, "no reaction event was published");
    CHECK(CountMaterial(&world, MATERIAL_STEAM) > 0, "no steam was produced");
    WorldUnload(&world);
}

static void test_one_fire_cell_cannot_consume_a_whole_dirt_field(void)
{
    /* The containment budget is the invariant that keeps fire from eating an
       unlimited connected dirt layer. Check it from several ignition points. */
    static const int ignitionX[] = {24, 10, 39, 24};
    static const int ignitionY[] = {16, 9, 23, 9};
    int probe;

    for (probe = 0; probe < 4; ++probe) {
        World world;
        int dirtBefore;
        int dirtAfter;

        CHECK(WorldInit(&world, 48, 32), "world allocation failed");
        FillRect(&world, 8, 8, 40, 24, MATERIAL_DIRT);
        dirtBefore = CountMaterial(&world, MATERIAL_DIRT);
        WorldSetCell(&world, ignitionX[probe], ignitionY[probe], MATERIAL_FIRE);
        Tick(&world, 400);
        dirtAfter = CountMaterial(&world, MATERIAL_DIRT);

        CHECK(dirtAfter > dirtBefore / 2,
              "ignition %d burned %d of %d dirt cells", probe,
              dirtBefore - dirtAfter, dirtBefore);
        CHECK(CountMaterial(&world, MATERIAL_FIRE) == 0,
              "ignition %d still burning after 400 ticks", probe);
        WorldUnload(&world);
    }
}

static void test_a_lava_pocket_cannot_consume_its_rock_lining(void)
{
    World world;
    int lavaBefore;
    int rockBefore;

    CHECK(WorldInit(&world, 128, 128), "world allocation failed");
    FillRect(&world, 0, 0, 127, 127, MATERIAL_ROCK);
    FillRect(&world, 40, 50, 79, 69, MATERIAL_LAVA);
    lavaBefore = CountMaterial(&world, MATERIAL_LAVA);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);

    /* Lava heats what it touches, but rock must asymptote below its melt point.
       Without that budget a single pocket turns the whole map to lava, exactly
       as an unbudgeted fire would burn every connected dirt cell. */
    Tick(&world, 3000);

    CHECK(CountMaterial(&world, MATERIAL_LAVA) == lavaBefore,
          "lava grew from %d to %d cells", lavaBefore,
          CountMaterial(&world, MATERIAL_LAVA));
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == rockBefore,
          "lava melted %d rock cells",
          rockBefore - CountMaterial(&world, MATERIAL_ROCK));
    WorldUnload(&world);
}

static void test_lava_still_ignites_dirt_it_touches(void)
{
    World world;
    bool burned = false;
    int tick;

    /* The melt budget must not make lava thermally inert: dirt ignites far
       below rock's threshold and still has to catch. */
    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_DIRT);
    FillRect(&world, 28, 28, 35, 35, MATERIAL_LAVA);

    for (tick = 0; tick < 600 && !burned; ++tick) {
        WorldUpdate(&world);
        burned = CountMaterial(&world, MATERIAL_FIRE) > 0 ||
                 CountMaterial(&world, MATERIAL_DIRT) < 64 * 64 - 64;
    }
    CHECK(burned, "lava never ignited the dirt around it");
    WorldUnload(&world);
}

static void test_settled_cells_sleep_but_wake_when_disturbed(void)
{
    World world;
    int settledChunks;

    CHECK(WorldInit(&world, 128, 128), "world allocation failed");
    FillRect(&world, 0, 100, 127, 127, MATERIAL_ROCK);
    FillRect(&world, 40, 90, 60, 99, MATERIAL_SAND);
    Tick(&world, 400);
    settledChunks = world.activeChunkCount;

    /* A pile that cannot move is not work: it must let its chunks sleep. */
    CHECK(settledChunks == 0, "settled sand kept %d chunks awake", settledChunks);

    /* ...but pulling the floor out from under it has to wake it again. */
    FillRect(&world, 40, 100, 60, 127, MATERIAL_EMPTY);
    WorldUpdate(&world);
    CHECK(world.activeChunkCount > 0, "removing the floor woke nothing");
    Tick(&world, 200);
    CHECK(WorldGetCell(&world, 50, 127) == MATERIAL_SAND,
          "sand did not fall into the space that opened below it");
    WorldUnload(&world);
}

/* --- drilling ---------------------------------------------------------- */

static void test_drill_removes_solids_and_leaves_liquids(void)
{
    World world;
    int waterBefore;
    int lavaBefore;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_ROCK);
    FillRect(&world, 28, 28, 35, 35, MATERIAL_WATER);
    FillRect(&world, 28, 36, 35, 40, MATERIAL_LAVA);
    waterBefore = CountMaterial(&world, MATERIAL_WATER);
    lavaBefore = CountMaterial(&world, MATERIAL_LAVA);

    CHECK(WorldDrillCircle(&world, 32, 32, 6) > 0, "drill removed nothing");
    CHECK(CountMaterial(&world, MATERIAL_WATER) == waterBefore,
          "drill destroyed %d water cells",
          waterBefore - CountMaterial(&world, MATERIAL_WATER));
    CHECK(CountMaterial(&world, MATERIAL_LAVA) == lavaBefore,
          "drill destroyed %d lava cells",
          lavaBefore - CountMaterial(&world, MATERIAL_LAVA));
    WorldUnload(&world);
}

static void test_drill_returns_the_number_of_cells_it_removed(void)
{
    World world;
    int solidBefore;
    int removed;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_ROCK);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK);
    removed = WorldDrillCircle(&world, 32, 32, 5);

    /* A fraction of the cut becomes ash instead of empty, so the removed count
       must match rock lost, not the empty cells created. */
    CHECK(CountMaterial(&world, MATERIAL_ROCK) == solidBefore - removed,
          "removed %d but rock fell by %d", removed,
          solidBefore - CountMaterial(&world, MATERIAL_ROCK));
    CHECK(WorldDrillCircle(&world, 32, 32, 5) == 0,
          "drilling the same hole twice removed cells again");
    WorldUnload(&world);
}

static void test_drill_cannot_breach_the_world_boundary(void)
{
    World world;
    int x;
    int y;

    CHECK(WorldInit(&world, 32, 32), "world allocation failed");
    FillRect(&world, 0, 0, 31, 31, MATERIAL_ROCK);
    /* Centred outside the grid, and straddling every edge. */
    WorldDrillCircle(&world, -8, 16, 6);
    WorldDrillCircle(&world, 40, 16, 6);
    WorldDrillCircle(&world, 16, -8, 6);
    WorldDrillCircle(&world, 16, 40, 6);
    WorldDrillCircle(&world, 0, 0, 4);

    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; ++x) {
            CHECK(WorldGetCell(&world, x, y) != MATERIAL_EMPTY ||
                      (x < 6 && y < 6),
                  "cell %d,%d was cleared from outside the grid", x, y);
        }
    }
    WorldUnload(&world);
}

static void test_drill_heat_cannot_ignite_dirt_or_boil_water(void)
{
    World world;
    int pass;

    CHECK(WorldInit(&world, 64, 64), "world allocation failed");
    FillRect(&world, 0, 0, 63, 63, MATERIAL_DIRT);
    FillRect(&world, 40, 20, 47, 44, MATERIAL_WATER);

    /* Repeated passes must not accumulate heat past the phase thresholds. */
    for (pass = 0; pass < 40; ++pass) {
        WorldDrillCircle(&world, 20 + pass % 8, 32, 5);
        WorldUpdate(&world);
    }
    Tick(&world, 60);

    CHECK(CountMaterial(&world, MATERIAL_FIRE) == 0,
          "drilling ignited %d dirt cells", CountMaterial(&world, MATERIAL_FIRE));
    CHECK(CountMaterial(&world, MATERIAL_STEAM) == 0,
          "drilling boiled %d water cells", CountMaterial(&world, MATERIAL_STEAM));
    WorldUnload(&world);
}

/* --- chunk scheduling -------------------------------------------------- */

static void test_a_settled_world_lets_chunks_sleep(void)
{
    World world;
    int totalChunks;

    CHECK(WorldInit(&world, 256, 256), "world allocation failed");
    FillRect(&world, 0, 200, 255, 255, MATERIAL_ROCK);
    Tick(&world, 120);
    totalChunks = world.chunkColumns * world.chunkRows;

    CHECK(world.activeChunkCount < totalChunks,
          "all %d chunks stayed awake in a static world", totalChunks);
    CHECK(WorldCountDynamicCells(&world) == 0,
          "static rock counted as %d dynamic cells",
          WorldCountDynamicCells(&world));
    WorldUnload(&world);
}

static void test_activity_wakes_only_a_local_neighbourhood(void)
{
    World world;

    CHECK(WorldInit(&world, 256, 256), "world allocation failed");
    Tick(&world, 8);
    CHECK(world.activeChunkCount == 0, "empty world kept %d chunks awake",
          world.activeChunkCount);

    WorldSetCell(&world, 128, 128, MATERIAL_SAND);
    WorldUpdate(&world);
    CHECK(world.activeChunkCount > 0, "a falling grain woke no chunk");
    CHECK(world.activeChunkCount <= 9,
          "one grain woke %d chunks", world.activeChunkCount);
    WorldUnload(&world);
}

static void test_tick_stats_report_structural_work(void)
{
    World world;

    CHECK(WorldInit(&world, 70, 50), "world allocation failed");
    WorldUpdate(&world);
    CHECK(world.lastTickStats.processedChunks == 0u,
          "empty world reported %u processed chunks",
          world.lastTickStats.processedChunks);
    CHECK(world.lastTickStats.processedCells == 0u,
          "empty world reported %llu processed cells",
          (unsigned long long)world.lastTickStats.processedCells);

    /* This cell is inside one full 32x32 chunk, away from every border. The
       counter describes cells visited by the scheduler, not cells that moved. */
    WorldSetCell(&world, 40, 10, MATERIAL_SAND);
    WorldUpdate(&world);
    CHECK(world.lastTickStats.processedChunks == 1u,
          "one local mutation processed %u chunks",
          world.lastTickStats.processedChunks);
    CHECK(world.lastTickStats.processedCells == 1024u,
          "one full chunk reported %llu processed cells",
          (unsigned long long)world.lastTickStats.processedCells);
    WorldUnload(&world);
}

static void test_generation_streams_only_requested_dynamic_regions(void)
{
    World world;
    int totalChunks;
    int firstRegionChunks;
    int chunkY;

    CHECK(WorldInit(&world, 512, 288), "world allocation failed");
    WorldGenerate(&world, 0xE6BEu);
    totalChunks = world.chunkColumns * world.chunkRows;

    /* Generated terrain has not interacted yet, so a huge map begins asleep.
       Streaming a region wakes only the chunks in it that actually contain
       sand, fluids, gases or heat. Actual public writes use a separate wake path. */
    CHECK(world.activeChunkCount == 0,
          "generation woke %d distant chunks before a region was requested",
          world.activeChunkCount);
    WorldActivateRegion(&world,
                        (Rectangle){0.0f, 0.0f, (float)world.width * 0.5f,
                                    (float)world.height});
    CHECK(world.activeChunkCount > 0,
          "streaming the first region woke no sand or fluids");
    firstRegionChunks = world.activeChunkCount;
    for (chunkY = 0; chunkY < world.chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = world.chunkColumns / 2; chunkX < world.chunkColumns;
             ++chunkX) {
            size_t index = (size_t)chunkY * (size_t)world.chunkColumns +
                           (size_t)chunkX;

            CHECK(world.activeChunks[index] == 0u,
                  "streaming the left half woke distant chunk %d,%d",
                  chunkX, chunkY);
        }
    }
    {
        int farX = world.width - 2;
        int farY = 2;
        size_t farIndex = (size_t)(farY / WORLD_CHUNK_SIZE) *
                              (size_t)world.chunkColumns +
                          (size_t)(farX / WORLD_CHUNK_SIZE);

        WorldSetCell(&world, farX, farY, MATERIAL_SAND);
        CHECK(world.activeChunks[farIndex] != 0u,
              "a real mutation outside the streamed region stayed asleep");
    }
    WorldActivateRegion(&world,
                        (Rectangle){0.0f, 0.0f, (float)world.width,
                                    (float)world.height});
    CHECK(world.activeChunkCount > firstRegionChunks,
          "streaming the second half found no additional dynamic chunks");
    CHECK(world.activeChunkCount < totalChunks / 2,
          "streaming woke %d of %d chunks instead of only dynamic regions",
          world.activeChunkCount, totalChunks);
    WorldUnload(&world);
}

/* --- player against the world ------------------------------------------ */

static void test_boosting_player_tunnels_through_rock(void)
{
    World world;
    Player player;
    int step;
    int drilled = 0;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 60, 40, 200, 90, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    for (step = 0; step < 120; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        drilled += player.drilledCells;
    }

    CHECK(drilled > 0, "the player never cut a cell");
    CHECK(player.position.x > 200.0f,
          "the player stalled at x=%.1f inside the rock", player.position.x);
    WorldUnload(&world);
}

static void test_boost_from_rest_bores_into_a_wall(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 128, 200), "world allocation failed");
    /* The player starts resting on solid ground, the way a spawn does, and
       boosts straight down into it. */
    FillRect(&world, 0, 40, 127, 199, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){64.0f, 32.0f});

    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){0.0f, 1.0f}, true, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);
    }

    /* Below the drill threshold the collision zeroes the blocked velocity every
       frame, so without a contact drill the speed can never climb to the
       threshold and the boost stalls on the surface forever. */
    CHECK(player.position.y > 120.0f,
          "the player never broke the surface, stopping at y=%.1f",
          player.position.y);
    WorldUnload(&world);
}

static void test_boosting_player_tunnels_through_sand(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 60, 40, 200, 90, MATERIAL_SAND);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    /* Sand is dynamic, so this has to run a whole frame the way main.c does:
       the drill cuts, the world settles into the fresh tunnel, and only then is
       embedding resolved. Driving PlayerUpdate alone would never see it. */
    for (step = 0; step < 240; ++step) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);
    }

    CHECK(player.position.x > 200.0f,
          "the player stalled at x=%.1f inside the sand", player.position.x);
    WorldUnload(&world);
}

static int CountActiveParticles(const ParticleSystem *particles)
{
    int i;
    int count = 0;

    for (i = 0; i < MAX_PARTICLES; ++i) {
        if (particles->particles[i].active) {
            ++count;
        }
    }
    return count;
}

static void test_cryo_does_not_destroy_solid_terrain(void)
{
    World world;
    int step;
    int solidBefore;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 0, 30, 95, 40, MATERIAL_ROCK);
    FillRect(&world, 0, 41, 95, 48, MATERIAL_DIRT);
    FillRect(&world, 0, 49, 95, 56, MATERIAL_SAND);
    solidBefore = CountMaterial(&world, MATERIAL_ROCK) +
                  CountMaterial(&world, MATERIAL_DIRT) +
                  CountMaterial(&world, MATERIAL_SAND);

    /* Cold is not a solvent. Rock, dirt and sand have no cooling transition, so
       chilling them far below ambient must do nothing but make them cold.
       Encoding "no transition" as a zero-filled field once made every one of
       them evaporate at 0C, and the cryo beam bored through the map. */
    for (step = 0; step < 240; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 5.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }

    CHECK(CountMaterial(&world, MATERIAL_ROCK) +
                  CountMaterial(&world, MATERIAL_DIRT) +
                  CountMaterial(&world, MATERIAL_SAND) ==
              solidBefore,
          "the cryo beam destroyed terrain: %d solid cells became %d", solidBefore,
          CountMaterial(&world, MATERIAL_ROCK) + CountMaterial(&world, MATERIAL_DIRT) +
              CountMaterial(&world, MATERIAL_SAND));
    CHECK(WorldGetTemperature(&world, 48, 31) < 0.0f,
          "the beam did not even chill the rock face it stopped against (%.1f C)",
          WorldGetTemperature(&world, 48, 31));
    WorldUnload(&world);
}

static int CountFilled(const World *world)
{
    int x;
    int y;
    int filled = 0;

    for (y = 0; y < world->height; ++y) {
        for (x = 0; x < world->width; ++x) {
            if (WorldGetCell(world, x, y) != MATERIAL_EMPTY) {
                ++filled;
            }
        }
    }
    return filled;
}

static void test_no_material_evaporates_when_merely_cooled(void)
{
    CellMaterial material;

    /* The whole-table version of the test above, and the one that catches the
       next material added without a deliberate cooling entry. Chilling may turn
       a material into a different one — water into ice, lava into rock, fire
       into smoke — but it must never make matter disappear. A few ticks only,
       so the lifetimes of fire, smoke and steam cannot confuse the count. */
    for (material = 0; material < MATERIAL_COUNT; ++material) {
        World world;
        int x;
        int y;
        int before;

        if (material == MATERIAL_EMPTY) {
            continue;
        }
        CHECK(WorldInit(&world, 32, 40), "world allocation failed");
        for (y = 10; y < 20; ++y) {
            for (x = 10; x < 20; ++x) {
                WorldSetCell(&world, x, y, material);
                WorldSetTemperature(&world, x, y, -260.0f);
            }
        }
        before = CountFilled(&world);
        Tick(&world, 4);
        CHECK(CountFilled(&world) == before,
              "%s lost cells when chilled: %d filled became %d",
              WorldMaterialName(material), before, CountFilled(&world));
        WorldUnload(&world);
    }
}

static void test_cryo_snuffs_fire_into_smoke(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 64, 48), "world allocation failed");
    FillRect(&world, 24, 24, 40, 30, MATERIAL_FIRE);

    for (step = 0; step < 60; ++step) {
        WorldApplyChill(&world, (Vector2){32.0f, 5.0f}, (Vector2){32.0f, 44.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_SMOKE) > 0,
          "chilled fire left no smoke behind");
    WorldUnload(&world);
}

static void test_cryo_freezes_water_into_standing_ice(void)
{
    World world;
    int step;
    int ice;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 20, 40, 76, 50, MATERIAL_WATER);

    for (step = 0; step < 60; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 20.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    ice = CountMaterial(&world, MATERIAL_ICE);
    CHECK(ice > 0, "the cryo beam froze nothing");
    CHECK(WorldMaterialIsSolid(MATERIAL_ICE), "ice must be solid to build with");

    /* Ice does not drift back on its own, so it is something the player can
       actually build with. */
    Tick(&world, 600);
    CHECK(CountMaterial(&world, MATERIAL_ICE) == ice,
          "undisturbed ice changed on its own: %d cells became %d", ice,
          CountMaterial(&world, MATERIAL_ICE));
    WorldUnload(&world);
}

static void test_heat_melts_ice_back_into_water(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 30, 40, 60, 46, MATERIAL_ICE);

    /* Stable is not permanent: anything warm has to undo it, or the player
       could seal the world shut with something nothing can remove. */
    for (step = 0; step < 60; ++step) {
        WorldApplyLaser(&world, (Vector2){45.0f, 20.0f}, (Vector2){45.0f, 60.0f},
                        2.25f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_WATER) > 0,
          "the laser never melted any ice");
    WorldUnload(&world);
}

static void test_cryo_settles_lava_back_into_rock(void)
{
    World world;
    int step;

    CHECK(WorldInit(&world, 96, 64), "world allocation failed");
    FillRect(&world, 30, 40, 66, 50, MATERIAL_LAVA);

    /* Lava relaxes back toward 900C every tick, so this only passes if the beam
       out-cools that relaxation rather than merely dipping the temperature. */
    for (step = 0; step < 120; ++step) {
        WorldApplyChill(&world, (Vector2){48.0f, 20.0f}, (Vector2){48.0f, 60.0f},
                        2.6f, 1.0f / 60.0f);
        WorldUpdate(&world);
    }
    CHECK(CountMaterial(&world, MATERIAL_ROCK) > 0,
          "sustained cryo never turned any lava into rock");
    WorldUnload(&world);
}

static void test_one_force_blast_throws_loose_material_far(void)
{
    World world;
    int before;
    int rightmost = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 160, 64), "world allocation failed");
    FillRect(&world, 30, 30, 40, 40, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);

    /* A single blow, not a stream: the whole point is that one press moves the
       world a long way. */
    WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                         62.0f, 0.82f, 34);

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND && x > rightmost) {
                rightmost = x;
            }
        }
    }

    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "the blast destroyed loose material: %d sand cells became %d", before,
          CountMaterial(&world, MATERIAL_SAND));
    CHECK(rightmost > 55, "one blast barely moved anything; rightmost sand at x=%d",
          rightmost);
    WorldUnload(&world);
}

static void test_configured_force_power_hits_far_and_hard(void)
{
    World world;
    AbilitySystem abilities;
    ParticleSystem particles;
    GameEventBuffer events = {0};
    bool requested[ABILITY_COUNT] = {false};
    const AbilityState *force;
    const GameEvent *forceEvent = NULL;
    int rightmost = 0;
    int activeParticles = 0;
    int before;
    uint16_t index;
    int x;
    int y;
    int i;

    CHECK(WorldInit(&world, 192, 72), "world allocation failed");
    FillRect(&world, 30, 30, 40, 40, MATERIAL_SAND);
    before = CountMaterial(&world, MATERIAL_SAND);
    AbilitiesInit(&abilities, 0xE6BEu);
    ParticlesInit(&particles, 0xE6BEu);

    requested[ABILITY_FORCE] = true;
    AbilitiesUpdate(&abilities, &world, NULL, NULL, NULL, &particles, &events,
                    (Vector2){20.0f, 35.0f}, (Vector2){150.0f, 35.0f},
                    1.0f / 60.0f, requested);
    force = AbilityStateAt(&abilities, ABILITY_FORCE);

    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND && x > rightmost) {
                rightmost = x;
            }
        }
    }
    for (i = 0; i < MAX_PARTICLES; ++i) {
        if (particles.particles[i].active) {
            ++activeParticles;
        }
    }
    for (index = 0u; index < events.count; ++index) {
        if (events.events[index].type == GAME_EVENT_FORCE) {
            forceEvent = &events.events[index];
        }
    }

    CHECK(force->triggered, "Q press did not fire the force ability");
    CHECK(CountMaterial(&world, MATERIAL_SAND) == before,
          "configured force destroyed loose cells");
    CHECK(rightmost > 76,
          "configured Q still reads as a weak shove; rightmost sand at x=%d",
          rightmost);
    CHECK(activeParticles >= 48,
          "configured Q spawned only %d burst particles", activeParticles);
    CHECK(forceEvent != NULL, "the force ability published no game event");
    /* The recoil now travels as the event's player impulse, so the player
       module never needs to know the force blast exists. */
    CHECK(forceEvent->playerImpulse.x <= -120.0f,
          "configured recoil is too small or points the wrong way: %.1f",
          forceEvent->playerImpulse.x);
    WorldUnload(&world);
}

static void test_force_blast_marks_solid_terrain_without_boring_through_it(void)
{
    World world;
    int before;
    int step;
    int remaining;
    int interior = 0;
    int x;
    int y;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 30, 20, 60, 50, MATERIAL_ROCK);
    before = CountMaterial(&world, MATERIAL_ROCK);

    for (step = 0; step < 20; ++step) {
        WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                             62.0f, 0.82f, 34);
        WorldUpdate(&world);
    }

    remaining = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(remaining < before, "the blast left no mark on the rock face at all");
    /* A dent, not a tunnel: the wall must still be a wall after twenty blows. */
    CHECK(remaining > before * 4 / 5,
          "the blast bored through solid terrain: %d rock cells became %d", before,
          remaining);

    /* Only the exposed face is scoured; the inside of the block is untouched. */
    for (y = 25; y <= 45; ++y) {
        for (x = 40; x <= 58; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_ROCK) {
                ++interior;
            }
        }
    }
    CHECK(interior == 21 * 19, "the blast hollowed out rock behind the surface");
    WorldUnload(&world);
}

static void test_force_blast_does_not_reach_behind_a_wall(void)
{
    World world;
    int step;
    int x;
    int y;
    int moved = 0;

    CHECK(WorldInit(&world, 160, 64), "world allocation failed");
    /* A wall across the cone, with loose sand sheltering behind it. */
    FillRect(&world, 50, 0, 54, 63, MATERIAL_ROCK);
    FillRect(&world, 70, 30, 80, 40, MATERIAL_SAND);

    for (step = 0; step < 10; ++step) {
        WorldApplyForceBlast(&world, (Vector2){20.0f, 35.0f}, (Vector2){1.0f, 0.0f},
                             62.0f, 0.82f, 34);
    }

    /* No settling ticks: any sand outside its original block can only have been
       thrown, and the wall should have absorbed the whole blow. */
    for (y = 0; y < world.height; ++y) {
        for (x = 0; x < world.width; ++x) {
            if (WorldGetCell(&world, x, y) == MATERIAL_SAND &&
                (x < 70 || x > 80 || y < 30 || y > 40)) {
                ++moved;
            }
        }
    }
    CHECK(moved == 0, "%d sheltered cells were thrown through solid rock", moved);
    WorldUnload(&world);
}

static void test_bouncing_particles_do_not_pass_through_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int inside = 0;
    int i;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 70, 0, 127, 63, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    /* Impact sparks fired straight at a rock wall from open air. */
    ParticlesSpawnImpact(&particles, (Vector2){50.0f, 32.0f},
                         (Vector2){1.0f, 0.0f}, 90.0f);
    CHECK(CountActiveParticles(&particles) > 0, "no particles were spawned");

    for (step = 0; step < 60; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
        for (i = 0; i < MAX_PARTICLES; ++i) {
            const Particle *particle = &particles.particles[i];

            if (particle->active && particle->position.x >= 71.0f) {
                ++inside;
            }
        }
    }

    CHECK(inside == 0, "%d particle samples ended up inside the rock", inside);
    WorldUnload(&world);
}

static void test_drill_debris_settles_as_ash_without_overwriting_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int rockBefore;
    int rockAfter;
    int ash;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 0, 40, 127, 63, MATERIAL_ROCK);
    rockBefore = CountMaterial(&world, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    for (step = 0; step < 40; ++step) {
        ParticlesSpawnDrillDebris(&particles, (Vector2){64.0f, 30.0f},
                                  (Vector2){0.0f, -120.0f}, 30);
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }
    for (step = 0; step < 120; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
    }

    ash = CountMaterial(&world, MATERIAL_ASH);
    rockAfter = CountMaterial(&world, MATERIAL_ROCK);
    CHECK(ash > 0, "debris never settled into the world");
    CHECK(rockAfter == rockBefore,
          "settling debris overwrote terrain: %d rock cells became %d",
          rockBefore, rockAfter);
    WorldUnload(&world);
}

static void test_passing_particles_ignore_terrain(void)
{
    World world;
    ParticleSystem particles;
    int step;
    int i;
    bool crossed = false;

    CHECK(WorldInit(&world, 128, 64), "world allocation failed");
    FillRect(&world, 0, 20, 127, 24, MATERIAL_ROCK);
    ParticlesInit(&particles, 0xE6BEu);

    /* Steam is a gas effect: a rock ceiling must not stop it, or reaction plumes
       would pile up against the lid of a pocket instead of drifting. */
    ParticlesSpawnSteam(&particles, (Vector2){64.0f, 40.0f});
    for (step = 0; step < 120 && !crossed; ++step) {
        ParticlesUpdate(&particles, &world, 1.0f / 60.0f);
        for (i = 0; i < MAX_PARTICLES; ++i) {
            const Particle *particle = &particles.particles[i];

            if (particle->active && particle->position.y < 20.0f) {
                crossed = true;
            }
        }
    }

    CHECK(crossed, "steam never rose past the rock ceiling");
    WorldUnload(&world);
}

static void test_drill_resistance_never_stalls_the_boost(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 40, 20, 255, 110, MATERIAL_ROCK);
    PlayerInit(&player, (Vector2){20.0f, 64.0f});

    for (step = 0; step < 200; ++step) {
        float speed;

        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true, 1.0f / 60.0f);
        speed = sqrtf(player.velocity.x * player.velocity.x +
                      player.velocity.y * player.velocity.y);
        /* Once inside solid terrain the drill must keep the player above its
           own threshold, otherwise a boost would bury itself. */
        if (player.position.x > 60.0f && player.position.x < 240.0f) {
            CHECK(speed >= player.drillSpeed,
                  "speed fell to %.1f at x=%.1f, below the %.1f drill threshold",
                  speed, player.position.x, player.drillSpeed);
        }
    }
    WorldUnload(&world);
}

static void test_normal_flight_keeps_a_hover_pose(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){48.0f, 64.0f});

    for (frame = 0; frame < 180; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, false,
                     1.0f / 120.0f);
    }

    CHECK(!player.boosting, "normal-flight test unexpectedly enabled boost");
    CHECK(player.velocity.x > player.maxSpeed * 0.95f,
          "normal flight never reached cruise speed: %.1f", player.velocity.x);
    CHECK(fabsf(player.leanAmount - 0.12f) < 0.02f,
          "normal flight stopped reading as a slight hover lean: %.3f",
          player.leanAmount);
    WorldUnload(&world);
}

static void test_boost_flight_reaches_a_head_first_pose(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 512, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){48.0f, 64.0f});

    for (frame = 0; frame < 180; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
    }

    CHECK(player.boosting, "boost-pose test never enabled boost");
    CHECK(player.leanAmount > 0.9f,
          "boost never reached its head-first pose: %.3f", player.leanAmount);
    WorldUnload(&world);
}

static void test_long_boost_climbs_three_stages_into_supersonic(void)
{
    World world;
    Player player;
    int expectedStage = 1;
    int transitions = 0;
    int stageThreeFrame = -1;
    int sonicFrame = -1;
    int frame;

    CHECK(WorldInit(&world, 4096, 128), "world allocation failed");
    PlayerInit(&player, (Vector2){128.0f, 64.0f});

    for (frame = 0; frame < 960 && sonicFrame < 0; ++frame) {
        float before = sqrtf(player.velocity.x * player.velocity.x +
                             player.velocity.y * player.velocity.y);
        float after;

        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
        after = sqrtf(player.velocity.x * player.velocity.x +
                      player.velocity.y * player.velocity.y);
        if (player.boostStageChanged != PLAYER_BOOST_NONE) {
            CHECK((int)player.boostStageChanged == expectedStage,
                  "boost skipped or reordered a stage: expected %d, got %d",
                  expectedStage, (int)player.boostStageChanged);
            if (player.boostStageChanged == PLAYER_BOOST_STAGE_TWO) {
                CHECK(after - before > 70.0f,
                      "stage II had no real kick: %.1f -> %.1f", before, after);
            } else if (player.boostStageChanged == PLAYER_BOOST_STAGE_THREE) {
                CHECK(after - before > 150.0f,
                      "stage III had no real kick: %.1f -> %.1f", before, after);
                stageThreeFrame = frame;
            }
            ++expectedStage;
            ++transitions;
        }
        if (player.boostStage == PLAYER_BOOST_STAGE_THREE &&
            after >= player.sonicSpeed) {
            sonicFrame = frame;
        }
    }

    CHECK(transitions == 3, "long boost emitted %d stage kicks instead of 3",
          transitions);
    CHECK(player.boostStage == PLAYER_BOOST_STAGE_THREE,
          "long boost stopped at stage %d", (int)player.boostStage);
    CHECK(sonicFrame >= 0, "stage III never crossed sonic speed %.1f",
          player.sonicSpeed);
    CHECK(sonicFrame - stageThreeFrame <= 30,
          "stage III needed %.2fs to reach sonic speed instead of one kick",
          (float)(sonicFrame - stageThreeFrame) / 120.0f);

    PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, false, 1.0f / 120.0f);
    CHECK(player.boostStage == PLAYER_BOOST_NONE,
          "releasing Shift left boost stage %d armed", (int)player.boostStage);
    WorldUnload(&world);
}

static void test_stalled_boost_cannot_charge_a_later_stage(void)
{
    World world;
    Player player;
    int frame;

    CHECK(WorldInit(&world, 128, 96), "world allocation failed");
    PlayerInit(&player, (Vector2){124.0f, 48.0f});

    /* The outside of the simulation is indestructible rock. Holding thrust into
       it exercises a long Shift press with neither sustained speed nor aligned
       travel; elapsed key time alone must never unlock Stage II. */
    for (frame = 0; frame < 600; ++frame) {
        PlayerUpdate(&player, &world, (Vector2){1.0f, 0.0f}, true,
                     1.0f / 120.0f);
    }

    CHECK(player.boostStage == PLAYER_BOOST_STAGE_ONE,
          "stalled boost charged stage %d at the world boundary",
          (int)player.boostStage);
    CHECK(player.boostStageTime < 0.01f,
          "stalled boost retained %.3fs of false stage progress",
          player.boostStageTime);
    WorldUnload(&world);
}

static void test_player_never_ends_a_frame_inside_solid_terrain(void)
{
    World world;
    Player player;
    int step;

    CHECK(WorldInit(&world, 256, 128), "world allocation failed");
    FillRect(&world, 0, 70, 255, 127, MATERIAL_ROCK);
    FillRect(&world, 90, 20, 130, 70, MATERIAL_DIRT);
    PlayerInit(&player, (Vector2){20.0f, 40.0f});

    for (step = 0; step < 400; ++step) {
        /* Sweep the input around so the player grinds along walls and corners
           rather than following one clean line. */
        float angle = (float)step * 0.11f;
        Vector2 input = {cosf(angle), sinf(angle)};
        int radius;
        int checkedX;
        int checkedY;
        bool embedded = false;

        PlayerUpdate(&player, &world, input, (step % 3) != 0, 1.0f / 60.0f);
        WorldUpdate(&world);
        PlayerResolveWorldCollision(&player, &world);

        radius = (int)player.radius;
        for (checkedY = -radius; checkedY <= radius && !embedded; ++checkedY) {
            for (checkedX = -radius; checkedX <= radius; ++checkedX) {
                int cellX = (int)player.position.x + checkedX;
                int cellY = (int)player.position.y + checkedY;

                if (checkedX * checkedX + checkedY * checkedY > radius * radius) {
                    continue;
                }
                if (WorldMaterialIsSolid(WorldGetCell(&world, cellX, cellY))) {
                    embedded = true;
                    break;
                }
            }
        }
        CHECK(!embedded, "player embedded in solid terrain at %.1f,%.1f on step %d",
              player.position.x, player.position.y, step);
    }
    WorldUnload(&world);
}

int main(void)
{
    RUN(test_world_render_preparation_is_headless_and_incremental);
    RUN(test_empty_world_render_data_preserves_background_depth);
    RUN(test_a_refused_chunk_keeps_its_dirty_flag);
    RUN(test_emissive_render_data_selects_emitters_not_bright_terrain);
    RUN(test_particle_emission_is_explicit_per_effect);
    RUN(test_presentation_fx_spawn_update_and_expire);
    RUN(test_presentation_fx_rejects_invalid_lifetime_and_clears);
    RUN(test_presentation_fx_capacity_has_bounded_priority_overflow);
    RUN(test_presentation_fx_events_create_visuals_without_mutating_events);
    RUN(test_presentation_fx_delays_a_stage_without_shortening_it);
    RUN(test_presentation_fx_maps_every_combat_event_with_bounded_rates);
    RUN(test_laser_contact_heat_uses_elapsed_time_not_frame_count);
    RUN(test_camera_feedback_stacks_clamps_and_expires);
    RUN(test_camera_lookahead_damps_reversal_before_leading_backward);
    RUN(test_camera_feedback_invalid_time_preserves_safe_stable_state);
    RUN(test_transient_camera_never_changes_mouse_aim_transform);
    RUN(test_game_event_buffer_is_fixed_and_ordered);
    RUN(test_game_update_publishes_transient_events);
    RUN(test_environment_palettes_validate_parse_and_force);
    RUN(test_environment_descriptors_are_seeded_and_finite);
    RUN(test_environment_view_handles_resize_rotation_and_invalid_time);
    RUN(test_environment_state_never_changes_gameplay_world);
    RUN(test_biome_layout_is_seeded_complete_and_bounded);
    RUN(test_generated_biomes_have_distinct_material_identity);
    RUN(test_biome_boundaries_and_spawn_are_coherent);
    RUN(test_every_biome_can_host_the_protected_spawn);
    RUN(test_the_same_seed_always_generates_the_same_world);
    RUN(test_regenerating_one_world_from_a_seed_reproduces_it);
    RUN(test_world_effects_cannot_shift_the_terrain_a_seed_produces);
    RUN(test_a_seeded_session_replays_identically);
    RUN(test_the_tick_counter_survives_wrapping_its_cell_stamp);
    RUN(test_the_schedule_never_lists_a_chunk_twice);
    RUN(test_visual_particles_never_change_the_world);
    RUN(test_a_fresh_manager_holds_no_bodies);
    RUN(test_a_zero_initialised_handle_names_nothing);
    RUN(test_allocation_returns_a_usable_body);
    RUN(test_the_manager_refuses_work_past_its_budgets);
    RUN(test_a_freed_slot_is_reused);
    RUN(test_a_stale_handle_never_resolves_to_the_new_body);
    RUN(test_reset_releases_every_body);
    RUN(test_a_body_preserves_the_material_and_heat_it_was_given);
    RUN(test_out_of_range_cell_access_is_safe);
    RUN(test_finalize_reports_the_occupied_bounds);
    RUN(test_mass_follows_material_density);
    RUN(test_centre_of_mass_lands_where_the_shape_says);
    RUN(test_inertia_grows_with_how_spread_out_a_body_is);
    RUN(test_the_body_manager_leaves_the_world_alone);
    RUN(test_regenerating_the_world_drops_its_detached_pieces);
    RUN(test_terrain_render_key_tracks_raster_edits_only);
    RUN(test_terrain_render_bounds_follow_the_simulation_transform);
    RUN(test_terrain_render_key_rejects_free_reset_and_slot_reuse);
    RUN(test_terrain_render_data_handles_empty_and_maximum_rasters);
    RUN(test_a_lone_island_is_reported_detached);
    RUN(test_two_islands_are_separate_components);
    RUN(test_a_liquid_gap_does_not_join_two_components);
    RUN(test_an_intact_bridge_to_the_ground_prevents_detachment);
    RUN(test_cutting_the_bridge_detaches_the_island);
    RUN(test_a_corner_contact_does_not_join_two_components);
    RUN(test_a_diagonal_only_join_to_the_ground_reads_as_detached);
    RUN(test_a_component_crossing_a_chunk_boundary_stays_whole);
    RUN(test_a_component_continuing_past_the_region_is_unknown);
    RUN(test_a_component_that_only_touches_the_region_edge_is_detached);
    RUN(test_a_component_touching_the_world_edge_is_anchored);
    RUN(test_a_component_larger_than_the_budget_is_too_large);
    RUN(test_an_oversized_or_malformed_query_is_refused);
    RUN(test_the_detector_never_changes_the_world);
    RUN(test_extraction_moves_an_island_out_of_the_world);
    RUN(test_extraction_conserves_materials_and_heat);
    RUN(test_extraction_places_the_body_where_the_island_was);
    RUN(test_extraction_wakes_and_dirties_the_chunks_it_emptied);
    RUN(test_extraction_crosses_a_chunk_boundary);
    RUN(test_an_anchored_component_is_never_extracted);
    RUN(test_an_unknown_component_is_never_extracted);
    RUN(test_a_component_the_detector_refused_is_never_extracted);
    RUN(test_extraction_without_a_free_body_slot_changes_nothing);
    RUN(test_a_component_too_wide_for_a_body_changes_nothing);
    RUN(test_a_malformed_component_changes_nothing);
    RUN(test_a_component_the_world_has_moved_past_changes_nothing);
    RUN(test_reset_after_extraction_returns_the_store_to_empty);
    RUN(test_a_moving_body_travels_at_its_velocity);
    RUN(test_gravity_accelerates_every_body_equally);
    RUN(test_damping_depends_on_time_and_not_on_step_count);
    RUN(test_angular_velocity_turns_a_body);
    RUN(test_angular_damping_slows_a_spin);
    RUN(test_the_body_transform_round_trips);
    RUN(test_kinematics_are_deterministic);
    RUN(test_a_still_body_falls_asleep);
    RUN(test_a_sleeping_body_keeps_its_transform_and_stops_integrating);
    RUN(test_a_moving_or_spinning_body_stays_awake);
    RUN(test_the_quiet_spell_restarts_whenever_a_body_moves);
    RUN(test_an_impulse_wakes_a_body_and_turns_it_about_its_centre);
    RUN(test_only_live_bodies_are_integrated);
    RUN(test_reset_clears_kinetic_state);
    RUN(test_the_integrator_refuses_impossible_input);
    RUN(test_speeds_are_capped);
    RUN(test_integration_never_touches_the_world);
    RUN(test_a_body_lands_on_the_floor_and_stays_out_of_it);
    RUN(test_a_landed_body_settles_and_sleeps);
    RUN(test_a_body_stops_against_a_wall);
    RUN(test_an_off_centre_landing_turns_the_body);
    RUN(test_friction_slows_a_body_sliding_along_the_floor);
    RUN(test_restitution_controls_the_bounce);
    RUN(test_the_shipped_config_cannot_tunnel);
    RUN(test_a_fast_body_cannot_cross_a_thin_wall);
    RUN(test_a_body_outside_the_world_is_safe);
    RUN(test_collision_never_changes_the_world);
    RUN(test_only_live_bodies_collide);
    RUN(test_collision_is_deterministic);
    RUN(test_the_contact_cap_is_never_exceeded);
    RUN(test_substeps_stay_inside_their_budget);
    RUN(test_body_slots_are_bounded_and_reusable);
    RUN(test_the_dynamic_cell_budget_is_counted_and_enforced);
    RUN(test_the_awake_budget_is_honoured);
    RUN(test_sleeping_on_the_ground_releases_the_awake_budget);
    RUN(test_a_body_lost_outside_the_world_is_destroyed);
    RUN(test_an_offscreen_body_is_never_destroyed);
    RUN(test_world_bounds_follow_the_body);
    RUN(test_reset_returns_every_budget);
    RUN(test_lifecycle_decisions_are_deterministic);
    RUN(test_an_explosion_under_a_block_detaches_it);
    RUN(test_damage_that_leaves_the_support_standing_detaches_nothing);
    RUN(test_drilling_through_a_support_detaches_the_section_above);
    RUN(test_a_fragment_that_escapes_the_search_window_stays_static);
    RUN(test_a_fragment_below_the_minimum_size_stays_static);
    RUN(test_a_fragment_above_the_maximum_size_stays_static);
    RUN(test_a_fragment_one_cell_past_the_ceiling_stays_static);
    RUN(test_a_full_body_manager_leaves_the_fragment_static);
    RUN(test_a_full_cell_budget_leaves_the_world_unchanged);
    RUN(test_one_blast_can_detach_two_islands);
    RUN(test_a_fragment_across_a_chunk_boundary_still_detaches);
    RUN(test_ordinary_simulation_never_runs_the_detector);
    RUN(test_a_check_never_explores_past_its_cell_limit);
    RUN(test_automatic_detachment_is_deterministic);
    RUN(test_detachment_conserves_every_cell_it_moves);
    RUN(test_overlapping_damage_aggregates_into_one_region);
    RUN(test_touching_damage_too_wide_to_merge_stays_separate);
    RUN(test_regenerating_the_world_drops_its_damage_log);
    RUN(test_repeated_destruction_stays_bounded);
    RUN(test_the_game_loop_detaches_terrain_by_itself);
    RUN(test_an_impulse_changes_velocity_by_impulse_over_mass);
    RUN(test_a_heavier_body_moves_less_under_the_same_impulse);
    RUN(test_an_off_centre_impulse_spins_the_body);
    RUN(test_a_body_with_more_inertia_spins_less);
    RUN(test_an_explosion_throws_a_body_outward);
    RUN(test_a_body_outside_the_blast_radius_is_untouched);
    RUN(test_a_closer_body_is_thrown_harder);
    RUN(test_force_pushes_along_its_cone_and_spins_off_centre_bodies);
    RUN(test_force_is_stopped_by_terrain_in_the_way);
    RUN(test_a_meaningful_impulse_wakes_a_sleeping_body);
    RUN(test_a_refused_blast_and_a_dead_body_are_safe);
    RUN(test_a_blast_never_changes_the_static_world);
    RUN(test_blast_results_are_deterministic);
    RUN(test_an_explosion_throws_the_fragment_it_just_freed);
    RUN(test_the_explosion_ability_throws_terrain_by_itself);
    RUN(test_carving_a_body_removes_cells_and_bumps_its_revision);
    RUN(test_carving_updates_mass_without_moving_what_is_left);
    RUN(test_a_body_carved_away_entirely_is_freed);
    RUN(test_a_cut_through_a_slab_splits_it_in_two);
    RUN(test_a_split_piece_inherits_the_motion_of_where_it_was);
    RUN(test_fracture_is_deterministic);
    RUN(test_fracture_without_a_free_slot_keeps_the_body_whole);
    RUN(test_a_chip_too_small_to_be_a_body_is_dropped);
    RUN(test_the_player_cannot_stay_inside_a_body);
    RUN(test_a_small_body_is_shoved_more_easily_than_a_huge_one);
    RUN(test_grab_only_takes_a_body_within_reach);
    RUN(test_a_held_body_is_pulled_rather_than_placed);
    RUN(test_releasing_throws_a_light_body_further_than_a_heavy_one);
    RUN(test_a_hold_ends_safely_when_the_body_is_destroyed);
    RUN(test_an_explosion_carves_and_splits_a_moving_body);
    RUN(test_a_fast_player_cannot_cross_a_thin_body);
    RUN(test_the_laser_burns_only_the_first_thing_it_reaches);
    RUN(test_the_body_raycast_finds_a_thin_rotated_body);
    RUN(test_the_raycast_never_steps_over_material);
    RUN(test_a_grab_never_lands_on_empty_raster);
    RUN(test_a_hold_follows_or_ends_when_its_side_is_cut_away);
    RUN(test_thrust_builds_speed_rather_than_setting_it);
    RUN(test_a_reversal_brakes_through_zero);
    RUN(test_braking_is_stronger_than_letting_go);
    RUN(test_braking_cannot_overshoot_into_reverse);
    RUN(test_steering_is_weaker_at_speed_but_never_gone);
    RUN(test_boost_stages_are_reached_and_survive_release);
    RUN(test_a_fast_player_cannot_cross_a_thin_wall);
    RUN(test_a_diagonal_drill_does_not_stall);
    RUN(test_the_same_flight_replays_identically);
    RUN(test_movement_survives_an_absurd_velocity);
    RUN(test_entering_water_costs_speed);
    RUN(test_ability_table_passes_its_own_validation);
    RUN(test_a_one_shot_ability_respects_its_cooldown);
    RUN(test_a_held_ability_reports_one_start_per_hold);
    RUN(test_ability_knockback_reaches_the_player_through_events);
    RUN(test_every_material_has_table_data);
    RUN(test_material_table_passes_its_own_validation);
    RUN(test_every_material_carries_its_own_cryo_rate);
    RUN(test_solidity_matches_what_blocks_the_player);
    RUN(test_sand_falls_to_the_floor_and_is_conserved);
    RUN(test_sand_falls_at_most_one_cell_per_tick);
    RUN(test_water_spreads_sideways_and_is_conserved);
    RUN(test_rock_becomes_lava_above_its_threshold);
    RUN(test_water_becomes_steam_above_its_threshold);
    RUN(test_water_and_lava_react_into_steam_and_rock);
    RUN(test_one_fire_cell_cannot_consume_a_whole_dirt_field);
    RUN(test_a_lava_pocket_cannot_consume_its_rock_lining);
    RUN(test_lava_still_ignites_dirt_it_touches);
    RUN(test_settled_cells_sleep_but_wake_when_disturbed);
    RUN(test_drill_removes_solids_and_leaves_liquids);
    RUN(test_drill_returns_the_number_of_cells_it_removed);
    RUN(test_drill_cannot_breach_the_world_boundary);
    RUN(test_drill_heat_cannot_ignite_dirt_or_boil_water);
    RUN(test_a_settled_world_lets_chunks_sleep);
    RUN(test_activity_wakes_only_a_local_neighbourhood);
    RUN(test_tick_stats_report_structural_work);
    RUN(test_generation_streams_only_requested_dynamic_regions);
    RUN(test_boosting_player_tunnels_through_rock);
    RUN(test_boost_from_rest_bores_into_a_wall);
    RUN(test_boosting_player_tunnels_through_sand);
    RUN(test_drill_resistance_never_stalls_the_boost);
    RUN(test_normal_flight_keeps_a_hover_pose);
    RUN(test_boost_flight_reaches_a_head_first_pose);
    RUN(test_long_boost_climbs_three_stages_into_supersonic);
    RUN(test_stalled_boost_cannot_charge_a_later_stage);
    RUN(test_cryo_does_not_destroy_solid_terrain);
    RUN(test_no_material_evaporates_when_merely_cooled);
    RUN(test_cryo_snuffs_fire_into_smoke);
    RUN(test_cryo_freezes_water_into_standing_ice);
    RUN(test_heat_melts_ice_back_into_water);
    RUN(test_cryo_settles_lava_back_into_rock);
    RUN(test_one_force_blast_throws_loose_material_far);
    RUN(test_configured_force_power_hits_far_and_hard);
    RUN(test_force_blast_marks_solid_terrain_without_boring_through_it);
    RUN(test_force_blast_does_not_reach_behind_a_wall);
    RUN(test_bouncing_particles_do_not_pass_through_terrain);
    RUN(test_drill_debris_settles_as_ash_without_overwriting_terrain);
    RUN(test_passing_particles_ignore_terrain);
    RUN(test_player_never_ends_a_frame_inside_solid_terrain);

    printf("\n%d tests, %d failed\n", testsRun, testsFailed);
    return testsFailed == 0 ? 0 : 1;
}
