#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>
#include <raymath.h>

#include "audio.h"
#include "game.h"
#include "input.h"
#include "renderer.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define VIEW_WIDTH 320.0f
#define VIEW_HEIGHT 180.0f

/* How far the camera leads the player, as a fraction of the view in each
   direction, and over how many seconds of travel that lead is measured. The
   final boost is much too fast for a centred 320-cell view, so lookahead works
   together with speed-based widening below. */
#define CAMERA_LOOKAHEAD_TIME 0.42f
#define CAMERA_LOOKAHEAD_VIEW_FRACTION 0.30f
/* The view also widens with speed, so the fastest flight is the one that sees
   the most. It follows measured speed rather than the boost key: a player
   thrown by an explosion gets the same widening, and a boost stalled against
   rock does not. */
#define CAMERA_FAST_VIEW_SCALE 2.0f

static float CameraZoomForWindow(float viewScale)
{
    float horizontal = (float)GetScreenWidth() / (VIEW_WIDTH * viewScale);
    float vertical = (float)GetScreenHeight() / (VIEW_HEIGHT * viewScale);
    return fmaxf(1.0f, fminf(horizontal, vertical));
}

static Vector2 CameraLookahead(Vector2 velocity)
{
    float limitX = VIEW_WIDTH * CAMERA_LOOKAHEAD_VIEW_FRACTION;
    float limitY = VIEW_HEIGHT * CAMERA_LOOKAHEAD_VIEW_FRACTION;

    return (Vector2){
        Clamp(velocity.x * CAMERA_LOOKAHEAD_TIME, -limitX, limitX),
        Clamp(velocity.y * CAMERA_LOOKAHEAD_TIME, -limitY, limitY)
    };
}

static float CameraViewScaleForSpeed(const Player *player)
{
    float speed = sqrtf(player->velocity.x * player->velocity.x +
                        player->velocity.y * player->velocity.y);
    float range = player->boostMaxSpeed - player->maxSpeed;
    float excess;

    if (range <= 0.001f) {
        return 1.0f;
    }
    excess = Clamp((speed - player->maxSpeed) / range, 0.0f, 1.0f);
    return 1.0f + (CAMERA_FAST_VIEW_SCALE - 1.0f) * excess;
}

/* The part of the world the camera can see, in cells. WorldRenderer rebuilds
   only what falls inside it, so drawing costs what is on screen rather than
   what the whole simulation happens to be doing. */
static Rectangle VisibleWorldRectangle(Camera2D camera)
{
    Vector2 topLeft = GetScreenToWorld2D((Vector2){0.0f, 0.0f}, camera);
    Vector2 bottomRight = GetScreenToWorld2D(
        (Vector2){(float)GetScreenWidth(), (float)GetScreenHeight()}, camera);

    return (Rectangle){topLeft.x, topLeft.y, bottomRight.x - topLeft.x,
                       bottomRight.y - topLeft.y};
}

static const char *PlayerBoostLabel(const Player *player, float speed)
{
    if (player->boostStage == PLAYER_BOOST_STAGE_THREE) {
        return speed >= player->sonicSpeed ? "MACH" : "BOOST III";
    }
    if (player->boostStage == PLAYER_BOOST_STAGE_TWO) {
        return "BOOST II";
    }
    if (player->boostStage == PLAYER_BOOST_STAGE_ONE) {
        return "BOOST I";
    }
    return "HOVER";
}

static Vector2 ClampCameraTarget(Vector2 target, float zoom, const World *world)
{
    float halfWidth = (float)GetScreenWidth() / (2.0f * zoom);
    float halfHeight = (float)GetScreenHeight() / (2.0f * zoom);

    if (halfWidth * 2.0f >= (float)world->width) {
        target.x = (float)world->width * 0.5f;
    } else {
        target.x = Clamp(target.x, halfWidth, (float)world->width - halfWidth);
    }
    if (halfHeight * 2.0f >= (float)world->height) {
        target.y = (float)world->height * 0.5f;
    } else {
        target.y = Clamp(target.y, halfHeight, (float)world->height - halfHeight);
    }
    return target;
}

