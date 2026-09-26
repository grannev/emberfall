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
#include "menu.h"
#include "renderer.h"
#include "settings.h"
#include "smoke_test.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define VIEW_WIDTH 426.0f
#define VIEW_HEIGHT 240.0f
/* How far the wheel may take the view: a scale of the logical 426x240 view,
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
    if (player->mode == PLAYER_MODE_WALK) {
        if (!player->grounded) {
            return "JUMP";
        }
        return fabsf(player->velocity.x) > PLAYER_WALK_SPEED + 4.0f
                   ? "RUN"
                   : (fabsf(player->velocity.x) > 4.0f ? "WALK" : "STAND");
    }
    if (!player->boosting) {
        return "HOVER";
    }
    return speed >= player->sonicSpeed ? "MACH" : "BOOST";
}

/* Held inside the world's rows only: across, the world wraps and the camera
   follows the character over the seam like over any other column. */
static Vector2 ClampCameraTarget(Vector2 target, float zoom, const World *world)
{
    float halfHeight = (float)GetScreenHeight() / (2.0f * zoom);

    if (halfHeight * 2.0f >= (float)world->height) {
        target.y = (float)world->height * 0.5f;
    } else {
        target.y = Clamp(target.y, halfHeight, (float)world->height - halfHeight);
    }
    return target;
}

