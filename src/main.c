#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>
#include <raymath.h>

#include "audio.h"
#include "camera_feedback.h"
#include "game.h"
#include "input.h"
#include "renderer.h"
#include "smoke_test.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define VIEW_WIDTH 320.0f
#define VIEW_HEIGHT 180.0f
/* How far the wheel may take the view: a scale of the logical 320x180 view,
   below one to look closer, above to see more. Zooming out makes the page
   cache and the light window grow with what is on screen; three times the
   view at full boost widening is still a few dozen pages. */
#define VIEW_ZOOM_CLOSEST 0.5f
#define VIEW_ZOOM_FURTHEST 3.0f
/* Each wheel notch scales the view by this much; the camera eases to it. */
#define VIEW_ZOOM_STEP 1.15f

/* The player's zoom, kept apart from the camera's own widening at speed: the
   two multiply, so a boost still pulls back from wherever the player left
   the view. */
typedef struct ViewZoom {
    float target;
    float current;
} ViewZoom;

static void ViewZoomUpdate(ViewZoom *zoom, float steps, float deltaTime)
{
    if (steps != 0.0f) {
        zoom->target *= powf(VIEW_ZOOM_STEP, -steps);
        zoom->target = Clamp(zoom->target, VIEW_ZOOM_CLOSEST, VIEW_ZOOM_FURTHEST);
    }
    /* Eased in log space, so zooming out from close takes as long as zooming
       in from far. */
    zoom->current = expf(logf(zoom->current) +
                         (logf(zoom->target) - logf(zoom->current)) *
                             (1.0f - expf(-12.0f * deltaTime)));
}

static float CameraZoomForWindow(float viewScale)
{
    if (!isfinite(viewScale) || viewScale < VIEW_ZOOM_CLOSEST) {
        viewScale = VIEW_ZOOM_CLOSEST;
    }
    float horizontal = (float)GetScreenWidth() / (VIEW_WIDTH * viewScale);
    float vertical = (float)GetScreenHeight() / (VIEW_HEIGHT * viewScale);
    return fmaxf(1.0f, fminf(horizontal, vertical));
}

/* The part of the world the camera can see, in cells. WorldRenderer rebuilds
   only what falls inside it, so drawing costs what is on screen rather than
   what the whole simulation happens to be doing. */
static Rectangle VisibleWorldRectangle(Camera2D camera)
{
    Vector2 corners[4] = {
        GetScreenToWorld2D((Vector2){0.0f, 0.0f}, camera),
        GetScreenToWorld2D((Vector2){(float)GetScreenWidth(), 0.0f}, camera),
        GetScreenToWorld2D(
            (Vector2){0.0f, (float)GetScreenHeight()}, camera),
        GetScreenToWorld2D(
            (Vector2){(float)GetScreenWidth(), (float)GetScreenHeight()},
            camera),
    };
    float minimumX = corners[0].x;
    float minimumY = corners[0].y;
    float maximumX = corners[0].x;
    float maximumY = corners[0].y;
    int index;

    /* Rotation makes opposite corners insufficient: the other two can extend
       beyond their axis-aligned bounds and would otherwise expose an uncached
       world-page sliver during a camera impulse. */
    for (index = 1; index < 4; ++index) {
        minimumX = fminf(minimumX, corners[index].x);
        minimumY = fminf(minimumY, corners[index].y);
        maximumX = fmaxf(maximumX, corners[index].x);
        maximumY = fmaxf(maximumY, corners[index].y);
    }
    return (Rectangle){minimumX, minimumY, maximumX - minimumX,
                       maximumY - minimumY};
}