static void DrawDebugHud(const GameState *game, const GameEventBuffer *events,
                         const Renderer *renderer, Vector2 cursorCell)
{
    const World *world = &game->world;
    const Player *player = &game->player;
    const AbilitySystem *abilities = &game->abilities;
    const AbilityState *explosion = AbilityStateAt(abilities, ABILITY_EXPLOSION);
    const int panelWidth = 520;
    const int panelHeight = 240;
    float cooldown = explosion->cooldown;
    float playerSpeed = sqrtf(player->velocity.x * player->velocity.x +
                              player->velocity.y * player->velocity.y);
    CellMaterial cursorMaterial = WorldGetCell(world, (int)cursorCell.x, (int)cursorCell.y);
    const WorldRendererStats *renderStats = RendererWorldStats(renderer);
    const RendererFrameStats *frameStats = RendererStats(renderer);

    DrawRectangle(12, 12, panelWidth, panelHeight, (Color){4, 8, 15, 205});
    DrawRectangleLines(12, 12, panelWidth, panelHeight, (Color){82, 157, 208, 220});
    DrawText(TextFormat("FPS: %d", GetFPS()), 24, 23, 20, RAYWHITE);
    DrawText(TextFormat("PLAYER: %.1f, %.1f  V: %.0f  %s", player->position.x,
                        player->position.y, playerSpeed,
                        PlayerBoostLabel(player, playerSpeed)),
             24, 47, 18, (Color){174, 219, 248, 255});
    DrawText(TextFormat("ACTIVE: %d CELLS | %d CHUNKS",
                        WorldCountDynamicCells(world),
                        world->activeChunkCount), 24, 69, 18,
             (Color){233, 198, 105, 255});
    DrawText(TextFormat("POWER: %s (%s)", AbilitiesCurrentName(abilities),
                        InputAbilityBinding(abilities->lastUsed)),
             24, 91, 18, (Color){255, 126, 86, 255});
    DrawText(TextFormat("CURSOR: %d, %d  %s  %.0fC", (int)cursorCell.x,
                        (int)cursorCell.y, WorldMaterialName(cursorMaterial),
                        WorldGetTemperature(world, (int)cursorCell.x, (int)cursorCell.y)),
             24, 113, 18, (Color){186, 194, 205, 255});
    DrawText(TextFormat("TICK: %llu CELLS / %u CHUNKS | EVENTS: %u +%u",
                        (unsigned long long)world->lastTickStats.processedCells,
                        world->lastTickStats.processedChunks,
                        (unsigned int)events->count,
                        (unsigned int)events->dropped),
             24, 135, 14, (Color){150, 205, 178, 255});
    DrawText(TextFormat("RENDER: %u UPLOADS  %.1f KiB  %.2f ms | PAGES: %u/%u +%u",
                        renderStats->textureUploads,
                        (double)renderStats->uploadedBytes / 1024.0,
                        renderStats->preparationMilliseconds,
                        renderStats->visiblePages, renderStats->residentPages,
                        renderStats->pageBinds),
             24, 153, 14, (Color){166, 183, 223, 255});
    DrawText(TextFormat("POST: %s %dx%d | %u PASSES %u TARGETS | %.2f ms",
                        frameStats->bloomEnabled ? "BLOOM" : "SHARP",
                        frameStats->bloomWidth, frameStats->bloomHeight,
                        frameStats->offscreenPasses, frameStats->renderTargets,
                        frameStats->bloomSubmissionMilliseconds),
             24, 171, 14, (Color){205, 156, 234, 255});
    DrawText(TextFormat("FX: %u ACTIVE | %u PEAK | %u DROPPED",
                        (unsigned int)frameStats->activeFx,
                        (unsigned int)frameStats->peakFx,
                        (unsigned int)frameStats->droppedFx),
             24, 189, 14, (Color){255, 188, 119, 255});
    /* The seed is here so that a bug report is reproducible: it plus the
       inputs is the whole state of a session. */
    DrawText(TextFormat("SEED: 0x%llx", (unsigned long long)game->worldSeed),
             24, 207, 14, (Color){186, 194, 205, 255});
    if (cooldown <= 0.0f) {
        DrawText("EXPLOSION: READY", 24, 225, 14, LIME);
    } else {
        DrawText(TextFormat("EXPLOSION: %.2fs", cooldown), 24, 225, 14,
                 LIGHTGRAY);
    }
}

/* Composed from the ability table rather than written out, so a new power
   appears in the hint the moment it is defined and bound. */
