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
#include "terrain_extraction.h"
#include "world_components.h"

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define VIEW_WIDTH 320.0f
#define VIEW_HEIGHT 180.0f

static float CameraZoomForWindow(float viewScale)
{
    if (!isfinite(viewScale) || viewScale < 1.0f) {
        viewScale = 1.0f;
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
    DrawText(TextFormat("RENDER: %u UPLOADS  %.1f KiB  %.2f ms | PAGES: %u/%u +%u"
,
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
    DrawText(TextFormat("HOLD: %s | CUT %d CELLS %d SPLITS",
                        TerrainInteractionIsHolding(&game->interaction,
                                                    &game->dynamicTerrain)
                            ? "CARRYING (F)"
                            : (DynamicTerrainGetConst(&game->dynamicTerrain,
                                                      game->interaction.hovered) !=
                                       NULL
                                   ? "READY (F)"
                                   : "-"),
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
    const char *hint = "WASD fly  |  Shift staged boost/drill";
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

/* An island extracted explicitly, given a visible transform, and freed on the
   last frame so the run also proves the generation-keyed cache releases its
   textures. This is the renderer's showcase and stays deliberately separate
   from the automatic-detach one below, which proves the gameplay path instead.
   Nothing here runs outside --smoke-test. */
static TerrainBodyHandle SetupSmokeTerrainBody(GameState *game, Vector2 aim,
                                               bool *worldCellsCleared)
{
    WorldComponentWorkspace workspace;
    TerrainExtractResult extracted;
    WorldComponentResult component;
    TerrainBodyHandle invalid = TerrainBodyInvalidHandle();
    TerrainBody *body;
    int originX = (int)aim.x - 70;
    int originY = (int)aim.y - 52;
    int localY;
    int index;

    *worldCellsCleared = false;
    for (localY = -2; localY <= 7; ++localY) {
        int localX;

        for (localX = -2; localX <= 13; ++localX) {
            WorldSetCell(&game->world, originX + localX, originY + localY,
                         MATERIAL_EMPTY);
        }
    }
    for (localY = 0; localY < 6; ++localY) {
        int localX;

        for (localX = 0; localX < 12; ++localX) {
            CellMaterial material;

            /* Cut the four corners and one interior pixel so rotation exposes
               the transparent raster rather than a plain rectangle. */
            if ((localY == 0 || localY == 5) &&
                (localX == 0 || localX == 11)) {
                continue;
            }
            if (localX == 5 && localY == 2) {
                continue;
            }
            material = localX < 4 ? MATERIAL_DIRT
                                  : (localX < 9 ? MATERIAL_ROCK : MATERIAL_ICE);
            WorldSetCell(&game->world, originX + localX, originY + localY,
                         material);
            if (material == MATERIAL_ROCK &&
                (localY == 2 || localY == 3)) {
                WorldSetTemperature(&game->world, originX + localX,
                                    originY + localY, 540.0f);
            }
        }
    }

    component = WorldFindComponent(
        &game->world, &workspace,
        (Rectangle){(float)(originX - 1), (float)(originY - 1), 14.0f, 8.0f},
        originX + 2, originY, WORLD_COMPONENT_MAX_CELLS);
    if (component.status != WORLD_COMPONENT_DETACHED) {
        return invalid;
    }
    extracted = TerrainExtractComponent(&game->world, &game->dynamicTerrain,
                                        &workspace, component);
    if (extracted.status != TERRAIN_EXTRACT_OK) {
        return invalid;
    }

    *worldCellsCleared = true;
    for (index = 0; index < component.cellCount; ++index) {
        if (WorldGetCell(&game->world, (int)workspace.cellX[index],
                        (int)workspace.cellY[index]) != MATERIAL_EMPTY) {
            *worldCellsCleared = false;
            break;
        }
    }

    body = DynamicTerrainGet(&game->dynamicTerrain, extracted.body);
    if (body == NULL) {
        return invalid;
    }
    /* Collision is already part of the production fixed step. A short shelf
       below the extracted island makes the renderer showcase exercise that
       shared transform at contact instead of only showing free flight. It is
       placed after extraction and outside the detector region, so it cannot
       become part of the detached component. */
    for (index = -6; index <= 17; ++index) {
        WorldSetCell(&game->world, originX + index, originY + 10,
                     MATERIAL_ROCK);
        WorldSetCell(&game->world, originX + index, originY + 11,
                     MATERIAL_ROCK);
    }
    body->angle = 0.18f;
    DynamicTerrainSetVelocity(&game->dynamicTerrain, extracted.body,
                              (Vector2){26.0f, 78.0f}, 1.8f);
    return extracted.body;
}

/* The acceptance shape for automatic detachment and for ability impulses, built
   where the screenshot can see it:

       ####   ##########    two blocks, one small and one large
        #          #        two thin pillars
       ##################   ground

   One blast between the pillars severs both. Nothing downstream is special-
   cased: the world logs the damage the way it logs any destructive cut, the
   fixed step detaches what came loose, and the same blast throws it. The two
   blocks differ only in size, so what the run shows is mass doing its job —
   the heavy one barely moves while the light one is flung.

   Returns the cell the blast should be centred on. */
#define SMOKE_SMALL_BLOCK_HALF 4
#define SMOKE_LARGE_BLOCK_HALF 10
#define SMOKE_BLOCK_HEIGHT_SMALL 5
#define SMOKE_BLOCK_HEIGHT_LARGE 12
#define SMOKE_PILLAR_DROP 27

static Vector2 SetupSmokeDetachScene(World *world, Vector2 aim)
{
    int originX = (int)aim.x + 44;
    int blockBottom = (int)aim.y - 20;
    int smallPillarX = originX + SMOKE_SMALL_BLOCK_HALF;
    int largePillarX = originX + 24;
    int groundTop = blockBottom + SMOKE_PILLAR_DROP;
    int x;
    int y;

    for (y = blockBottom - SMOKE_BLOCK_HEIGHT_LARGE - 8; y < groundTop; ++y) {
        for (x = originX - 10; x <= originX + 40; ++x) {
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
        }
    }
    /* The floor reaches well past the cleared air on both sides so it grows
       into the hillside that was already there. A slab that merely floated
       where the scene was built would be genuinely detached, and the detector
       would be right to take it — which is a fine result but a confusing
       screenshot. */
    for (y = groundTop; y <= groundTop + 8; ++y) {
        for (x = originX - 22; x <= originX + 52; ++x) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    for (y = blockBottom + 1; y < groundTop; ++y) {
        WorldSetCell(world, smallPillarX, y, MATERIAL_ROCK);
        WorldSetCell(world, largePillarX, y, MATERIAL_ROCK);
    }
    for (y = blockBottom - SMOKE_BLOCK_HEIGHT_SMALL + 1; y <= blockBottom; ++y) {
        for (x = smallPillarX - SMOKE_SMALL_BLOCK_HALF;
             x <= smallPillarX + SMOKE_SMALL_BLOCK_HALF; ++x) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    for (y = blockBottom - SMOKE_BLOCK_HEIGHT_LARGE + 1; y <= blockBottom; ++y) {
        for (x = largePillarX - SMOKE_LARGE_BLOCK_HALF;
             x <= largePillarX + SMOKE_LARGE_BLOCK_HALF; ++x) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    return (Vector2){(float)((smallPillarX + largePillarX) / 2),
                     (float)(blockBottom + 14)};
}

/* The largest live body, which in the showcase is unambiguously the heavy
   block. Picking "the first body over some size" would sometimes pick the
   renderer's own showcase island instead, and then the force blow would be
   aimed at wherever that had flown off to. */
static const TerrainBody *SmokeHeaviestBody(const DynamicTerrainSystem *terrain)
{
    const TerrainBody *heaviest = NULL;
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *body = &terrain->bodies[slot];

        if (!body->active) {
            continue;
        }
        if (heaviest == NULL || body->cellCount > heaviest->cellCount) {
            heaviest = body;
        }
    }
    return heaviest;
}

/* --- gameplay acceptance run --------------------------------------------- */

/* The whole point of EF-PHY-001, played through once on a script so it can be
   watched rather than only asserted:

       platform on a thin support
         -> the support is blown away
         -> the platform comes loose and falls
         -> the player walks into it and shoves it
         -> takes hold of it and drags it
         -> lets go, throwing it
         -> an explosion bites a hole in it
         -> the hole severs it and the pieces fly apart

   It runs after the render smoke has taken its reference screenshot and freed
   its bodies, so the two phases never argue about what is on screen. Frame
   numbers are deliberately plain: the run steps one fixed tick per frame, so a
   frame is a known amount of simulated time and the schedule reads as one. */
#define ACCEPT_START 12
#define ACCEPT_BLAST (ACCEPT_START + 1)
#define ACCEPT_PUSH (ACCEPT_START + 40)
#define ACCEPT_GRAB (ACCEPT_START + 70)
#define ACCEPT_RELEASE (ACCEPT_START + 130)
#define ACCEPT_CUT (ACCEPT_START + 150)
#define ACCEPT_SHOT (ACCEPT_START + 165)
#define ACCEPT_END (ACCEPT_START + 172)

typedef struct SmokeAcceptance {
    Vector2 platformAt;
    Vector2 supportAt;
    bool detached;
    bool pushed;
    bool grabbed;
    bool dragged;
    bool threw;
    bool carved;
    bool split;
    /* Presentation observed while the gameplay phase was running. The two
       features were built on separate branches, so "they both work" is not the
       same claim as "they work at the same time", and only the second is worth
       asserting. */
    bool fxDuringPlay;
    bool cameraFeedbackDuringPlay;
    float pushSpeed;
    float dragDistance;
    float throwSpeed;
    int fragments;
    Vector2 dragStart;
} SmokeAcceptance;

/* A wide slab resting on one thin column, in cleared ground so the surrounding
   world cannot join in. */
static void SetupSmokeAcceptanceScene(World *world, Vector2 centre,
                                      SmokeAcceptance *state)
{
    int columnX = (int)centre.x;
    int platformBottom = (int)centre.y;
    int groundTop = platformBottom + 28;
    int x;
    int y;

    for (y = platformBottom - 30; y < groundTop; ++y) {
        for (x = columnX - 40; x <= columnX + 40; ++x) {
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
        }
    }
    for (y = groundTop; y <= groundTop + 10; ++y) {
        for (x = columnX - 56; x <= columnX + 56; ++x) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    for (y = platformBottom + 1; y < groundTop; ++y) {
        WorldSetCell(world, columnX, y, MATERIAL_ROCK);
    }
    for (y = platformBottom - 7; y <= platformBottom; ++y) {
        for (x = columnX - 15; x <= columnX + 14; ++x) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    state->platformAt = (Vector2){(float)columnX, (float)platformBottom - 4.0f};
    state->supportAt = (Vector2){(float)columnX,
                                 (float)(platformBottom + groundTop) / 2.0f};
}

/* The largest live body, which through the whole run is the platform or, after
   the cut, the bigger of its halves. */
static const TerrainBody *SmokeAcceptanceBody(const DynamicTerrainSystem *terrain)
{
    return SmokeHeaviestBody(terrain);
}

/* Drives one frame of the script. Returns the input the frame should run
   with. */
static void RunSmokeAcceptance(GameState *game, SmokeAcceptance *state,
                               int frame, GameInput *input)
{
    const TerrainBody *body = SmokeAcceptanceBody(&game->dynamicTerrain);

    memset(input->ability, 0, sizeof(input->ability));
    input->move = (Vector2){0.0f, 0.0f};
    input->grabHeld = false;
    input->boostHeld = false;
    input->regeneratePressed = false;
    if (body != NULL) {
        input->aimWorld = body->position;
    }

    if (frame == ACCEPT_START) {
        SetupSmokeAcceptanceScene(&game->world, state->platformAt, state);
        /* Beside the scene, so the camera frames it and the player is in
           position to walk into the slab once it lands. */
        game->player.position = (Vector2){state->platformAt.x - 46.0f,
                                          state->platformAt.y + 20.0f};
        game->player.velocity = (Vector2){0.0f, 0.0f};
        return;
    }
    if (frame == ACCEPT_BLAST) {
        WorldDestroyCircle(&game->world, (int)state->supportAt.x,
                           (int)state->supportAt.y, 7, 0.0f);
        (void)TerrainImpulseQueueBlast(&game->impulses, (TerrainBlast){
            .shape = TERRAIN_BLAST_RADIAL,
            .origin = state->supportAt,
            .radius = ABILITY_EXPLOSION_SHOCK_RADIUS,
            .momentum = ABILITY_EXPLOSION_BODY_IMPULSE,
            .carveRadius = 0.0f,
        });
        return;
    }
    if (body == NULL) {
        return;
    }
    state->detached = state->detached || game->detach.stats.autoDetachSucceeded > 0;

    if (frame >= ACCEPT_PUSH && frame < ACCEPT_GRAB) {
        /* Walked into the side of the slab. Placed rather than flown so the
           contact happens at a known moment; everything the contact then does
           is the production path. */
        if (frame == ACCEPT_PUSH) {
            game->player.position = (Vector2){body->position.x - 24.0f,
                                              body->position.y};
        }
        game->player.velocity = (Vector2){52.0f, 0.0f};
        input->move = (Vector2){1.0f, 0.0f};
        if (game->interaction.stats.contacts > 0 && !state->pushed) {
            state->pushed = true;
            state->pushSpeed = body->velocity.x;
        }
        return;
    }
    if (frame >= ACCEPT_GRAB && frame < ACCEPT_RELEASE) {
        input->grabHeld = true;
        if (frame == ACCEPT_GRAB) {
            state->dragStart = body->position;
        } else {
            /* Dragged upward and to the left, away from where it was. */
            input->aimWorld = (Vector2){game->player.position.x - 18.0f,
                                        game->player.position.y - 12.0f};
        }
        state->grabbed = state->grabbed ||
                         TerrainInteractionIsHolding(&game->interaction,
                                                     &game->dynamicTerrain);
        state->dragDistance = Vector2Distance(body->position, state->dragStart);
        state->dragged = state->dragged || state->dragDistance > 6.0f;
        return;
    }
    if (frame == ACCEPT_RELEASE) {
        /* Let go while pointing away: the throw carries it off. The result is
           read on the next frame, because the release happens inside the update
           this input is being prepared for. */
        input->aimWorld = (Vector2){body->position.x + 40.0f,
                                    body->position.y - 10.0f};
        return;
    }
    if (frame == ACCEPT_RELEASE + 1) {
        state->threw = game->interaction.stats.throws > 0;
        state->throwSpeed = Vector2Length(body->velocity);
        return;
    }
    if (frame == ACCEPT_CUT) {
        /* The real power, aimed at the middle of the slab — not a blast queued
           straight into the terrain system. Going through the ability is the
           point: it publishes the event that drives the staged FX, the audio
           and the camera kick, and a run that skipped it would prove the
           gameplay works while saying nothing about whether it works *with*
           the presentation built beside it. */
        input->ability[ABILITY_EXPLOSION] = true;
        input->aimWorld = body->position;
        return;
    }
    if (frame > ACCEPT_CUT) {
        int slot;
        int live = 0;

        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            if (game->dynamicTerrain.bodies[slot].active) {
                ++live;
            }
        }
        if (live > state->fragments) {
            state->fragments = live;
        }
        state->carved = game->damage.stats.cellsCarved > 0;
        state->split = game->damage.stats.fractureSplits > 0;
    }
}

/* --- movement acceptance run ---------------------------------------------- */

/* The traversal chain EF-MOV-001 exists for, flown once on a script so it can be
   watched and measured rather than only asserted:

       still
         -> ordinary acceleration up to cruise
         -> boost, and the three stages it climbs through
         -> a wide turn at the top of the range
         -> drilling straight through a rock support
         -> braking back down out of the boost
         -> reversing
         -> stopped

   Each phase records the one number that says whether it felt right, and those
   numbers are what the tuning was done against. */
#define MOVE_START (ACCEPT_END)
#define MOVE_CRUISE (MOVE_START + 80)
#define MOVE_BOOST (MOVE_CRUISE + 5)
#define MOVE_TURN (MOVE_BOOST + 220)
#define MOVE_DRILL (MOVE_TURN + 60)
#define MOVE_BRAKE (MOVE_DRILL + 110)
#define MOVE_REVERSE (MOVE_BRAKE + 70)
#define MOVE_SHOT (MOVE_REVERSE + 50)
/* The most characteristic frame of the run: mid-tunnel, at the top of the boost
   range, with the cut trailing behind. */
#define MOVE_SHOT_FRAME (MOVE_DRILL + 22)
#define MOVE_STOP (MOVE_SHOT + 4)
#define MOVE_END (MOVE_STOP + 130)

typedef struct SmokeMovement {
    Vector2 origin;
    float cruiseSpeed;
    float stageOneSpeed;
    float stageTwoSpeed;
    float peakSpeed;
    float turnLateral;
    float brakeFrom;
    float reverseSpeed;
    float finalSpeed;
    int drilled;
    int brakeFrames;
    PlayerBoostStage topStage;
    bool turned;
    bool reversed;
    bool stopped;
} SmokeMovement;

/* Clear sky to fly through. Long enough to hold a full boost run: the player
   crosses better than two thousand cells before the turn, and flying into
   untouched world would turn the whole phase into a drilling test. */
#define MOVE_CORRIDOR_LENGTH 3000
#define MOVE_CORRIDOR_HEIGHT 100

static void SetupSmokeMovementScene(World *world, Vector2 origin)
{
    int firstX = (int)origin.x;
    int y = (int)origin.y;
    int x;
    int row;

    for (row = y - MOVE_CORRIDOR_HEIGHT; row <= y + MOVE_CORRIDOR_HEIGHT; ++row) {
        for (x = firstX - 60; x <= firstX + MOVE_CORRIDOR_LENGTH; ++x) {
            WorldSetCell(world, x, row, MATERIAL_EMPTY);
        }
    }
}

/* The support to drill through, built where the player actually is rather than
   at a coordinate guessed in advance. The trajectory is deterministic, but it
   is not something to work out on paper — and a test that misses its own
   obstacle proves nothing about drilling. */
static void SetupSmokeMovementSupport(World *world, Vector2 at, Vector2 heading)
{
    float length = sqrtf(heading.x * heading.x + heading.y * heading.y);
    int wallX;
    int y = (int)at.y;
    int row;
    int x;

    if (length < 0.001f) {
        return;
    }
    /* Far enough ahead that the player meets it at speed rather than starting
       inside it. */
    wallX = (int)(at.x + heading.x / length * 40.0f);
    for (row = y - 60; row <= y + 60; ++row) {
        for (x = wallX; x <= wallX + 11; ++x) {
            WorldSetCell(world, x, row, MATERIAL_ROCK);
        }
    }
}

static void RunSmokeMovement(GameState *game, SmokeMovement *state, int frame,
                             GameInput *input)
{
    float speed = Vector2Length(game->player.velocity);

    memset(input->ability, 0, sizeof(input->ability));
    input->grabHeld = false;
    input->regeneratePressed = false;
    input->aimWorld = (Vector2){game->player.position.x + 40.0f,
                                game->player.position.y};

    if (frame == MOVE_START) {
        SetupSmokeMovementScene(&game->world, state->origin);
        game->player.position = (Vector2){state->origin.x, state->origin.y};
        game->player.velocity = (Vector2){0.0f, 0.0f};
        input->move = (Vector2){0.0f, 0.0f};
        input->boostHeld = false;
        return;
    }

    if (frame < MOVE_CRUISE) {
        /* Ordinary flight: no boost, and it has to reach a steady cruise. */
        input->move = (Vector2){1.0f, 0.0f};
        input->boostHeld = false;
        state->cruiseSpeed = speed;
        return;
    }
    if (frame < MOVE_TURN) {
        input->move = (Vector2){1.0f, 0.0f};
        input->boostHeld = true;
        if (game->player.boostStage > state->topStage) {
            state->topStage = game->player.boostStage;
        }
        if (game->player.boostStage == PLAYER_BOOST_STAGE_ONE) {
            state->stageOneSpeed = speed;
        }
        if (game->player.boostStage == PLAYER_BOOST_STAGE_TWO) {
            state->stageTwoSpeed = speed;
        }
        if (speed > state->peakSpeed) {
            state->peakSpeed = speed;
        }
        return;
    }
    if (frame < MOVE_DRILL) {
        /* Down and then back up: an arc the player flies out of, rather than a
           dive that ends in the ground. What is recorded is how far off the
           line the turn carried them, which at this speed is the whole point —
           a fast turn is a wide one. */
        float deviation = fabsf(game->player.position.y - state->origin.y);

        input->move = (Vector2){1.0f, frame < MOVE_TURN + 30 ? 0.85f : -0.85f};
        input->boostHeld = true;
        if (deviation > state->turnLateral) {
            state->turnLateral = deviation;
        }
        state->turned = state->turnLateral > 12.0f;
        return;
    }
    if (frame < MOVE_BRAKE) {
        if (frame == MOVE_DRILL) {
            /* Levelled out before the wall goes up, so the tunnel is cut across
               open sky and the player comes out the far side into it rather
               than into the ground the dive was heading for. */
            game->player.velocity = (Vector2){speed, 0.0f};
            game->player.position.y = state->origin.y;
            SetupSmokeMovementSupport(&game->world, game->player.position,
                                      game->player.velocity);
        }
        input->move = (Vector2){1.0f, 0.0f};
        input->boostHeld = true;
        state->drilled += game->player.drilledCells;
        return;
    }
    if (frame < MOVE_REVERSE) {
        if (frame == MOVE_BRAKE) {
            state->brakeFrom = speed;
        }
        input->move = (Vector2){-1.0f, 0.0f};
        input->boostHeld = false;
        if (speed > 40.0f) {
            ++state->brakeFrames;
        }
        return;
    }
    if (frame < MOVE_STOP) {
        input->move = (Vector2){-1.0f, 0.0f};
        input->boostHeld = false;
        state->reversed = state->reversed || game->player.velocity.x < -20.0f;
        state->reverseSpeed = game->player.velocity.x;
        return;
    }
    /* Hands off: momentum has to bleed away on its own rather than vanish. */
    input->move = (Vector2){0.0f, 0.0f};
    input->boostHeld = false;
    state->finalSpeed = speed;
    state->stopped = state->stopped || speed < 20.0f;
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
static void PresentGameAudio(const GameEventBuffer *events, GameAudio *audio)
{
    uint16_t index;

    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];

        switch (event->type) {
        case GAME_EVENT_BOOST_STAGE: {
            int stage = event->count;
            GameAudioPlayBoost(audio, stage);
            break;
        }
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
    bool debugHud = true;
    bool smokeTest = false;
    int argument;
    int smokeFrames = 0;
    Vector2 cameraFocus;
    Vector2 smokeAim = {0.0f, 0.0f};
    bool smokeReactionObserved = false;
    bool smokeLaserHitObserved = false;
    bool smokeExplosionObserved = false;
    bool smokeForceObserved = false;
    bool smokeCryoObserved = false;
    bool smokeBoostObserved = false;
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
    bool smokeEnvironmentObserved = false;
    bool smokeEnvironmentViewValid = true;
    bool smokeEnvironmentCameraFeedback = false;
    bool smokeEnvironmentZoomOut = false;
    uint8_t smokeEnvironmentPaletteMask = 0u;
    uint16_t smokeEnvironmentMaximumSceneDrawCalls = 0u;
    uint16_t smokeEnvironmentMaximumEmissiveDrawCalls = 0u;
    TerrainBodyHandle smokeTerrainBody = TerrainBodyInvalidHandle();
    Vector2 smokeTerrainStartPosition = {0.0f, 0.0f};
    float smokeTerrainStartAngle = 0.0f;
    bool smokeTerrainExtracted = false;
    bool smokeTerrainWorldCleared = false;
    bool smokeTerrainRendered = false;
    bool smokeTerrainMoved = false;
    bool smokeTerrainRotated = false;
    bool smokeTerrainCollisionObserved = false;
    bool smokeTerrainCacheReleased = false;
    /* The automatic-detach acceptance run: a blast severs a pillar and the
       fixed step is expected to turn the block above it into a body without
       anything in main.c reaching for the extraction API. */
    Vector2 smokeDetachBlast = {0.0f, 0.0f};
    bool smokeAutoDetachObserved = false;
    bool smokeAutoDetachEvent = false;
    /* The impulse half of the acceptance run: the blast has to throw what it
       freed, the light block has to outrun the heavy one, and a force blow has
       to move a body that is already lying there. */
    float smokeLightSpeed = 0.0f;
    float smokeHeavySpeed = 0.0f;
    float smokeThrownSpin = 0.0f;
    float smokeForceSpeedBefore = -1.0f;
    float smokeForceSpeedAfter = -1.0f;
    float smokeForceStartX = 0.0f;
    float smokeForceShiftX = 0.0f;
    bool smokeMassMattered = false;
    bool smokeForceMovedBody = false;
    /* The gameplay acceptance run's progress. Zeroed at setup rather than here
       so the whole struct is cleared in one place. */
    SmokeMovement movement = {{0.0f, 0.0f}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0, 0, PLAYER_BOOST_NONE, false, false,
                              false};
    SmokeAcceptance acceptance = {{0.0f, 0.0f}, {0.0f, 0.0f}, false, false,
                                  false, false, false, false, false, false,
                                  false, 0.0f, 0.0f, 0.0f, 0, {0.0f, 0.0f}};
    uint32_t smokeTerrainTextureUpdates = 0u;
    /* Bodies the render phase alone detached, so the upload count can be
       checked against what that phase produced rather than against everything
       the gameplay phase goes on to break apart. */
    int smokeRenderDetaches = 0;
    uint32_t smokeTerrainMaximumDrawCalls = 0u;
    uint64_t smokeTerrainMaximumTextureBytes = 0u;
    double smokeBloomSubmissionTotal = 0.0;
    double smokeBloomSubmissionMaximum = 0.0;
    /* What the world costs to keep on screen, measured over the flight rather
       than read off one frame of the HUD. A single frame's number varies by
       several times between runs on a loaded machine, which is enough to tune
       a renderer in the wrong direction. */
    Vector2 smokeGroundPosition = {0.0f, 0.0f};
    Vector2 smokeGroundVelocity = {0.0f, 0.0f};
    double smokePrepareTotal = 0.0;
    double smokePrepareMaximum = 0.0;
    int smokePrepareFrames = 0;
    int smokeBloomFrames = 0;
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
        !RendererInit(&renderer, &game, environmentPalette)) {
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
        smokeTerrainBody = SetupSmokeTerrainBody(
            &game, smokeAim, &smokeTerrainWorldCleared);
        smokeDetachBlast = SetupSmokeDetachScene(&game.world, smokeAim);
        memset(&acceptance, 0, sizeof(acceptance));
        /* Far enough from every other smoke fixture that the two phases never
           share a cell. */
        acceptance.platformAt = (Vector2){smokeAim.x + 150.0f, smokeAim.y - 6.0f};
        /* Open sky, well above the terrain. Underground the corridor has to be
           carved, and a carved corridor immediately fills with the sand its own
           ceiling drops into it — the player then spends the run bouncing off
           falling grains instead of building a boost. Up here nothing can fall
           in, and the turn has the whole sky to use. */
        movement.origin = (Vector2){smokeAim.x + 400.0f, 140.0f};
        {
            const TerrainBody *body = DynamicTerrainGetConst(
                &game.dynamicTerrain, smokeTerrainBody);

            if (body != NULL) {
                smokeTerrainExtracted = true;
                smokeTerrainStartPosition = body->position;
                smokeTerrainStartAngle = body->angle;
            }
        }
    }
    cameraFocus = game.player.position;
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

        /* Exercise the render-target resize lifecycle in the automated GL
           smoke run, then restore the reference screenshot dimensions. */
        if (smokeTest && smokeFrames == 2) {
            SetWindowSize(WINDOW_WIDTH - 320, WINDOW_HEIGHT - 180);
        } else if (smokeTest && smokeFrames == 7) {
            SetWindowSize(WINDOW_WIDTH, WINDOW_HEIGHT);
        }
        /* Present the palette reference frames at the widened high-speed view.
           They belong to the compact renderer phase before the long movement
           run reaches boost III; this presentation-only injection exercises
           the same camera scale without changing GameState. */
        if (smokeTest && smokeFrames == 9) {
            cameraFeedback.viewScale = 1.65f;
        }
        /* Input and the reticle share this exact stable transform. Camera
           impulses are applied later to a copy and therefore cannot leak back
           through GetScreenToWorld2D on the next frame. */
        stableCamera.offset =
            (Vector2){(float)GetScreenWidth() * 0.5f,
                      (float)GetScreenHeight() * 0.5f};
        stableCamera.rotation = 0.0f;
        stableCamera.zoom = CameraZoomForWindow(cameraFeedback.viewScale);
        stableCamera.target = ClampCameraTarget(
            stableCamera.target, stableCamera.zoom, &game.world);
        aimCamera = stableCamera;
        input = InputPoll(&game.world, aimCamera);
        cursorCell = input.cursorCell;
        aimPosition = input.game.aimWorld;
        /* All three palette screenshots contain the moving body. Free it as
           the next acceptance phase starts, so the same GL run also proves
           that the generation-keyed cache releases textures without a ghost. */
        if (smokeTest && smokeFrames == ACCEPT_START) {
            int slot;

            /* Every body, not only the showcase one: the check below is that
               the generation-keyed cache releases what it holds, and it only
               means that if nothing is left holding a slot. */
            DynamicTerrainFreeBody(&game.dynamicTerrain, smokeTerrainBody);
            for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
                if (game.dynamicTerrain.bodies[slot].active) {
                    DynamicTerrainFreeBody(&game.dynamicTerrain,
                                           (TerrainBodyHandle){
                                               (uint16_t)slot,
                                               game.dynamicTerrain.bodies[slot]
                                                   .generation});
                }
            }
        }
        /* Early enough that the freed block has several fixed steps to fall and
           land before the reference screenshot at frame ten. The scene is laid
           out again immediately before the blast: the surrounding generated
           world is a live simulation, and loose material that has poured into
           the gaps since setup would genuinely reconnect the block — a correct
           answer from the detector, and a useless showcase. Nothing here
           touches the terrain system: this is a blast, and the rest is the
           production path. */
        if (smokeTest && smokeFrames == 2) {
            smokeDetachBlast = SetupSmokeDetachScene(&game.world, smokeAim);
            WorldDestroyCircle(&game.world, (int)smokeDetachBlast.x,
                               (int)smokeDetachBlast.y, 12, 0.0f);
            /* Exactly what AbilityApplyExplosion queues. The blast is described
               here and delivered by the fixed step after detachment, so what it
               throws includes the two blocks it is about to set free. */
            (void)TerrainImpulseQueueBlast(&game.impulses, (TerrainBlast){
                .shape = TERRAIN_BLAST_RADIAL,
                .origin = smokeDetachBlast,
                .radius = ABILITY_EXPLOSION_SHOCK_RADIUS,
                .momentum = ABILITY_EXPLOSION_BODY_IMPULSE,
            });
        }
        /* A body that has been lying still for a few frames, shoved by the same
           cone the force power uses. */
        if (smokeTest && smokeFrames == 8) {
            const TerrainBody *resting = SmokeHeaviestBody(&game.dynamicTerrain);

            if (resting != NULL) {
                smokeForceSpeedBefore = Vector2Length(resting->velocity);
                smokeForceStartX = resting->position.x;
                (void)TerrainImpulseQueueBlast(&game.impulses, (TerrainBlast){
                    .shape = TERRAIN_BLAST_CONE,
                    .origin = (Vector2){resting->position.x - 24.0f,
                                        resting->position.y - 4.0f},
                    .direction = {1.0f, 0.0f},
                    .radius = ABILITY_FORCE_LENGTH,
                    .spreadCosine = ABILITY_FORCE_SPREAD_COSINE,
                    .momentum = ABILITY_FORCE_BODY_IMPULSE,
                });
            }
        }

        if (input.toggleDebugPressed) {
            debugHud = !debugHud;
        }

        if (smokeTest) {
            if (smokeFrames == 9) {
                (void)RendererSetEnvironmentPalette(
                    &renderer, ENVIRONMENT_PALETTE_EMBER_WASTE);
            } else if (smokeFrames == 10) {
                (void)RendererSetEnvironmentPalette(
                    &renderer, ENVIRONMENT_PALETTE_ABYSSAL_BLUE);
            } else if (smokeFrames == 11) {
                (void)RendererSetEnvironmentPalette(
                    &renderer, ENVIRONMENT_PALETTE_VERDIGRIS_STORM);
            } else if (smokeFrames == 12) {
                (void)RendererSetEnvironmentPalette(
                    &renderer, ENVIRONMENT_PALETTE_AMBER_DUNES);
            } else if (smokeFrames == 13) {
                (void)RendererSetEnvironmentPalette(
                    &renderer, ENVIRONMENT_PALETTE_GLACIER_SHELF);
            } else if (smokeFrames == 14) {
                /* Back to AUTO, so the rest of the run is driven by the ground
                   the player is over, which is what a session actually does. */
                (void)RendererSetEnvironmentPalette(&renderer,
                                                    ENVIRONMENT_PALETTE_AUTO);
            }
            /* The backdrop frames are photographs of a backdrop, and a
               backdrop at dawn is a dark shape against a dark sky. Noon while
               they are taken; the day resumes from where it was afterwards. */
            if (smokeFrames >= 8 && smokeFrames <= 14) {
                game.dayPhase = 0.25f;
            }
            /* Above the clouds, where there is nothing to hold anything down.
               Asked for the same way midnight is: the run is ten seconds long
               and cannot fly there on its own. Two frames so the light and the
               page cache catch up before the photograph. */
            if (smokeFrames == 18) {
                smokeGroundPosition = game.player.position;
                smokeGroundVelocity = game.player.velocity;
            }
            if (smokeFrames >= 18 && smokeFrames <= 26) {
                game.player.position.y =
                    (float)game.world.height * WORLD_SPACE_LINE * 0.9f;
                game.player.velocity = (Vector2){40.0f, 0.0f};
            }
            if (smokeFrames == 27) {
                /* Put back exactly where it was: the acceptance run that starts
                   here is about a slab on a platform, and it must not find its
                   player three hundred cells up. */
                game.player.position = smokeGroundPosition;
                game.player.velocity = smokeGroundVelocity;
            }
            /* A day lasts seven minutes and the run lasts ten seconds, so
               midnight has to be asked for. Held over two frames — one for the
               light to be re-solved with it, one to photograph — and then given
               back, so nothing after this sees a night that never happened. */
            if (smokeFrames == 15 || smokeFrames == 16) {
                game.dayPhase = 0.75f;
            } else if (smokeFrames == 17) {
                game.dayPhase = 0.25f;
            }
        }

        if (smokeTest && smokeFrames < ACCEPT_START) {
            aimPosition = (Vector2){smokeAim.x + 0.5f, smokeAim.y + 0.5f};
            cursorCell = smokeAim;
            input.game.aimWorld = aimPosition;
            input.game.move = smokeFrames == 0 ? (Vector2){1.0f, 0.0f}
                                                : (Vector2){0.0f, 0.0f};
            input.game.boostHeld = smokeFrames == 0;
            memset(input.game.ability, 0, sizeof(input.game.ability));
            input.game.ability[ABILITY_LASER] = smokeFrames >= 1 && smokeFrames <= 7;
            input.game.ability[ABILITY_EXPLOSION] = smokeFrames == 5;
            input.game.ability[ABILITY_FORCE] = smokeFrames == 8;
            input.game.ability[ABILITY_CRYO] = smokeFrames >= 9;
            input.game.regeneratePressed = false;
        } else if (smokeTest && smokeFrames < MOVE_START) {
            RunSmokeAcceptance(&game, &acceptance, smokeFrames, &input.game);
            aimPosition = input.game.aimWorld;
            cursorCell = aimPosition;
        } else if (smokeTest) {
            RunSmokeMovement(&game, &movement, smokeFrames, &input.game);
            aimPosition = input.game.aimWorld;
            cursorCell = aimPosition;
        }

        GameUpdate(&game, &input.game, deltaTime, &events);
        if (input.game.regeneratePressed) {
            cameraFocus = game.player.position;
            CameraFeedbackClear(&cameraFeedback);
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
        PresentGameAudio(&events, &audio);
        CameraFeedbackConsumeEvents(&cameraFeedback, &events,
                                    game.player.position);
        RendererUpdatePresentation(&renderer, &events, deltaTime);
        if (smokeTest) {
            smokeAutoDetachEvent = smokeAutoDetachEvent ||
                                   EventsContain(&events,
                                                 GAME_EVENT_TERRAIN_DETACHED);
            smokeAutoDetachObserved = game.detach.stats.autoDetachSucceeded > 0;
            /* The frame the blast lands: the two freed blocks are moving and
               nothing has slowed them yet. */
            if (smokeFrames == 3) {
                int slot;

                for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
                    const TerrainBody *thrown = &game.dynamicTerrain.bodies[slot];
                    float speed;

                    if (!thrown->active || thrown->cellCount < 20) {
                        continue;
                    }
                    speed = Vector2Length(thrown->velocity);
                    if (thrown->cellCount < 100) {
                        smokeLightSpeed = speed;
                    } else {
                        smokeHeavySpeed = speed;
                    }
                    if (fabsf(thrown->angularVelocity) > fabsf(smokeThrownSpin)) {
                        smokeThrownSpin = thrown->angularVelocity;
                    }
                }
                smokeMassMattered = smokeLightSpeed > smokeHeavySpeed * 1.5f &&
                                    smokeHeavySpeed > 0.0f;
            }
            /* Sampled on the frame the blow lands, not the one after: a body
               lying on the ground is being rubbed by friction every tick, and a
               shove read a frame late is a shove already half spent. */
            if (smokeFrames >= 8 && smokeForceSpeedBefore >= 0.0f) {
                const TerrainBody *shoved = SmokeHeaviestBody(&game.dynamicTerrain);

                if (shoved != NULL) {
                    if (smokeFrames == 8) {
                        smokeForceSpeedAfter = Vector2Length(shoved->velocity);
                    }
                    smokeForceShiftX = shoved->position.x - smokeForceStartX;
                    smokeForceMovedBody = smokeForceMovedBody ||
                                          smokeForceShiftX > 1.0f;
                }
            }
            smokeReactionObserved = smokeReactionObserved ||
                                    EventsContain(&events,
                                                  GAME_EVENT_MATERIAL_REACTION);
            smokeLaserHitObserved = smokeLaserHitObserved ||
                                    EventsContain(&events, GAME_EVENT_LASER_HIT);
            smokeExplosionObserved = smokeExplosionObserved ||
                                     EventsContain(&events,
                                                   GAME_EVENT_EXPLOSION);
            smokeForceObserved = smokeForceObserved ||
                                 EventsContain(&events, GAME_EVENT_FORCE);
            smokeCryoObserved = smokeCryoObserved ||
                                EventsContain(&events, GAME_EVENT_CRYO_HIT);
            smokeBoostObserved = smokeBoostObserved ||
                                 EventsContain(&events,
                                               GAME_EVENT_BOOST_STAGE);
        }

        cameraOutput = CameraFeedbackUpdate(
            &cameraFeedback,
            (CameraFeedbackMotion){
                .velocity = game.player.velocity,
                .normalSpeed = game.player.maxSpeed,
                .maximumSpeed = game.player.boostMaxSpeed,
                .viewWidth = VIEW_WIDTH,
                .viewHeight = VIEW_HEIGHT,
            },
            deltaTime);
        if (smokeTest && smokeFrames >= ACCEPT_CUT) {
            /* The camera answering to what the gameplay phase is doing, on the
               frames where a body is being blown apart. */
            acceptance.cameraFeedbackDuringPlay =
                acceptance.cameraFeedbackDuringPlay ||
                fabsf(cameraOutput.impulseOffset.x) > 0.01f ||
                fabsf(cameraOutput.impulseOffset.y) > 0.01f ||
                fabsf(cameraOutput.rotationDegrees) > 0.01f ||
                fabsf(cameraOutput.zoomKick) > 0.0001f;
        }
        /* Widening belongs to the stable camera used on the next input frame.
           The immediate kick is applied only to the presentation copy below,
           so transient feedback never changes mouse-to-world conversion. */
        {
            Vector2 lead = {
                game.player.position.x + cameraOutput.lookahead.x,
                game.player.position.y + cameraOutput.lookahead.y};

            desiredCamera = ClampCameraTarget(
                lead, CameraZoomForWindow(cameraOutput.viewScale), &game.world);
        }
        cameraFocus.x += (desiredCamera.x - cameraFocus.x) *
                         (1.0f - expf(-8.0f * deltaTime));
        cameraFocus.y += (desiredCamera.y - cameraFocus.y) *
                         (1.0f - expf(-8.0f * deltaTime));
        stableCamera.zoom = CameraZoomForWindow(cameraOutput.viewScale);
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
                           52.0f + (float)game.player.boostStage * 22.0f,
                           0.72f + (float)game.player.boostStage * 0.09f);

        RendererRenderScene(&renderer, &game, presentationCamera, aimCamera,
                            aimPosition,
                            VisibleWorldRectangle(presentationCamera));
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
            if (smokeFrames >= ACCEPT_CUT) {
                /* Staged FX still spawning while the gameplay phase blows a
                   body apart: the two features are running at once, not merely
                   both present. */
                acceptance.fxDuringPlay = acceptance.fxDuringPlay ||
                                          frameStats->activeFx > 0u;
            }
            smokePresentationFxObserved = smokePresentationFxObserved ||
                                          (frameStats->activeFx > 0u &&
                                           frameStats->peakFx >= 22u &&
                                           frameStats->droppedFx == 0u);
            smokeEnvironmentObserved = smokeEnvironmentObserved ||
                                       (frameStats->environmentSceneDrawCalls > 0u &&
                                        frameStats->environmentEmissiveDrawCalls > 0u &&
                                        frameStats->environmentEmissiveContributors > 0u);
            smokeEnvironmentViewValid = smokeEnvironmentViewValid &&
                                        frameStats->environmentViewValid;
            if (frameStats->environmentPalette >= 0 &&
                frameStats->environmentPalette < ENVIRONMENT_PALETTE_COUNT) {
                smokeEnvironmentPaletteMask |=
                    (uint8_t)(1u <<
                              (unsigned int)frameStats->environmentPalette);
            }
            if (frameStats->environmentSceneDrawCalls >
                smokeEnvironmentMaximumSceneDrawCalls) {
                smokeEnvironmentMaximumSceneDrawCalls =
                    frameStats->environmentSceneDrawCalls;
            }
            if (frameStats->environmentEmissiveDrawCalls >
                smokeEnvironmentMaximumEmissiveDrawCalls) {
                smokeEnvironmentMaximumEmissiveDrawCalls =
                    frameStats->environmentEmissiveDrawCalls;
            }
            if ((fabsf(presentationCamera.rotation) > 0.001f ||
                 fabsf(presentationCamera.zoom - aimCamera.zoom) > 0.001f) &&
                frameStats->environmentViewValid) {
                smokeEnvironmentCameraFeedback = true;
            }
            smokeEnvironmentZoomOut = smokeEnvironmentZoomOut ||
                                      (smokeFrames >= 9 &&
                                       cameraOutput.viewScale > 1.35f &&
                                       frameStats->environmentViewValid);
            if (smokeFrames < ACCEPT_START) {
                /* Counted for the render phase alone. The gameplay phase carves
                   and splits bodies on purpose, and every one of those is a
                   legitimate new upload. */
                smokeTerrainTextureUpdates +=
                    frameStats->terrainBodyTextureUpdates;
                smokeRenderDetaches = game.detach.stats.autoDetachSucceeded;
            }
            if (frameStats->terrainBodyDrawCalls >
                smokeTerrainMaximumDrawCalls) {
                smokeTerrainMaximumDrawCalls =
                    frameStats->terrainBodyDrawCalls;
            }
            if (frameStats->terrainBodyTextureMemoryBytes >
                smokeTerrainMaximumTextureBytes) {
                smokeTerrainMaximumTextureBytes =
                    frameStats->terrainBodyTextureMemoryBytes;
            }
            smokeTerrainRendered = smokeTerrainRendered ||
                                   (frameStats->visibleTerrainBodies == 1u &&
                                    frameStats->cachedTerrainBodies == 1u &&
                                    frameStats->terrainBodyDrawCalls >= 2u);
            smokeTerrainCollisionObserved = smokeTerrainCollisionObserved ||
                game.dynamicTerrain.stats.collisionContacts > 0;
            {
                const TerrainBody *body = DynamicTerrainGetConst(
                    &game.dynamicTerrain, smokeTerrainBody);

                if (body != NULL) {
                    smokeTerrainMoved = smokeTerrainMoved ||
                        Vector2Distance(body->position,
                                        smokeTerrainStartPosition) > 0.05f;
                    smokeTerrainRotated = smokeTerrainRotated ||
                        fabsf(body->angle - smokeTerrainStartAngle) > 0.01f;
                }
            }
            if (smokeFrames == ACCEPT_START) {
                smokeTerrainCacheReleased =
                    frameStats->cachedTerrainBodies == 0u &&
                    frameStats->visibleTerrainBodies == 0u &&
                    frameStats->terrainBodyTextureMemoryBytes == 0u;
            }
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
            {
                const WorldRendererStats *worldStats = RendererWorldStats(&renderer);

                if (smokeFrames >= 8 && worldStats != NULL) {
                    smokePrepareTotal += worldStats->preparationMilliseconds;
                    smokePrepareMaximum = fmax(smokePrepareMaximum,
                                               worldStats->preparationMilliseconds);
                    ++smokePrepareFrames;
                }
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
        /* The two beam frames are photographs of a beam, and the debug panel
           sits exactly where the character does at the start of the run. */
        if (debugHud &&
            !(smokeTest && (smokeFrames == 3 || smokeFrames == 10))) {
            DrawDebugHud(&game, &events, &renderer, cursorCell);
        }
        DrawControlsHint();
        EndDrawing();

        if (smokeTest) {
            /* Mid-beam, so the frame shows where a beam actually leaves the
               character. It left the chest for a long time and nobody could
               tell from the reference screenshot, which is taken after the
               beam has stopped. */
            /* Frame three: the beam is held and the explosion has not fired
               yet, so the picture is the beam and nothing else. Frame ten is
               the cryo beam for the same reason. */
            if (smokeFrames == 3) {
                TakeScreenshot("build/emberfall-beam.png");
            }
            if (smokeFrames == 10) {
                TakeScreenshot("build/emberfall-cryo.png");
            }
            if (smokeFrames == 9) {
                TakeScreenshot("build/emberfall-smoke-ember.png");
            } else if (smokeFrames == 10) {
                TakeScreenshot("build/emberfall-smoke-abyss.png");
            } else if (smokeFrames == 11) {
                TakeScreenshot("build/emberfall-smoke-storm.png");
            } else if (smokeFrames == 12) {
                TakeScreenshot("build/emberfall-smoke-dunes.png");
            } else if (smokeFrames == 13) {
                TakeScreenshot("build/emberfall-smoke-glacier.png");
            }
            if (smokeFrames == 16) {
                TakeScreenshot("build/emberfall-night.png");
            }
            if (smokeFrames == 26) {
                TakeScreenshot("build/emberfall-space.png");
            }
            if (smokeFrames == 10) {
                TakeScreenshot("build/emberfall-smoke.png");
            }
            /* Mid-hold, so the frame shows the telekinetic beam actually
               reaching a slab rather than the aftermath of having moved it. */
            if (smokeFrames == ACCEPT_GRAB + 30) {
                TakeScreenshot("build/emberfall-grab.png");
            }
            if (smokeFrames == ACCEPT_SHOT) {
                TakeScreenshot("build/emberfall-gameplay.png");
            }
            if (smokeFrames == MOVE_SHOT_FRAME) {
                TakeScreenshot("build/emberfall-movement.png");
            }
            if (++smokeFrames >= MOVE_END) {
                break;
            }
        }
    }

    if (smokeTest && smokeBloomFrames > 0) {
        const RendererFrameStats *frameStats = RendererStats(&renderer);

        printf("Smoke render: bloom=%dx%d passes=%u targets=%u "
               "submit_avg=%.3fms submit_max=%.3fms "
               "prepare_avg=%.3fms prepare_max=%.3fms "
               "resize=%d restored=%d bloom_resize=%d bloom_restored=%d "
               "target_sync=%d fx_peak=%u fx_dropped=%u "
               "body_draws=%u body_updates=%u body_kib=%.1f "
               "body_collision=%d body_released=%d "
               "auto_detach=%d auto_detach_event=%d detach_checks=%d "
               "detach_cells=%d detach_rejects=a%d/u%d/s%d/l%d/b%d "
               "light=%.1f heavy=%.1f spin=%.3f mass_matters=%d "
               "force_before=%.1f force_after=%.1f force_shift=%.2f "
               "force_hits=%d force_moved=%d impulses=%d "
               "env_mask=0x%x env_draws=%u+%u env_camera=%d env_zoom=%d "
               "play_detached=%d play_pushed=%d(%.1f) play_grabbed=%d "
               "play_dragged=%d(%.1f) play_threw=%d(%.1f) play_carved=%d "
               "play_split=%d play_fragments=%d play_fx=%d play_camera=%d "
               "move_cruise=%.1f move_s1=%.1f move_s2=%.1f move_peak=%.1f "
               "move_stage=%d move_turn=%.1f move_drilled=%d "
               "move_brake=%.1f->%.1f in %d frames move_reverse=%.1f "
               "move_final=%.1f\n",
               frameStats->bloomWidth, frameStats->bloomHeight,
               frameStats->offscreenPasses, frameStats->renderTargets,
               smokeBloomSubmissionTotal / (double)smokeBloomFrames,
               smokeBloomSubmissionMaximum,
               smokePrepareFrames > 0
                   ? smokePrepareTotal / (double)smokePrepareFrames
                   : 0.0,
               smokePrepareMaximum, smokeResizeObserved,
               smokeResizeRestored, smokeBloomResized, smokeBloomRestored,
               smokeTargetsSynchronized, (unsigned int)frameStats->peakFx,
               (unsigned int)frameStats->droppedFx,
               smokeTerrainMaximumDrawCalls, smokeTerrainTextureUpdates,
               (double)smokeTerrainMaximumTextureBytes / 1024.0,
               smokeTerrainCollisionObserved,
               smokeTerrainCacheReleased,
               game.detach.stats.autoDetachSucceeded,
               smokeAutoDetachEvent, game.detach.stats.detachChecks,
               game.detach.stats.autoDetachCells,
               game.detach.stats.autoDetachRejectedAnchored,
               game.detach.stats.autoDetachRejectedUnknown,
               game.detach.stats.autoDetachRejectedTooSmall,
               game.detach.stats.autoDetachRejectedTooLarge,
               game.detach.stats.autoDetachRejectedBudget,
               (double)smokeLightSpeed, (double)smokeHeavySpeed,
               (double)smokeThrownSpin, smokeMassMattered,
               (double)smokeForceSpeedBefore, (double)smokeForceSpeedAfter,
               (double)smokeForceShiftX,
               game.impulses.stats.bodiesAffectedByForce, smokeForceMovedBody,
               game.impulses.stats.bodyImpulseApplications,
               (unsigned int)smokeEnvironmentPaletteMask,
               (unsigned int)smokeEnvironmentMaximumSceneDrawCalls,
               (unsigned int)smokeEnvironmentMaximumEmissiveDrawCalls,
               smokeEnvironmentCameraFeedback, smokeEnvironmentZoomOut,
               acceptance.detached, acceptance.pushed, (double)acceptance.pushSpeed,
               acceptance.grabbed, acceptance.dragged,
               (double)acceptance.dragDistance, acceptance.threw,
               (double)acceptance.throwSpeed, acceptance.carved, acceptance.split,
               acceptance.fragments, acceptance.fxDuringPlay,
               acceptance.cameraFeedbackDuringPlay,
               (double)movement.cruiseSpeed, (double)movement.stageOneSpeed,
               (double)movement.stageTwoSpeed, (double)movement.peakSpeed,
               (int)movement.topStage, (double)movement.turnLateral,
               movement.drilled, (double)movement.brakeFrom,
               (double)movement.finalSpeed, movement.brakeFrames,
               (double)movement.reverseSpeed, (double)movement.finalSpeed);
    }

    if (smokeTest && (!smokeReactionObserved || !smokeLaserHitObserved ||
                      !smokeExplosionObserved || !smokeForceObserved ||
                      !smokeCryoObserved || !smokeBoostObserved ||
                      !smokeCollisionObserved ||
                      !smokeDrillObserved || !smokeFireContained ||
                      !smokeBloomObserved || !smokeTargetsSynchronized ||
                      !smokePresentationFxObserved ||
                      !smokeEnvironmentObserved ||
                      !smokeEnvironmentViewValid ||
                      !smokeEnvironmentCameraFeedback ||
                      !smokeEnvironmentZoomOut ||
                      smokeEnvironmentPaletteMask !=
                          (uint8_t)((1u << ENVIRONMENT_PALETTE_COUNT) - 1u) ||
                      !smokeTerrainExtracted || !smokeTerrainWorldCleared ||
                      !smokeTerrainRendered || !smokeTerrainMoved ||
                      !smokeTerrainRotated ||
                      !smokeTerrainCollisionObserved ||
                      !smokeTerrainCacheReleased ||
                      !smokeAutoDetachObserved || !smokeAutoDetachEvent ||
                      !smokeMassMattered || !smokeForceMovedBody ||
                      !acceptance.detached || !acceptance.pushed ||
                      !acceptance.grabbed || !acceptance.dragged ||
                      !acceptance.threw || !acceptance.carved ||
                      !acceptance.split || acceptance.fragments < 2 ||
                      !acceptance.fxDuringPlay ||
                      !acceptance.cameraFeedbackDuringPlay ||
                      movement.topStage != PLAYER_BOOST_STAGE_THREE ||
                      movement.cruiseSpeed < 40.0f ||
                      movement.peakSpeed < movement.stageTwoSpeed ||
                      !movement.turned || movement.drilled <= 0 ||
                      !movement.reversed || !movement.stopped ||
                      fabsf(smokeThrownSpin) <= 0.0f ||
                      /* Two uploads — scene and emissive — for each body that
                         ever existed, and not one more: a body whose raster
                         never changes must not be re-uploaded per frame. */
                      smokeTerrainTextureUpdates !=
                          2u * (1u + (unsigned int)smokeRenderDetaches) ||
                      game.world.activeChunkCount <= 0 ||
                      game.world.activeChunkCount >=
                          game.world.chunkColumns * game.world.chunkRows)) {
        fprintf(stderr,
                "Smoke test failed: reaction=%d laser=%d explosion=%d force=%d "
                "cryo=%d boost=%d collision=%d "
                "drill=%d fire_contained=%d resize=%d restored=%d "
                "bloom=%d bloom_resize=%d bloom_restored=%d target_sync=%d "
                "presentation_fx=%d body=%d cleared=%d rendered=%d moved=%d "
                "rotated=%d collision=%d released=%d auto_detach=%d "
                "auto_detach_event=%d mass_matters=%d force_moved=%d spin=%.3f "
                "env=%d/valid%d/camera%d/zoom%d/palettes0x%x "
                "play=d%d/p%d/g%d/D%d/t%d/c%d/s%d/f%d/fx%d/cam%d "
                "move=cruise%.0f/peak%.0f/stage%d/turn%.0f/drill%d/rev%d/stop%d "
                "updates=%u chunks=%d/%d\n",
                smokeReactionObserved, smokeLaserHitObserved,
                smokeExplosionObserved, smokeForceObserved,
                smokeCryoObserved, smokeBoostObserved,
                smokeCollisionObserved, smokeDrillObserved, smokeFireContained,
                smokeResizeObserved, smokeResizeRestored,
                smokeBloomObserved, smokeBloomResized, smokeBloomRestored,
                smokeTargetsSynchronized,
                smokePresentationFxObserved,
                smokeTerrainExtracted, smokeTerrainWorldCleared,
                smokeTerrainRendered, smokeTerrainMoved,
                smokeTerrainRotated, smokeTerrainCollisionObserved,
                smokeTerrainCacheReleased, smokeAutoDetachObserved,
                smokeAutoDetachEvent, smokeMassMattered, smokeForceMovedBody,
                (double)smokeThrownSpin,
                smokeEnvironmentObserved, smokeEnvironmentViewValid,
                smokeEnvironmentCameraFeedback, smokeEnvironmentZoomOut,
                (unsigned int)smokeEnvironmentPaletteMask,
                acceptance.detached, acceptance.pushed,
                acceptance.grabbed, acceptance.dragged, acceptance.threw,
                acceptance.carved, acceptance.split, acceptance.fragments,
                acceptance.fxDuringPlay, acceptance.cameraFeedbackDuringPlay,
                (double)movement.cruiseSpeed, (double)movement.peakSpeed,
                (int)movement.topStage, (double)movement.turnLateral,
                movement.drilled, movement.reversed, movement.stopped,
                smokeTerrainTextureUpdates,
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