static const char *PlayerBoostLabel(const Player *player, float speed)
{
    if (!player->boosting) {
        return "HOVER";
    }
    return speed >= player->sonicSpeed ? "MACH" : "BOOST";
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
    const AbilityState *punch = AbilityStateAt(abilities, ABILITY_FORCE);
    const int panelWidth = 620;
    const int panelHeight = 304;
    float cooldown = punch->cooldown;
    float playerSpeed = sqrtf(player->velocity.x * player->velocity.x +
                              player->velocity.y * player->velocity.y);
    CellMaterial cursorMaterial = WorldGetCell(world, (int)cursorCell.x, (int)cursorCell.y);
    const WorldRendererStats *renderStats = RendererWorldStats(renderer);
    const RendererFrameStats *frameStats = RendererStats(renderer);
    const EnvironmentPaletteDefinition *environmentPalette =
        EnvironmentPaletteDefinitionAt(frameStats->environmentPalette);

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
    {
        const char *binding = InputAbilityBinding(abilities->lastUsed);

        DrawText(TextFormat("POWER: %s (%s)", AbilitiesCurrentName(abilities),
                            binding != NULL ? binding : "—"),
                 24, 91, 18, (Color){255, 126, 86, 255});
    }
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
    DrawText(TextFormat("RENDER: %u UPLOADS  %.1f KiB  %.2f ms | PAGES: %u/%u +%u "
                        "| LIGHT: %s %.2f ms %u UP",
                        renderStats->textureUploads,
                        (double)renderStats->uploadedBytes / 1024.0,
                        renderStats->preparationMilliseconds,
                        renderStats->visiblePages, renderStats->residentPages,
                        renderStats->pageBinds,
                        frameStats->lightingEnabled ? "GPU" : "OFF",
                        frameStats->lightMilliseconds,
                        frameStats->lightUploads),
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
    DrawText(TextFormat("BODIES: %u VISIBLE / %u CACHED | %u DRAWS %u UP | %.1f KiB",
                        frameStats->visibleTerrainBodies,
                        frameStats->cachedTerrainBodies,
                        frameStats->terrainBodyDrawCalls,
                        frameStats->terrainBodyTextureUpdates,
                        (double)frameStats->terrainBodyTextureMemoryBytes /
                            1024.0),
             24, 207, 14, (Color){139, 218, 201, 255});
    /* Simulation-side counters, read and never written here: the HUD is a
       reader of gameplay state, and no simulation module draws. The detach
       numbers answer the two questions a player-facing bug about falling
       terrain always turns into — did anything get checked, and did anything
       come loose. */
    DrawText(TextFormat("TERRAIN: %d LIVE %d AWAKE | DETACH %d CHECKS %d FREED "
                        "%d CELLS | WELD %d BACK | BLAST %d BOOM %d FORCE",
                        DynamicTerrainStatistics(&game->dynamicTerrain)->activeBodies,
                        DynamicTerrainStatistics(&game->dynamicTerrain)->awakeBodies,
                        game->detach.stats.detachChecks,
                        game->detach.stats.autoDetachSucceeded,
                        game->detach.stats.autoDetachCells,
                        game->weld.stats.bodiesWelded,
                        game->impulses.stats.bodiesAffectedByExplosion,
                        game->impulses.stats.bodiesAffectedByForce),
             24, 225, 14, (Color){139, 218, 201, 255});
    /* What the player can take hold of, and what they have. Read from the
       simulation's own state — nothing here writes back into it, and the
       simulation draws nothing. */
    DrawText(TextFormat("HOLD: %s | PUSH %d | CUT %d CELLS %d SPLITS",
                        TerrainInteractionIsHolding(&game->interaction,
                                                    &game->dynamicTerrain)
                            ? "CARRYING (F)"
                            : (DynamicTerrainGetConst(&game->dynamicTerrain,
                                                      game->interaction.hovered) !=
                                       NULL
                                   ? "READY (F)"
                                   : "-"),
                        game->interaction.stats.braceFrames,
                        game->damage.stats.cellsCarved,
                        game->damage.stats.fractureSplits),
             24, 243, 14, (Color){228, 208, 140, 255});
    DrawText(TextFormat("ENV: %s | %u+%u DRAWS | %02d:%02d %s | SKY %u/%u",
                        environmentPalette != NULL ? environmentPalette->name
                                                   : "INVALID",
                        (unsigned int)frameStats->environmentSceneDrawCalls,
                        (unsigned int)frameStats->environmentEmissiveDrawCalls,
                        /* Dawn is midnight plus six, so phase zero reads 06:00
                           and the clock agrees with the sky. */
                        ((int)(game->dayPhase * 24.0f) + 6) % 24,
                        (int)(game->dayPhase * 1440.0f) % 60,
                        GameDaylightAt(game->dayPhase) > 0.5f ? "DAY" : "NIGHT",
                        (unsigned int)frameStats->skyClouds,
                        (unsigned int)frameStats->skyStars,
                        frameStats->skySpaceVisible ? " ORBIT" : ""),
             24, 261, 14, (Color){184, 210, 162, 255});
    /* The seed is here so that a bug report is reproducible: it plus the
       inputs is the whole state of a session. */
    DrawText(TextFormat("SEED: 0x%llx | BIOME: %s",
                        (unsigned long long)game->worldSeed,
                        WorldBiomeName(WorldBiomeAt(world,
                                                    (int)player->position.x))),
             24, 279, 14, (Color){186, 194, 205, 255});
    if (cooldown <= 0.0f) {
        DrawText("PUNCH: READY", 24, 297, 14, LIME);
    } else {
        DrawText(TextFormat("PUNCH: %.2fs", cooldown), 24, 297, 14, LIGHTGRAY);
    }
}

/* Composed from the ability table rather than written out, so a new power
   appears in the hint the moment it is defined and bound. */
static void DrawControlsHint(void)
{
    const char *hint = "WASD fly  |  Shift boost/drill";
    int fontSize = 18;
    int id;
    int width;
    int x;
    int y;

    for (id = 0; id < ABILITY_COUNT; ++id) {
        const char *binding = InputAbilityBinding((AbilityId)id);

        /* An ability with no control is not one of the player's, and listing it
           would promise something the buttons cannot deliver. */
        if (binding == NULL) {
            continue;
        }
        hint = TextFormat("%s  |  %s %s", hint, binding,
                          AbilityDefinitionAt((AbilityId)id)->name);
    }
    hint = TextFormat("%s  |  RMB grab terrain  |  R regenerate  |  F1 HUD",
                      hint);
    width = MeasureText(hint, fontSize);
    x = (GetScreenWidth() - width) / 2;
    y = GetScreenHeight() - 34;

    DrawRectangle(x - 10, y - 5, width + 20, fontSize + 10, (Color){3, 6, 12, 190});
    DrawText(hint, x, y, fontSize, (Color){214, 221, 229, 255});
}

/* Presentation consumes transient gameplay facts after GameUpdate. Gameplay
   neither plays sounds nor shakes a Camera2D, and adding another consumer does
   not require another one-frame flag on Player or PowerSystem. */
static void PresentGameAudio(const GameEventBuffer *events, GameAudio *audio)
{
    uint16_t index;

    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];

        switch (event->type) {
        case GAME_EVENT_BOOST_ENGAGED:
            GameAudioPlayBoost(audio);
            break;
        case GAME_EVENT_PLAYER_IMPACT:
            GameAudioPlayImpact(audio, event->strength);
            break;
        case GAME_EVENT_FORCE:
            GameAudioPlayForce(audio);
            break;
        case GAME_EVENT_EXPLOSION:
            GameAudioPlayExplosion(audio, event->strength);
            break;
        case GAME_EVENT_LASER_HIT:
            GameAudioPlayLaserImpact(audio, 1.0f);
            break;
        case GAME_EVENT_CRYO_HIT:
            GameAudioPlayChillImpact(audio);
            break;
        case GAME_EVENT_MATERIAL_REACTION:
            GameAudioPlayReaction(audio);
            break;
        case GAME_EVENT_LIQUID_SPLASH:
            GameAudioPlaySplash(audio, event->strength +
                                           (float)event->count * 0.25f);
            break;
        case GAME_EVENT_REENTRY:
            /* Only the character's own burn: the air roaring around a slab
               half a map away is not something the character can hear. */
            if (event->count == 0) {
                GameAudioPlayReentry(audio, event->strength);
            }
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
    CameraFeedback cameraFeedback = {0};
    Camera2D stableCamera = {0};
    EnvironmentPalette environmentPalette = ENVIRONMENT_PALETTE_AUTO;
    /* Static rather than on the frame: the run keeps a few hundred bytes of
       measurements, and main's stack is not where they belong. */
    static SmokeTest smoke;
    bool debugHud = true;
    bool smokeTest = false;
    int argument;
    Vector2 cameraFocus;
    /* Where the character stood at the end of the last frame: a jump further
       than a flight could make in one is a teleport, and the camera is put
       on it rather than sent chasing it across a world four thousand cells
       tall. */
    Vector2 lastPlayerPosition;
    ViewZoom viewZoom = {1.0f, 1.0f};
    int exitCode = 0;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--smoke-test") == 0) {
            smokeTest = true;
        } else if (strcmp(argv[argument], "--seed") == 0 && argument + 1 < argc) {
            /* Replays a reported world exactly. strtoull takes 0x forms, which
               is how the debug HUD prints the seed. */
            config.seed = strtoull(argv[++argument], NULL, 0);
        } else if (strcmp(argv[argument], "--palette") == 0 &&
                   argument + 1 < argc) {
            if (!EnvironmentPaletteParse(argv[++argument],
                                         &environmentPalette)) {
                fprintf(stderr,
                        "unknown palette '%s' (expected auto, ember, abyss, "
                        "or storm)\n",
                        argv[argument]);
                return 1;
            }
        } else {
            fprintf(stderr,
                    "usage: %s [--smoke-test] [--seed VALUE] "
                    "[--palette auto|ember|abyss|storm]\n",
                    argv[0]);
            return 1;
        }
    }
    /* The smoke test must produce the same frame every run, or its reference
       screenshot is worthless as a comparison. */
    if (smokeTest && config.seed == 0u) {
        config.seed = 0x00e6be11u;
    }

    /* The smoke run steps fixed ticks and never looks at the clock, so it
       has no use for vsync or a frame cap — and both make it hostage to the
       desktop: a compositor presents an unfocused window at one frame a
       second, and a run that waits for the swap takes seven minutes instead
       of ten seconds. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | (smokeTest ? 0u : FLAG_VSYNC_HINT));
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "EMBERFALL - pixel physics sandbox");
    if (!IsWindowReady()) {
        fprintf(stderr, "Failed to initialize the raylib window.\n");
        return 1;
    }

    SetWindowMinSize(640, 360);
    SetTargetFPS(smokeTest ? 0 : 120);
    SetExitKey(KEY_ESCAPE);
    (void)GameAudioInit(&audio);

    if (!GameInit(&game, config) ||
        !RendererInit(&renderer, &game, environmentPalette)) {
        fprintf(stderr, "Failed to allocate or initialize the world.\n");
        RendererUnload(&renderer);
        GameUnload(&game);
        GameAudioUnload(&audio);
        CloseWindow();
        return 1;
    }

    if (smokeTest) {
        SmokeTestPrepare(&smoke, &game);
    }
    cameraFocus = game.player.position;
    lastPlayerPosition = game.player.position;
    CameraFeedbackInit(&cameraFeedback);
    stableCamera.target = cameraFocus;
    stableCamera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                                    (float)GetScreenHeight() * 0.5f};
    stableCamera.rotation = 0.0f;
    stableCamera.zoom = CameraZoomForWindow(1.0f);

    while (!WindowShouldClose()) {
        /* The smoke run steps at exactly one fixed tick per frame. Real frame
           time makes the number of simulation ticks a frame runs depend on how
           busy the machine is, which turns every assertion about where a body
           got to into a coin flip — and the reference screenshot with it. */
        float deltaTime = smokeTest ? game.config.fixedStep
                                    : fminf(GetFrameTime(), 0.05f);
        AppInput input;
        Camera2D aimCamera;
        Camera2D presentationCamera;
        Vector2 desiredCamera;
        CameraFeedbackOutput cameraOutput;
        Vector2 cursorCell;
        Vector2 aimPosition;

        if (smokeTest) {
            SmokeTestBeginFrame(&smoke, &game, &renderer, &cameraFeedback);
        }
        /* Input and the reticle share this exact stable transform. Camera
           impulses are applied later to a copy and therefore cannot leak back
           through GetScreenToWorld2D on the next frame. */
        stableCamera.offset =
            (Vector2){(float)GetScreenWidth() * 0.5f,
                      (float)GetScreenHeight() * 0.5f};
        stableCamera.rotation = 0.0f;
        stableCamera.zoom =
            CameraZoomForWindow(cameraFeedback.viewScale * viewZoom.current);
        stableCamera.target = ClampCameraTarget(
            stableCamera.target, stableCamera.zoom, &game.world);
        aimCamera = stableCamera;
        input = InputPoll(&game.world, aimCamera);
        if (!smokeTest) {
            ViewZoomUpdate(&viewZoom, input.zoomSteps, deltaTime);
        }
        cursorCell = input.cursorCell;
        aimPosition = input.game.aimWorld;
        if (input.toggleDebugPressed) {
            debugHud = !debugHud;
        }
        if (smokeTest) {
            SmokeTestScriptInput(&smoke, &game, &input, &aimPosition, &cursorCell);
        }

        GameUpdate(&game, &input.game, deltaTime, &events);
        if (input.game.regeneratePressed) {
            cameraFocus = game.player.position;
            CameraFeedbackClear(&cameraFeedback);
            RendererClearPresentation(&renderer);
        } else if (Vector2Distance(game.player.position, lastPlayerPosition) >
                   VIEW_HEIGHT * 0.66f) {
            /* Boost covers under forty cells in the longest frame; two thirds
               of a screen in one frame is a teleport, never flight. */
            cameraFocus = game.player.position;
        }
        lastPlayerPosition = game.player.position;
        {
            GameAudioState sounding = {0};

            sounding.laser = AbilityStateAt(&game.abilities, ABILITY_LASER)->active;
            sounding.drilling = game.player.drilledCells > 0;
            sounding.drillMaterial = game.player.drillMaterial;
            sounding.chill = AbilityStateAt(&game.abilities, ABILITY_CRYO)->active;
            GameAudioUpdate(&audio, sounding, deltaTime);
        }
        PresentGameAudio(&events, &audio);
        CameraFeedbackConsumeEvents(&cameraFeedback, &events,
                                    game.player.position);
        RendererUpdatePresentation(&renderer, &events, deltaTime);
        if (smokeTest) {
            SmokeTestObserveUpdate(&smoke, &game, &events);
        }

        cameraOutput = CameraFeedbackUpdate(
            &cameraFeedback,
            (CameraFeedbackMotion){
                .velocity = game.player.velocity,
                .normalSpeed = game.player.maxSpeed,
                .maximumSpeed = game.player.boostSpeed,
                .viewWidth = VIEW_WIDTH,
                .viewHeight = VIEW_HEIGHT,
            },
            deltaTime);
        if (smokeTest) {
            SmokeTestObserveCamera(&smoke, cameraOutput);
        }
        /* Widening belongs to the stable camera used on the next input frame.
           The immediate kick is applied only to the presentation copy below,
           so transient feedback never changes mouse-to-world conversion. */
        {
            Vector2 lead = {
                game.player.position.x + cameraOutput.lookahead.x,
                game.player.position.y + cameraOutput.lookahead.y};

            desiredCamera = ClampCameraTarget(
                lead,
                CameraZoomForWindow(cameraOutput.viewScale * viewZoom.current),
                &game.world);
        }
        cameraFocus.x += (desiredCamera.x - cameraFocus.x) *
                         (1.0f - expf(-8.0f * deltaTime));
        cameraFocus.y += (desiredCamera.y - cameraFocus.y) *
                         (1.0f - expf(-8.0f * deltaTime));
        stableCamera.zoom =
            CameraZoomForWindow(cameraOutput.viewScale * viewZoom.current);
        stableCamera.target = ClampCameraTarget(cameraFocus, stableCamera.zoom,
                                                &game.world);
        presentationCamera =
            CameraFeedbackApplyTransient(aimCamera, cameraOutput);
        presentationCamera.target = ClampCameraTarget(
            presentationCamera.target, presentationCamera.zoom, &game.world);

        /* The player carries their own light. Without it a bored tunnel would
           be unplayably dark the moment it leaves the reach of daylight, and
           the drill would be a way to blind yourself. It brightens with the
           boost, so the fastest flight also lights the furthest. */
        WorldSetPointLight(&game.world, game.player.position,
                           game.player.boosting ? 96.0f : 52.0f,
                           game.player.boosting ? 0.90f : 0.72f);

        RendererRenderScene(&renderer, &game, presentationCamera, aimCamera,
                            aimPosition,
                            VisibleWorldRectangle(presentationCamera));
        if (smokeTest) {
            SmokeTestObserveRender(&smoke, &game, &renderer, presentationCamera,
                                   aimCamera, cameraOutput);
        }

        BeginDrawing();
        /* This clear is intentionally retained as a safe backbuffer fallback
           if RendererComposite ever has no valid scene target to draw. */
        ClearBackground((Color){2, 4, 9, 255});
        RendererComposite(&renderer);
        if (debugHud && !(smokeTest && SmokeTestHidesHud(&smoke))) {
            DrawDebugHud(&game, &events, &renderer, cursorCell);
        }
        DrawControlsHint();
        if (smokeTest) {
            SmokeTestCapture(&smoke);
        }
        EndDrawing();

        if (smokeTest && SmokeTestAdvance(&smoke)) {
            break;
        }
    }

    if (smokeTest) {
        exitCode = SmokeTestReport(&smoke, &game, &renderer);
    }
    RendererUnload(&renderer);
    GameUnload(&game);
    GameAudioUnload(&audio);
    CloseWindow();
    return exitCode;
}