static void DrawControlsHint(void)
{
    const char *hint = "WASD fly  |  Shift staged boost/drill";
    int fontSize = 18;
    int id;
    int width;
    int x;
    int y;

    for (id = 0; id < ABILITY_COUNT; ++id) {
        hint = TextFormat("%s  |  %s %s", hint,
                          InputAbilityBinding((AbilityId)id),
                          AbilityDefinitionAt((AbilityId)id)->name);
    }
    hint = TextFormat("%s  |  R regenerate  |  F1 HUD", hint);
    width = MeasureText(hint, fontSize);
    x = (GetScreenWidth() - width) / 2;
    y = GetScreenHeight() - 34;

    DrawRectangle(x - 10, y - 5, width + 20, fontSize + 10, (Color){3, 6, 12, 190});
    DrawText(hint, x, y, fontSize, (Color){214, 221, 229, 255});
}

static void RunSmokePlayerProbe(World *world, ParticleSystem *particles,
                                bool *collisionObserved, bool *drillObserved)
{
    Player probe;
    int drilledCells = 0;
    int step;
    int x;
    int y;

    for (y = 44; y <= 56; ++y) {
        WorldSetCell(world, 232, y, MATERIAL_ROCK);
    }
    PlayerInit(&probe, (Vector2){225.0f, 50.0f});
    probe.velocity.x = 180.0f;
    PlayerUpdate(&probe, world, (Vector2){0.0f, 0.0f}, false, 0.05f);
    *collisionObserved = probe.position.x < 229.0f && probe.velocity.x < -10.0f &&
                         probe.impactStrength > 80.0f &&
                         probe.impactNormal.x < -0.5f;
    for (y = 44; y <= 56; ++y) {
        WorldSetCell(world, 232, y, MATERIAL_EMPTY);
    }

    for (x = 232; x <= 242; ++x) {
        for (y = 44; y <= 56; ++y) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    PlayerInit(&probe, (Vector2){212.0f, 50.0f});
    for (step = 0; step < 10; ++step) {
        PlayerUpdate(&probe, world, (Vector2){1.0f, 0.0f}, true, 0.05f);
        drilledCells += probe.drilledCells;
        /* Drive the same feedback the frame loop does, so the boost trail and
           drill debris paths stay covered without steering the live player. */
        if (probe.boostTrailEmitted) {
            ParticlesSpawnBoostTrail(particles, probe.position, probe.velocity,
                                     (int)probe.boostStage);
        }
        if (probe.drilledCells > 0) {
            ParticlesSpawnDrillDebris(particles, probe.drillPosition,
                                      probe.velocity, probe.drilledCells);
        }
    }
    *drillObserved = probe.position.x > 242.0f && drilledCells > 20;
    for (x = 232; x <= 242; ++x) {
        for (y = 44; y <= 56; ++y) {
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
        }
    }
}

/* Guarantees the smoke run has terrain under the cursor to burn, plus a
   water/lava pair that must react, wherever the world generator put things. */
static void SetupSmokeTarget(World *world, Vector2 aim)
{
    int centerX = (int)aim.x;
    int centerY = (int)aim.y;
    int x;
    int y;

    for (y = centerY - 10; y <= centerY + 22; ++y) {
        for (x = centerX - 20; x <= centerX + 20; ++x) {
            WorldSetCell(world, x, y, MATERIAL_DIRT);
        }
    }
    for (y = centerY - 26; y < centerY - 10; ++y) {
        for (x = centerX - 20; x <= centerX + 20; ++x) {
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
        }
    }
    WorldSetCell(world, centerX - 6, centerY - 12, MATERIAL_WATER);
    WorldSetCell(world, centerX - 5, centerY - 12, MATERIAL_LAVA);

    /* A separate pocket survives the explosion long enough to make lava and
       fire emissive visible in the reference screenshot. */
    for (x = centerX + 26; x <= centerX + 36; ++x) {
        WorldSetCell(world, x, centerY + 16, MATERIAL_ROCK);
    }
    for (y = centerY + 12; y <= centerY + 15; ++y) {
        WorldSetCell(world, centerX + 26, y, MATERIAL_ROCK);
        WorldSetCell(world, centerX + 36, y, MATERIAL_ROCK);
    }
    for (y = centerY + 13; y <= centerY + 15; ++y) {
        for (x = centerX + 28; x <= centerX + 34; ++x) {
            WorldSetCell(world, x, y, MATERIAL_LAVA);
        }
    }
    WorldSetCell(world, centerX + 31, centerY + 10, MATERIAL_FIRE);
}

static bool RunSmokeFireContainmentProbe(void)
{
    const int minimumX = 8;
    const int maximumX = 40;
    const int minimumY = 8;
    const int maximumY = 24;
    const int initialFireRadius = 1;
    const int minimumRemainingDirt = 480;
    World probe;
    int remainingDirt = 0;
    int tick;
    int y;

    if (!WorldInit(&probe, 48, 32)) {
        return false;
    }

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            WorldSetCell(&probe, x, y, MATERIAL_DIRT);
        }
    }
    for (y = 16 - initialFireRadius; y <= 16 + initialFireRadius; ++y) {
        int x;

        for (x = 24 - initialFireRadius; x <= 24 + initialFireRadius; ++x) {
            WorldSetCell(&probe, x, y, MATERIAL_FIRE);
        }
    }

    for (tick = 0; tick < 240; ++tick) {
        WorldUpdate(&probe);
    }

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            if (WorldGetCell(&probe, x, y) == MATERIAL_DIRT) {
                ++remainingDirt;
            }
        }
    }
    WorldUnload(&probe);
    return remainingDirt >= minimumRemainingDirt;
}