static void DrawDebugHud(const GameState *game, const GameEventBuffer *events,
                         const Renderer *renderer, Vector2 cursorCell,
                         AbilityId selected)
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
        /* The power the wheel has selected, the one LMB will fire — the
           same one the corner widget names. The last one fired is not
           what the player is holding. */
        const char *binding = InputAbilityBinding(selected);

        DrawText(TextFormat("POWER: %s (%s)", AbilityDefinitionAt(selected)->name,
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
    DrawText(TextFormat("ENV: %s | %u+%u DRAWS | %02d:%02d %s | CLOUDS %u | SPACE %d%%",
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
                        (int)(frameStats->spaceAmount * 100.0f)),
             24, 261, 14, (Color){184, 210, 162, 255});
    /* The seed is here so that a bug report is reproducible: it plus the
       inputs is the whole state of a session. */
    {
        WeatherSample weather = WeatherAt(&game->weather, world, player->position.x);

        DrawText(TextFormat("SEED: 0x%llx | BIOME: %s | WEATHER: %s %d%% | WIND %+.0f | HABITAT: %s",
                            (unsigned long long)game->worldSeed,
                            WorldBiomeName(WorldBiomeAt(world, (int)player->position.x)),
                            WeatherKindName(weather.kind),
                            (int)(weather.intensity * 100.0f), (double)world->wind,
                            game->fauna.nearest >= 0
                                ? FaunaKindName((FaunaKind)game->fauna.nearest)
                                : "-"),
                 24, 279, 14, (Color){186, 194, 205, 255});
    }
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
    const char *hint = "AD walk | Space jump / fly | Shift boost | TAB powers | LMB use | RMB grab";
    int fontSize = GetScreenWidth() < 1000 ? 12 : 16;
    int width;
    int x;
    int y;

    width = MeasureText(hint, fontSize);
    x = (GetScreenWidth() - width) / 2;
    y = GetScreenHeight() - 34;

    DrawRectangle(x - 10, y - 5, width + 20, fontSize + 10, (Color){3, 6, 12, 190});
    DrawText(hint, x, y, fontSize, (Color){214, 221, 229, 255});
}

static Color AbilityColor(AbilityId id)
{
    if (id == ABILITY_NUCLEAR) return (Color){247, 179, 76, 255};
    if (id == ABILITY_LASER) return (Color){241, 91, 77, 255};
    if (id == ABILITY_CRYO) return (Color){119, 209, 234, 255};
    return (Color){239, 203, 146, 255};
}

/* Small pixel glyphs remain readable at the same scale as the world. */
static void DrawAbilityIcon(AbilityId id, Vector2 at, float scale, Color color)
{
    static const unsigned char punch[7] = {14, 31, 31, 31, 30, 14, 12};
    static const unsigned char laser[7] = {0, 16, 8, 31, 2, 1, 0};
    static const unsigned char cryo[7] = {4, 21, 14, 31, 14, 21, 4};
    static const unsigned char nuclear[7] = {17, 27, 14, 4, 14, 27, 17};
    const unsigned char *glyph = id == ABILITY_NUCLEAR ? nuclear : id == ABILITY_LASER ? laser :
                                 (id == ABILITY_CRYO ? cryo : punch);
    int y;

    for (y = 0; y < 7; ++y) {
        int x;
        for (x = 0; x < 5; ++x) {
            if ((glyph[y] & (1u << (unsigned int)(4 - x))) != 0) {
                DrawRectangleV((Vector2){at.x + ((float)x - 2.5f) * scale,
                                          at.y + ((float)y - 3.5f) * scale},
                               (Vector2){scale, scale}, color);
            }
        }
    }
}

static void DrawAbilitySelection(const AbilitySelection *selection,
                                  const AbilitySystem *abilities)
{
    float scale = fminf((float)GetScreenWidth() / 1280.0f,
                        (float)GetScreenHeight() / 720.0f);
    Color accent = AbilityColor(selection->selected);
    const AbilityState *state = AbilityStateAt(abilities, selection->selected);
    int font = (int)(18.0f * scale);

    if (!selection->open) {
        float y = (float)GetScreenHeight() - 92.0f * scale;
        DrawRectangle(18, (int)y, (int)(242.0f * scale), (int)(48.0f * scale),
                      (Color){12, 17, 24, 224});
        DrawRectangle(18, (int)y, (int)(3.0f * scale), (int)(48.0f * scale), accent);
        DrawAbilityIcon(selection->selected, (Vector2){18.0f + 25.0f * scale,
                                                       y + 24.0f * scale}, 3.0f * scale, accent);
        DrawText(AbilityDefinitionAt(selection->selected)->name,
                 (int)(18.0f + 48.0f * scale), (int)(y + 7.0f * scale), font, RAYWHITE);
        const char *hint = state->cooldown > 0.0f ? "RECOVERING" :
                          (selection->selected == ABILITY_NUCLEAR ?
                           "HOLD LMB / RELEASE TO FIRE" : "LMB  /  TAB to select");
        if (selection->selected == ABILITY_NUCLEAR && state->active) {
            float charge = Clamp(state->chargeTime / ABILITY_NUCLEAR_CHARGE_TIME, 0.0f, 1.0f);
            hint = TextFormat("CHARGE %d%% / RELEASE", (int)roundf(charge * 100.0f));
            DrawRectangle(18, (int)(y + 46.0f * scale),
                          (int)(242.0f * scale * charge), (int)(2.0f * scale), accent);
        }
        DrawText(hint,
                 (int)(18.0f + 48.0f * scale), (int)(y + 29.0f * scale),
                 (int)(12.0f * scale), accent);
        return;
    }
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){3, 7, 13, 144});
    {
        int count = InputAbilityCount();
        int index;
        Vector2 center = selection->center;
        for (index = 0; index < count; ++index) {
            AbilityId id = InputAbilityAt(index);
            float angle = -90.0f + (float)index * 360.0f / (float)count;
            float half = 180.0f / (float)count - 2.0f;
            bool hovered = selection->hovered == index;
            Vector2 label = {center.x + cosf(angle * DEG2RAD) * 111.0f * scale,
                             center.y + sinf(angle * DEG2RAD) * 111.0f * scale};
            Color color = AbilityColor(id);
            const char *name = AbilityDefinitionAt(id)->name;
            DrawRing(center, 49.0f * scale, 162.0f * scale, angle - half,
                     angle + half, 40, hovered ? (Color){53, 64, 76, 248} :
                                                (Color){16, 23, 33, 242});
            DrawRing(center, 158.0f * scale, 162.0f * scale, angle - half,
                     angle + half, 40, hovered || id == selection->selected ? color :
                                                (Color){68, 78, 91, 255});
            DrawAbilityIcon(id, (Vector2){label.x, label.y - 12.0f * scale},
                            4.0f * scale, color);
            DrawText(name, (int)label.x - MeasureText(name, font) / 2,
                     (int)(label.y + 14.0f * scale), font, RAYWHITE);
        }
        DrawText("TAB", (int)center.x - MeasureText("TAB", font) / 2,
                 (int)center.y - font / 2, font, RAYWHITE);
        {
            const char *hint = "Point to a power / release TAB / LMB to use";
            DrawText(hint, (int)center.x - MeasureText(hint, font) / 2,
                     (int)(center.y + 188.0f * scale), font, RAYWHITE);
        }
    }
}