static bool EventsContain(const GameEventBuffer *events, GameEventType type)
{
    uint16_t index;

    for (index = 0u; index < events->count; ++index) {
        if (events->events[index].type == type) {
            return true;
        }
    }
    return false;
}

/* Presentation consumes transient gameplay facts after GameUpdate. Gameplay
   neither plays sounds nor shakes a Camera2D, and adding another consumer does
   not require another one-frame flag on Player or PowerSystem. */
static void PresentGameEvents(const GameEventBuffer *events, GameAudio *audio,
                              float *cameraShake)
{
    uint16_t index;

    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];

        switch (event->type) {
        case GAME_EVENT_BOOST_STAGE: {
            int stage = event->count;
            float shake = stage == 1 ? 0.8f : (stage == 2 ? 1.8f : 4.0f);

            *cameraShake = fmaxf(*cameraShake, shake);
            GameAudioPlayBoost(audio, stage);
            break;
        }
        case GAME_EVENT_PLAYER_IMPACT:
            *cameraShake = fmaxf(
                *cameraShake,
                Clamp((event->strength - 10.0f) * 0.025f, 0.0f, 2.6f));
            GameAudioPlayImpact(audio, event->strength);
            break;
        case GAME_EVENT_PLAYER_DRILL:
            *cameraShake = fmaxf(
                *cameraShake,
                Clamp(0.25f + (float)event->count * 0.025f, 0.0f, 1.4f));
            break;
        case GAME_EVENT_FORCE:
            *cameraShake = fmaxf(*cameraShake, 4.2f);
            GameAudioPlayForce(audio);
            break;
        case GAME_EVENT_EXPLOSION:
            *cameraShake = fmaxf(*cameraShake, 4.8f);
            GameAudioPlayExplosion(audio);
            break;
        case GAME_EVENT_MATERIAL_REACTION:
            GameAudioPlayReaction(audio);
            break;
        default:
            break;
        }
    }
}

int main(int argc, char **argv)
{
    GameConfig config = GameDefaultConfig();
    GameState game = {0};
    GameEventBuffer events = {0};
    GameAudio audio = {0};
    Renderer renderer = {0};
    Camera2D camera = {0};
    bool debugHud = true;
    bool smokeTest = false;
    int argument;
    int smokeFrames = 0;
    Vector2 cameraFocus;
    Vector2 smokeAim = {0.0f, 0.0f};
    float cameraShake = 0.0f;
    float cameraViewScale = 1.0f;
    bool smokeReactionObserved = false;
    bool smokeLaserHitObserved = false;
    bool smokeExplosionObserved = false;
    bool smokeCollisionObserved = false;
    bool smokeDrillObserved = false;
    bool smokeFireContained = false;
    bool smokeResizeObserved = false;
    bool smokeResizeRestored = false;
    bool smokeBloomObserved = false;
    bool smokeBloomResized = false;
    bool smokeBloomRestored = false;
    bool smokeTargetsSynchronized = true;
    bool smokePresentationFxObserved = false;
    double smokeBloomSubmissionTotal = 0.0;
    double smokeBloomSubmissionMaximum = 0.0;
    int smokeBloomFrames = 0;
    int exitCode = 0;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--smoke-test") == 0) {
            smokeTest = true;
        } else if (strcmp(argv[argument], "--seed") == 0 && argument + 1 < argc) {
            /* Replays a reported world exactly. strtoull takes 0x forms, which
               is how the debug HUD prints the seed. */
            config.seed = strtoull(argv[++argument], NULL, 0);
        } else {
            fprintf(stderr, "usage: %s [--smoke-test] [--seed VALUE]\n", argv[0]);
            return 1;
        }
    }
    /* The smoke test must produce the same frame every run, or its reference
       screenshot is worthless as a comparison. */
    if (smokeTest && config.seed == 0u) {
        config.seed = 0x00e6be11u;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "EMBERFALL - pixel physics sandbox");
    if (!IsWindowReady()) {
        fprintf(stderr, "Failed to initialize the raylib window.\n");
        return 1;
    }

    SetWindowMinSize(640, 360);
    SetTargetFPS(120);
    SetExitKey(KEY_ESCAPE);
    (void)GameAudioInit(&audio);

    if (!GameInit(&game, config) ||
        !RendererInit(&renderer, &game)) {
        fprintf(stderr, "Failed to allocate or initialize the world.\n");
        RendererUnload(&renderer);
        GameUnload(&game);
        GameAudioUnload(&audio);
        CloseWindow();
        return 1;
    }

    if (smokeTest) {
        smokeFireContained = RunSmokeFireContainmentProbe();
        RunSmokePlayerProbe(&game.world, &game.particles, &smokeCollisionObserved,
                            &smokeDrillObserved);
        /* The probe has exercised the spawn paths; drop what it emitted so the
           reference screenshot starts from a clean frame. */
        ParticlesInit(&game.particles, game.worldSeed);
        /* Build the laser/explosion target relative to the spawn instead of at
           fixed coordinates: generation is randomised, so a hardcoded point is
           not guaranteed to contain terrain. */
        smokeAim = (Vector2){game.player.position.x + 14.0f,
                             game.player.position.y + 40.0f};
        SetupSmokeTarget(&game.world, smokeAim);
    }
    cameraFocus = game.player.position;
    camera.target = cameraFocus;
    camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                              (float)GetScreenHeight() * 0.5f};
    camera.rotation = 0.0f;
    camera.zoom = CameraZoomForWindow(cameraViewScale);

    while (!WindowShouldClose()) {
        float deltaTime = fminf(GetFrameTime(), 0.05f);
        AppInput input = InputPoll(&game.world, camera);
        Vector2 desiredCamera;
        Vector2 cursorCell = input.cursorCell;
        Vector2 aimPosition = input.game.aimWorld;

        /* Exercise the render-target resize lifecycle in the automated GL
           smoke run, then restore the reference screenshot dimensions. */
        if (smokeTest && smokeFrames == 2) {
            SetWindowSize(WINDOW_WIDTH - 320, WINDOW_HEIGHT - 180);
        } else if (smokeTest && smokeFrames == 7) {
            SetWindowSize(WINDOW_WIDTH, WINDOW_HEIGHT);
        }

        if (input.toggleDebugPressed) {
            debugHud = !debugHud;
        }

        if (smokeTest) {
            aimPosition = (Vector2){smokeAim.x + 0.5f, smokeAim.y + 0.5f};
            cursorCell = smokeAim;
            input.game.aimWorld = aimPosition;
            memset(input.game.ability, 0, sizeof(input.game.ability));
            input.game.ability[ABILITY_LASER] = smokeFrames >= 1 && smokeFrames <= 9;
            input.game.ability[ABILITY_EXPLOSION] = smokeFrames == 5;
            input.game.regeneratePressed = false;
        }

        GameUpdate(&game, &input.game, deltaTime, &events);
        if (input.game.regeneratePressed) {
            cameraFocus = game.player.position;
            cameraShake = 0.0f;
            RendererClearPresentation(&renderer);
        }
        {
            GameAudioState sounding = {0};

            sounding.laser = AbilityStateAt(&game.abilities, ABILITY_LASER)->active;
            sounding.drilling = game.player.drilledCells > 0;
            sounding.drillMaterial = game.player.drillMaterial;
            sounding.chill = AbilityStateAt(&game.abilities, ABILITY_CRYO)->active;
            GameAudioUpdate(&audio, sounding, deltaTime);
        }
        PresentGameEvents(&events, &audio, &cameraShake);
        RendererUpdatePresentation(&renderer, &events, deltaTime);
        if (smokeTest) {
            smokeReactionObserved = smokeReactionObserved ||
                                    EventsContain(&events,
                                                  GAME_EVENT_MATERIAL_REACTION);
            smokeLaserHitObserved = smokeLaserHitObserved ||
                                    EventsContain(&events, GAME_EVENT_LASER_HIT);
            smokeExplosionObserved = smokeExplosionObserved ||
                                     EventsContain(&events,
                                                   GAME_EVENT_EXPLOSION);
        }

        camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                                  (float)GetScreenHeight() * 0.5f};
        /* Smooth the view scale rather than the zoom so the rate does not depend
           on window size, and keep it slower than the focus so the frame breathes
           instead of snapping. */
        cameraViewScale +=
            (CameraViewScaleForSpeed(&game.player) - cameraViewScale) *
                           (1.0f - expf(-4.5f * deltaTime));
        camera.zoom = CameraZoomForWindow(cameraViewScale);
        {
            Vector2 lookahead = CameraLookahead(game.player.velocity);
            Vector2 lead = {game.player.position.x + lookahead.x,
                            game.player.position.y + lookahead.y};

            desiredCamera = ClampCameraTarget(lead, camera.zoom, &game.world);
        }
        cameraFocus.x += (desiredCamera.x - cameraFocus.x) *
                         (1.0f - expf(-8.0f * deltaTime));
        cameraFocus.y += (desiredCamera.y - cameraFocus.y) *
                         (1.0f - expf(-8.0f * deltaTime));
        cameraShake *= expf(-9.0f * deltaTime);
        if (cameraShake < 0.02f) {
            cameraShake = 0.0f;
        }
        camera.target = (Vector2){
            cameraFocus.x + ((float)GetRandomValue(-1000, 1000) / 1000.0f) * cameraShake,
            cameraFocus.y + ((float)GetRandomValue(-1000, 1000) / 1000.0f) * cameraShake
        };

        /* The player carries their own light. Without it a bored tunnel would
           be unplayably dark the moment it leaves the reach of daylight, and
           the drill would be a way to blind yourself. It brightens with the
           boost, so the fastest flight also lights the furthest. */
        WorldSetPointLight(&game.world, game.player.position,
                           52.0f + (float)game.player.boostStage * 22.0f,
                           0.72f + (float)game.player.boostStage * 0.09f);

        RendererRenderScene(&renderer, &game, camera, aimPosition,
                            VisibleWorldRectangle(camera));
        if (smokeTest) {
            const RendererFrameStats *frameStats = RendererStats(&renderer);
            int screenWidth = GetScreenWidth();
            int screenHeight = GetScreenHeight();

            smokeTargetsSynchronized = smokeTargetsSynchronized &&
                                       frameStats->targetWidth == screenWidth &&
                                       frameStats->targetHeight == screenHeight;
            if (frameStats->bloomEnabled) {
                smokeTargetsSynchronized = smokeTargetsSynchronized &&
                                           frameStats->bloomWidth ==
                                               (screenWidth + 1) / 2 &&
                                           frameStats->bloomHeight ==
                                               (screenHeight + 1) / 2;
            }

            smokeBloomObserved = smokeBloomObserved ||
                                 (frameStats->bloomEnabled &&
                                  frameStats->offscreenPasses == 5u &&
                                  frameStats->renderTargets == 4u);
            smokePresentationFxObserved = smokePresentationFxObserved ||
                                          (frameStats->activeFx > 0u &&
                                           frameStats->peakFx == 2u &&
                                           frameStats->droppedFx == 0u);
            smokeBloomResized = smokeBloomResized ||
                                (frameStats->bloomEnabled &&
                                 frameStats->bloomWidth == (WINDOW_WIDTH - 320) / 2 &&
                                 frameStats->bloomHeight == (WINDOW_HEIGHT - 180) / 2);
            smokeBloomRestored = smokeBloomRestored ||
                                 (smokeBloomResized && smokeFrames >= 7 &&
                                  frameStats->bloomWidth == WINDOW_WIDTH / 2 &&
                                  frameStats->bloomHeight == WINDOW_HEIGHT / 2);
            if (frameStats->bloomEnabled && smokeFrames >= 8) {
                smokeBloomSubmissionTotal +=
                    frameStats->bloomSubmissionMilliseconds;
                smokeBloomSubmissionMaximum =
                    fmax(smokeBloomSubmissionMaximum,
                         frameStats->bloomSubmissionMilliseconds);
                ++smokeBloomFrames;
            }
            smokeResizeObserved = smokeResizeObserved ||
                                  (frameStats->targetWidth == WINDOW_WIDTH - 320 &&
                                   frameStats->targetHeight == WINDOW_HEIGHT - 180);
            smokeResizeRestored = smokeResizeRestored ||
                                  (smokeResizeObserved && smokeFrames >= 7 &&
                                   frameStats->targetWidth == WINDOW_WIDTH &&
                                   frameStats->targetHeight == WINDOW_HEIGHT);
        }

        BeginDrawing();
        /* This clear is intentionally retained as a safe backbuffer fallback
           if RendererComposite ever has no valid scene target to draw. */
        ClearBackground((Color){2, 4, 9, 255});
        RendererComposite(&renderer);
        if (debugHud) {
            DrawDebugHud(&game, &events, &renderer, cursorCell);
        }
        DrawControlsHint();
        EndDrawing();

        if (smokeTest) {
            if (smokeFrames == 10) {
                TakeScreenshot("build/emberfall-smoke.png");
            }
            if (++smokeFrames >= 12) {
                break;
            }
        }
    }

    if (smokeTest && smokeBloomFrames > 0) {
        const RendererFrameStats *frameStats = RendererStats(&renderer);

        printf("Smoke render: bloom=%dx%d passes=%u targets=%u "
               "submit_avg=%.3fms submit_max=%.3fms "
               "resize=%d restored=%d bloom_resize=%d bloom_restored=%d "
               "target_sync=%d fx_peak=%u fx_dropped=%u\n",
               frameStats->bloomWidth, frameStats->bloomHeight,
               frameStats->offscreenPasses, frameStats->renderTargets,
               smokeBloomSubmissionTotal / (double)smokeBloomFrames,
               smokeBloomSubmissionMaximum, smokeResizeObserved,
               smokeResizeRestored, smokeBloomResized, smokeBloomRestored,
               smokeTargetsSynchronized, (unsigned int)frameStats->peakFx,
               (unsigned int)frameStats->droppedFx);
    }

    if (smokeTest && (!smokeReactionObserved || !smokeLaserHitObserved ||
                      !smokeExplosionObserved || !smokeCollisionObserved ||
                      !smokeDrillObserved || !smokeFireContained ||
                      !smokeBloomObserved || !smokeTargetsSynchronized ||
                      !smokePresentationFxObserved ||
                      game.world.activeChunkCount <= 0 ||
                      game.world.activeChunkCount >=
                          game.world.chunkColumns * game.world.chunkRows)) {
        fprintf(stderr,
                "Smoke test failed: reaction=%d laser=%d explosion=%d collision=%d "
                "drill=%d fire_contained=%d resize=%d restored=%d "
                "bloom=%d bloom_resize=%d bloom_restored=%d target_sync=%d "
                "presentation_fx=%d chunks=%d/%d\n",
                smokeReactionObserved, smokeLaserHitObserved, smokeExplosionObserved,
                smokeCollisionObserved, smokeDrillObserved, smokeFireContained,
                smokeResizeObserved, smokeResizeRestored,
                smokeBloomObserved, smokeBloomResized, smokeBloomRestored,
                smokeTargetsSynchronized,
                smokePresentationFxObserved,
                game.world.activeChunkCount,
                game.world.chunkColumns * game.world.chunkRows);
        exitCode = 2;
    }
    RendererUnload(&renderer);
    GameUnload(&game);
    GameAudioUnload(&audio);
    CloseWindow();
    return exitCode;
}