/* Puts the settings into effect. Every one of them is presentation or
   platform; none reaches the simulation. The smoke run keeps its own fixed
   configuration and is never touched by a player's file. */
static void ApplySettings(const Settings *settings)
{
    bool borderless = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);

    SetMasterVolume(settings->masterVolume);
    SetTargetFPS(SettingsFrameLimitRate(settings->frameLimit));
    if (settings->vsync) {
        SetWindowState(FLAG_VSYNC_HINT);
    } else {
        ClearWindowState(FLAG_VSYNC_HINT);
    }
    if (settings->fullscreen != borderless) {
        ToggleBorderlessWindowed();
    }
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
        case GAME_EVENT_HEAVY_LANDING:
            GameAudioPlayImpact(audio, 180.0f + event->strength * 380.0f);
            GameAudioPlayExplosion(audio, 60.0f + event->strength * 140.0f);
            break;
        case GAME_EVENT_BOOST_ENGAGED:
            GameAudioPlayBoost(audio);
            break;
        case GAME_EVENT_SONIC_BREAK:
            GameAudioPlaySonic(audio);
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
    Settings settings = SettingsDefaults();
    char settingsPath[512];
    bool settingsFile = false;
    Menu menu;
    bool menuOpen = false;
    bool quit = false;
    bool smokeTest = false;
    int argument;
    Vector2 cameraFocus;
    /* Where the character stood at the end of the last frame: a jump further
       than a flight could make in one is a teleport, and the camera is put
       on it rather than sent chasing it across a world four thousand cells
       tall. */
    Vector2 lastPlayerPosition;
    ViewZoom viewZoom = {1.0f, 1.0f};
    AbilitySelection selection = {.selected = ABILITY_FORCE, .hovered = -1};
    /* The cursor and the aim survive a frame spent in the menu: the world
       under it is drawn as the last frame of play left it. */
    Vector2 cursorCell = {0.0f, 0.0f};
    Vector2 aimPosition = {0.0f, 0.0f};
    int exitCode = 0;

    for (argument = 1; argument < argc; ++argument) {
        if (strcmp(argv[argument], "--smoke-test") == 0) {
            smokeTest = true;
        } else if (strcmp(argv[argument], "--weather") == 0 && argument + 1 < argc) {
            /* Holds one kind of weather everywhere: rain, storm, snow,
               blizzard, sandstorm, ashfall, cloudy, clear. */
            config.forcedWeather = WeatherKindParse(argv[++argument]);
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
    /* The player's settings, read before the window exists so vsync can be
       asked for when it is created. */
    if (!smokeTest && SettingsPath(settingsPath, sizeof(settingsPath))) {
        settingsFile = true;
        (void)SettingsLoad(&settings, settingsPath);
    }
    SetConfigFlags(FLAG_WINDOW_RESIZABLE |
                   (!smokeTest && settings.vsync ? FLAG_VSYNC_HINT : 0u));
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "EMBERFALL - pixel physics sandbox");
    if (!IsWindowReady()) {
        fprintf(stderr, "Failed to initialize the raylib window.\n");
        return 1;
    }

    SetWindowMinSize(640, 360);
    SetTargetFPS(smokeTest ? 0 : 120);
    /* Escape opens the menu; the menu's QUIT and the window's close button
       are the ways out. */
    SetExitKey(KEY_NULL);
    (void)GameAudioInit(&audio);
    if (!smokeTest) {
        ApplySettings(&settings);
    }

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
    MenuInit(&menu);
    menu.worldSeed = game.worldSeed;
    /* The game opens on the menu, over the world it has just made; the smoke
       run goes straight to play. */
    menuOpen = !smokeTest;
    cameraFocus = game.player.position;
    lastPlayerPosition = game.player.position;
    aimPosition = (Vector2){game.player.position.x + 24.0f, game.player.position.y};
    CameraFeedbackInit(&cameraFeedback);
    stableCamera.target = cameraFocus;
    stableCamera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                                    (float)GetScreenHeight() * 0.5f};
    stableCamera.rotation = 0.0f;
    stableCamera.zoom = CameraZoomForWindow(1.0f);

    while (!WindowShouldClose() && !quit) {
        /* The smoke run steps at exactly one fixed tick per frame. Real frame
           time makes the number of simulation ticks a frame runs depend on how
           busy the machine is, which turns every assertion about where a body
           got to into a coin flip — and the reference screenshot with it. */
        float deltaTime = smokeTest ? game.config.fixedStep
                                    : fminf(GetFrameTime(), 0.05f);
        Camera2D aimCamera;
        Camera2D presentationCamera;
        CameraFeedbackOutput cameraOutput = {0};

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
        presentationCamera = aimCamera;

        if (menuOpen) {
            MenuInput menuInput = InputPollMenu();
            MenuAction action = MenuUpdate(&menu, &menuInput, &settings,
                                           GetScreenWidth(), GetScreenHeight(),
                                           deltaTime);

            /* The world waits under the menu: nothing is simulated and nothing
               sounds. Before the first world is entered the camera drifts
               slowly along it, so the menu opens onto a place rather than a
               still. */
            GameEventsClear(&events);
            GameAudioUpdate(&audio, (GameAudioState){0}, deltaTime);
            if (!menu.worldEntered) {
                cameraFocus.x += 14.0f * deltaTime;
                stableCamera.target =
                    ClampCameraTarget(cameraFocus, stableCamera.zoom, &game.world);
                presentationCamera = stableCamera;
            }
            switch (action.type) {
            case MENU_ACTION_CONTINUE:
                menuOpen = false;
                if (!menu.worldEntered) {
                    cameraFocus = game.player.position;
                }
                menu.worldEntered = true;
                break;
            case MENU_ACTION_NEW_WORLD:
                if (action.seeded) {
                    GameReset(&game, action.seed);
                } else {
                    GameRegenerate(&game);
                }
                menu.worldSeed = game.worldSeed;
                menu.worldEntered = true;
                menuOpen = false;
                cameraFocus = game.player.position;
                lastPlayerPosition = game.player.position;
                stableCamera.target = cameraFocus;
                CameraFeedbackClear(&cameraFeedback);
                RendererClearPresentation(&renderer);
                break;
            case MENU_ACTION_SETTINGS_CHANGED:
                ApplySettings(&settings);
                if (settingsFile) {
                    (void)SettingsSave(&settings, settingsPath);
                }
                break;
            case MENU_ACTION_QUIT:
                quit = true;
                break;
            case MENU_ACTION_NONE:
            default:
                break;
            }
        } else {
            AppInput input = InputPoll(&game.world, aimCamera, &selection);
            Vector2 desiredCamera;

            if (!smokeTest) {
                ViewZoomUpdate(&viewZoom, input.zoomSteps * settings.zoomSpeed,
                               deltaTime);
            }
            cursorCell = input.cursorCell;
            aimPosition = input.game.aimWorld;
            if (input.toggleDebugPressed) {
                settings.showDebugHud = !settings.showDebugHud;
                if (settingsFile) {
                    (void)SettingsSave(&settings, settingsPath);
                }
            }
            if (smokeTest) {
                SmokeTestScriptInput(&smoke, &game, &input, &aimPosition, &cursorCell);
            }

            GameUpdate(&game, &input.game, deltaTime, &events);
            /* The character crossed the seam and was moved back into the
               map a whole width; the camera, the aim and every effect on
               screen go with it, so the frame does not move at all. */
            if (game.wrapShift != 0.0f) {
                cameraFocus.x += game.wrapShift;
                stableCamera.target.x += game.wrapShift;
                lastPlayerPosition.x += game.wrapShift;
                aimPosition.x += game.wrapShift;
                RendererShiftPresentation(&renderer, game.wrapShift);
            }
            if (input.game.regeneratePressed) {
                cameraFocus = game.player.position;
                menu.worldSeed = game.worldSeed;
                CameraFeedbackClear(&cameraFeedback);
                RendererClearPresentation(&renderer);
            } else if (Vector2Distance(game.player.position, lastPlayerPosition) >
                       VIEW_HEIGHT * 0.66f) {
                /* Boost covers under forty cells in the longest frame; two
                   thirds of a screen in one frame is a teleport, never
                   flight. */
                cameraFocus = game.player.position;
            }
            lastPlayerPosition = game.player.position;
            {
                GameAudioState sounding = {0};

                sounding.laser = AbilityStateAt(&game.abilities, ABILITY_LASER)->active ||
                                  AbilityStateAt(&game.abilities, ABILITY_NUCLEAR)->active;
                sounding.drilling = game.player.drilledCells > 0;
                sounding.drillMaterial = game.player.drillMaterial;
                sounding.chill = AbilityStateAt(&game.abilities, ABILITY_CRYO)->active;
                {
                    WeatherSample weather = WeatherAt(&game.weather, &game.world,
                                                      game.player.position.x);

                    sounding.windStrength = fabsf(game.world.wind);
                    sounding.rainIntensity =
                        weather.kind == WEATHER_RAIN || weather.kind == WEATHER_STORM
                            ? weather.intensity
                            : 0.0f;
                    sounding.thunder = renderer.weather.thunderThisFrame;
                    sounding.leavesRustle = game.player.brushedLeaves > 0;
                    sounding.sandBlowing = weather.kind == WEATHER_SANDSTORM;
                    sounding.fauna = game.fauna.nearest;
                }
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
            /* The player's own taste in shake. Scaled here, after the
               smoke run has seen the real output, and never below what
               widening at speed needs: the view scale is not shake. */
            if (!smokeTest) {
                cameraOutput.impulseOffset.x *= settings.screenShake;
                cameraOutput.impulseOffset.y *= settings.screenShake;
                cameraOutput.rotationDegrees *= settings.screenShake;
                cameraOutput.zoomKick *= settings.screenShake;
            }
            /* Widening belongs to the stable camera used on the next input
               frame. The immediate kick is applied only to the presentation
               copy below, so transient feedback never changes mouse-to-world
               conversion. */
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

            /* Escape opens the menu at the end of the frame, over the world
               exactly as this frame left it. */
            if (input.menuPressed && !smokeTest) {
                AbilitiesCancelCharge(&game.abilities);
                selection.open = false;
                selection.blockPrimary = true;
                menuOpen = true;
                MenuOpen(&menu);
            }
        }

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
        if (smokeTest && (smoke.frame == 5 || smoke.frame == 6)) {
            /* The smoke run never opens the menu, so it is photographed
               here, drawn over a frame of play: the main screen, then the
               settings. */
            Menu shown = menu;

            shown.screen = smoke.frame == 5 ? MENU_SCREEN_MAIN : MENU_SCREEN_SETTINGS;
            shown.selected = smoke.frame == 5 ? 0 : 1;
            MenuDraw(&shown, &settings, GetScreenWidth(), GetScreenHeight());
        } else if (menuOpen) {
            MenuDraw(&menu, &settings, GetScreenWidth(), GetScreenHeight());
        } else {
            if ((smokeTest || settings.showDebugHud) &&
                !(smokeTest && SmokeTestHidesHud(&smoke))) {
                DrawDebugHud(&game, &events, &renderer, cursorCell, selection.selected);
            }
            if (smokeTest || settings.showControls) {
                DrawControlsHint();
            }
            if (!smokeTest) DrawAbilitySelection(&selection, &game.abilities);
        }
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
