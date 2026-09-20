/* The scripted GL session behind --smoke-test. See smoke_test.h. */
#include "smoke_test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <raymath.h>
#include <rlgl.h>

#include "world_components.h"

#define SMOKE_WINDOW_WIDTH 1280
#define SMOKE_WINDOW_HEIGHT 720

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
            ParticlesSpawnBoostTrail(particles, probe.position, probe.velocity);
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
    /* Static rather than automatic: the workspace is a hundred kilobytes, which
       is more than belongs on a stack frame even in a setup path that runs
       once. */
    static WorldComponentWorkspace workspace;
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
           is the production path.
         *
           The height is held level with the body every frame rather than only
           on the first. The slab is still falling when this phase opens, and a
           player placed level with it once walked straight over the top of it
           as it dropped eleven cells away — which is why the phase stopped
           registering a contact at all when the world grew taller. Only the
           starting distance is fixed; the approach itself is the production
           path. */
        if (frame == ACCEPT_PUSH) {
            game->player.position.x = body->position.x - 24.0f;
        }
        game->player.position.y = body->position.y;
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
        state->boostSpeed = speed;
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
            state->drillEntrySpeed = speed;
            state->drillLowSpeed = speed;
        } else if (speed < state->drillLowSpeed) {
            state->drillLowSpeed = speed;
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

void SmokeTestPrepare(SmokeTest *smoke, GameState *game)
{
    const TerrainBody *body;

    memset(smoke, 0, sizeof(*smoke));
    smoke->targetsSynchronized = true;
    smoke->environmentViewValid = true;
    smoke->forceSpeedBefore = -1.0f;
    smoke->forceSpeedAfter = -1.0f;
    smoke->terrainBody = TerrainBodyInvalidHandle();

    smoke->fireContained = RunSmokeFireContainmentProbe();
    RunSmokePlayerProbe(&game->world, &game->particles, &smoke->collisionObserved,
                        &smoke->drillObserved);
    /* The probe has exercised the spawn paths; drop what it emitted so the
       reference screenshot starts from a clean frame. */
    ParticlesInit(&game->particles, game->worldSeed);
    /* Build the laser/explosion target relative to the spawn instead of at
       fixed coordinates: generation is randomised, so a hardcoded point is
       not guaranteed to contain terrain. */
    smoke->aim = (Vector2){game->player.position.x + 14.0f,
                           game->player.position.y + 40.0f};
    SetupSmokeTarget(&game->world, smoke->aim);
    smoke->terrainBody = SetupSmokeTerrainBody(game, smoke->aim,
                                               &smoke->terrainWorldCleared);
    smoke->detachBlast = SetupSmokeDetachScene(&game->world, smoke->aim);
    /* Far enough from every other smoke fixture that the two phases never
       share a cell. */
    smoke->acceptance.platformAt =
        (Vector2){smoke->aim.x + 150.0f, smoke->aim.y - 6.0f};
    /* Open sky, well above the terrain. Underground the corridor has to be
       carved, and a carved corridor immediately fills with the sand its own
       ceiling drops into it — the player then spends the run bouncing off
       falling grains instead of building a boost. Up here nothing can fall
       in, and the turn has the whole sky to use. */
    smoke->movement.origin =
        (Vector2){smoke->aim.x + 400.0f, WorldCloudLineY(&game->world) - 300.0f};
    body = DynamicTerrainGetConst(&game->dynamicTerrain, smoke->terrainBody);
    if (body != NULL) {
        smoke->terrainExtracted = true;
        smoke->terrainStartPosition = body->position;
        smoke->terrainStartAngle = body->angle;
    }
}

void SmokeTestBeginFrame(SmokeTest *smoke, GameState *game, Renderer *renderer,
                         CameraFeedback *feedback)
{
    int frame = smoke->frame;

    /* Exercise the render-target resize lifecycle in the automated GL smoke
       run, then restore the reference screenshot dimensions. */
    if (frame == 2) {
        SetWindowSize(SMOKE_WINDOW_WIDTH - 320, SMOKE_WINDOW_HEIGHT - 180);
    } else if (frame == 7) {
        SetWindowSize(SMOKE_WINDOW_WIDTH, SMOKE_WINDOW_HEIGHT);
    }
    /* Present the palette reference frames at the widened high-speed view.
       They belong to the compact renderer phase before the long movement run
       reaches boost III; this presentation-only injection exercises the same
       camera scale without changing GameState. */
    if (frame == 9) {
        feedback->viewScale = 1.65f;
    }
    /* All three palette screenshots contain the moving body. Free it as the
       next acceptance phase starts, so the same GL run also proves that the
       generation-keyed cache releases textures without a ghost. Every body,
       not only the showcase one: the check is that the cache releases what it
       holds, and it only means that if nothing is left holding a slot. */
    if (frame == ACCEPT_START) {
        int slot;

        DynamicTerrainFreeBody(&game->dynamicTerrain, smoke->terrainBody);
        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            if (game->dynamicTerrain.bodies[slot].active) {
                DynamicTerrainFreeBody(
                    &game->dynamicTerrain,
                    (TerrainBodyHandle){(uint16_t)slot,
                                        game->dynamicTerrain.bodies[slot]
                                            .generation});
            }
        }
    }
    /* Early enough that the freed block has several fixed steps to fall and
       land before the reference screenshot at frame ten. The scene is laid
       out again immediately before the blast: the surrounding generated world
       is a live simulation, and loose material that has poured into the gaps
       since setup would genuinely reconnect the block — a correct answer from
       the detector, and a useless showcase. Nothing here touches the terrain
       system: this is a blast, and the rest is the production path. */
    if (frame == 2) {
        smoke->detachBlast = SetupSmokeDetachScene(&game->world, smoke->aim);
        WorldDestroyCircle(&game->world, (int)smoke->detachBlast.x,
                           (int)smoke->detachBlast.y, 12, 0.0f);
        /* Exactly what AbilityApplyExplosion queues. The blast is described
           here and delivered by the fixed step after detachment, so what it
           throws includes the two blocks it is about to set free. */
        (void)TerrainImpulseQueueBlast(&game->impulses, (TerrainBlast){
            .shape = TERRAIN_BLAST_RADIAL,
            .origin = smoke->detachBlast,
            .radius = ABILITY_EXPLOSION_SHOCK_RADIUS,
            .momentum = ABILITY_EXPLOSION_BODY_IMPULSE,
        });
    }
    /* A body that has been lying still for a few frames, shoved by the same
       cone the force power uses. */
    if (frame == 8) {
        const TerrainBody *resting = SmokeHeaviestBody(&game->dynamicTerrain);

        if (resting != NULL) {
            smoke->forceSpeedBefore = Vector2Length(resting->velocity);
            smoke->forceStartX = resting->position.x;
            (void)TerrainImpulseQueueBlast(&game->impulses, (TerrainBlast){
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

    if (frame == 9) {
        (void)RendererSetEnvironmentPalette(renderer,
                                            ENVIRONMENT_PALETTE_EMBER_WASTE);
    } else if (frame == 10) {
        (void)RendererSetEnvironmentPalette(renderer,
                                            ENVIRONMENT_PALETTE_ABYSSAL_BLUE);
    } else if (frame == 11) {
        (void)RendererSetEnvironmentPalette(renderer,
                                            ENVIRONMENT_PALETTE_VERDIGRIS_STORM);
    } else if (frame == 12) {
        (void)RendererSetEnvironmentPalette(renderer,
                                            ENVIRONMENT_PALETTE_AMBER_DUNES);
    } else if (frame == 13) {
        (void)RendererSetEnvironmentPalette(renderer,
                                            ENVIRONMENT_PALETTE_GLACIER_SHELF);
    } else if (frame == 14) {
        /* Back to AUTO, so the rest of the run is driven by the ground the
           player is over, which is what a session actually does. */
        (void)RendererSetEnvironmentPalette(renderer, ENVIRONMENT_PALETTE_AUTO);
    }
    /* The backdrop frames are photographs of a backdrop, and a backdrop at
       dawn is a dark shape against a dark sky. Noon while they are taken; the
       day resumes from where it was afterwards. */
    if (frame >= 8 && frame <= 14) {
        game->dayPhase = 0.25f;
    }
    /* Above the clouds, where there is nothing to hold anything down. Asked
       for the same way midnight is: the run is ten seconds long and cannot fly
       there on its own. Held for several frames so the light and the page
       cache catch up before the photograph. */
    if (frame == 18) {
        smoke->groundPosition = game->player.position;
        smoke->groundVelocity = game->player.velocity;
    }
    if (frame >= 18 && frame <= 26) {
        game->player.position.y =
            (float)game->world.height * WORLD_SPACE_LINE * 0.9f;
        game->player.velocity = (Vector2){40.0f, 0.0f};
    }
    if (frame == 27) {
        /* Put back exactly where it was: the acceptance run that starts here
           is about a slab on a platform, and it must not find its player three
           hundred cells up. */
        game->player.position = smoke->groundPosition;
        game->player.velocity = smoke->groundVelocity;
    }
    /* A day lasts seven minutes and the run lasts ten seconds, so midnight has
       to be asked for. Held over two frames — one for the light to be
       re-solved with it, one to photograph — and then given back, so nothing
       after this sees a night that never happened. */
    if (frame == 15 || frame == 16) {
        game->dayPhase = 0.75f;
    } else if (frame == 17) {
        game->dayPhase = 0.25f;
    }
}

void SmokeTestScriptInput(SmokeTest *smoke, GameState *game, AppInput *input,
                          Vector2 *aimPosition, Vector2 *cursorCell)
{
    int frame = smoke->frame;

    if (frame < ACCEPT_START) {
        *aimPosition = (Vector2){smoke->aim.x + 0.5f, smoke->aim.y + 0.5f};
        *cursorCell = smoke->aim;
        input->game.aimWorld = *aimPosition;
        input->game.move = frame == 0 ? (Vector2){1.0f, 0.0f}
                                      : (Vector2){0.0f, 0.0f};
        input->game.boostHeld = frame == 0;
        memset(input->game.ability, 0, sizeof(input->game.ability));
        input->game.ability[ABILITY_LASER] = frame >= 1 && frame <= 7;
        input->game.ability[ABILITY_EXPLOSION] = frame == 5;
        input->game.ability[ABILITY_FORCE] = frame == 8;
        input->game.ability[ABILITY_CRYO] = frame >= 9;
        input->game.regeneratePressed = false;
    } else if (frame < MOVE_START) {
        RunSmokeAcceptance(game, &smoke->acceptance, frame, &input->game);
        *aimPosition = input->game.aimWorld;
        *cursorCell = *aimPosition;
    } else {
        RunSmokeMovement(game, &smoke->movement, frame, &input->game);
        *aimPosition = input->game.aimWorld;
        *cursorCell = *aimPosition;
    }
}

void SmokeTestObserveUpdate(SmokeTest *smoke, const GameState *game,
                            const GameEventBuffer *events)
{
    int frame = smoke->frame;

    if (game->world.activeChunkCount > smoke->mostActiveChunks) {
        smoke->mostActiveChunks = game->world.activeChunkCount;
    }
    smoke->autoDetachEvent = smoke->autoDetachEvent ||
                             EventsContain(events, GAME_EVENT_TERRAIN_DETACHED);
    smoke->autoDetachObserved = game->detach.stats.autoDetachSucceeded > 0;
    /* The frame the blast lands: the two freed blocks are moving and nothing
       has slowed them yet. */
    if (frame == 3) {
        int slot;

        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            const TerrainBody *thrown = &game->dynamicTerrain.bodies[slot];
            float speed;

            if (!thrown->active || thrown->cellCount < 20) {
                continue;
            }
            speed = Vector2Length(thrown->velocity);
            if (thrown->cellCount < 100) {
                smoke->lightSpeed = speed;
            } else {
                smoke->heavySpeed = speed;
            }
            if (fabsf(thrown->angularVelocity) > fabsf(smoke->thrownSpin)) {
                smoke->thrownSpin = thrown->angularVelocity;
            }
        }
        smoke->massMattered = smoke->lightSpeed > smoke->heavySpeed * 1.5f &&
                              smoke->heavySpeed > 0.0f;
    }
    /* Sampled on the frame the blow lands, not the one after: a body lying on
       the ground is being rubbed by friction every tick, and a shove read a
       frame late is a shove already half spent. */
    if (frame >= 8 && smoke->forceSpeedBefore >= 0.0f) {
        const TerrainBody *shoved = SmokeHeaviestBody(&game->dynamicTerrain);

        if (shoved != NULL) {
            if (frame == 8) {
                smoke->forceSpeedAfter = Vector2Length(shoved->velocity);
            }
            smoke->forceShiftX = shoved->position.x - smoke->forceStartX;
            smoke->forceMovedBody = smoke->forceMovedBody ||
                                    smoke->forceShiftX > 1.0f;
        }
    }
    smoke->reactionObserved = smoke->reactionObserved ||
                              EventsContain(events, GAME_EVENT_MATERIAL_REACTION);
    smoke->laserHitObserved = smoke->laserHitObserved ||
                              EventsContain(events, GAME_EVENT_LASER_HIT);
    smoke->explosionObserved = smoke->explosionObserved ||
                               EventsContain(events, GAME_EVENT_EXPLOSION);
    smoke->forceObserved = smoke->forceObserved ||
                           EventsContain(events, GAME_EVENT_FORCE);
    smoke->cryoObserved = smoke->cryoObserved ||
                          EventsContain(events, GAME_EVENT_CRYO_HIT);
    smoke->boostObserved = smoke->boostObserved ||
                           EventsContain(events, GAME_EVENT_BOOST_ENGAGED);
}

void SmokeTestObserveCamera(SmokeTest *smoke, CameraFeedbackOutput output)
{
    if (smoke->frame >= ACCEPT_CUT) {
        /* The camera answering to what the gameplay phase is doing, on the
           frames where a body is being blown apart. */
        smoke->acceptance.cameraFeedbackDuringPlay =
            smoke->acceptance.cameraFeedbackDuringPlay ||
            fabsf(output.impulseOffset.x) > 0.01f ||
            fabsf(output.impulseOffset.y) > 0.01f ||
            fabsf(output.rotationDegrees) > 0.01f ||
            fabsf(output.zoomKick) > 0.0001f;
    }
}

void SmokeTestObserveRender(SmokeTest *smoke, const GameState *game,
                            const Renderer *renderer, Camera2D presentationCamera,
                            Camera2D aimCamera, CameraFeedbackOutput output)
{
    const RendererFrameStats *frameStats = RendererStats(renderer);
    const WorldRendererStats *worldStats = RendererWorldStats(renderer);
    int frame = smoke->frame;
    int screenWidth = GetScreenWidth();
    int screenHeight = GetScreenHeight();

    smoke->targetsSynchronized = smoke->targetsSynchronized &&
                                 frameStats->targetWidth == screenWidth &&
                                 frameStats->targetHeight == screenHeight;
    if (frameStats->bloomEnabled) {
        smoke->targetsSynchronized =
            smoke->targetsSynchronized &&
            frameStats->bloomWidth == (screenWidth + 1) / 2 &&
            frameStats->bloomHeight == (screenHeight + 1) / 2;
    }

    smoke->bloomObserved = smoke->bloomObserved ||
                           (frameStats->bloomEnabled &&
                            frameStats->offscreenPasses == 5u &&
                            frameStats->renderTargets == 4u);
    if (frame >= ACCEPT_CUT) {
        /* Staged FX still spawning while the gameplay phase blows a body
           apart: the two features are running at once, not merely both
           present. */
        smoke->acceptance.fxDuringPlay = smoke->acceptance.fxDuringPlay ||
                                         frameStats->activeFx > 0u;
    }
    smoke->presentationFxObserved = smoke->presentationFxObserved ||
                                    (frameStats->activeFx > 0u &&
                                     frameStats->peakFx >= 22u &&
                                     frameStats->droppedFx == 0u);
    smoke->environmentObserved =
        smoke->environmentObserved ||
        (frameStats->environmentSceneDrawCalls > 0u &&
         frameStats->environmentEmissiveDrawCalls > 0u &&
         frameStats->environmentEmissiveContributors > 0u);
    smoke->environmentViewValid = smoke->environmentViewValid &&
                                  frameStats->environmentViewValid;
    if (frameStats->environmentPalette >= 0 &&
        frameStats->environmentPalette < ENVIRONMENT_PALETTE_COUNT) {
        smoke->environmentPaletteMask |=
            (uint8_t)(1u << (unsigned int)frameStats->environmentPalette);
    }
    if (frameStats->environmentSceneDrawCalls >
        smoke->environmentMaximumSceneDrawCalls) {
        smoke->environmentMaximumSceneDrawCalls =
            frameStats->environmentSceneDrawCalls;
    }
    if (frameStats->environmentEmissiveDrawCalls >
        smoke->environmentMaximumEmissiveDrawCalls) {
        smoke->environmentMaximumEmissiveDrawCalls =
            frameStats->environmentEmissiveDrawCalls;
    }
    if ((fabsf(presentationCamera.rotation) > 0.001f ||
         fabsf(presentationCamera.zoom - aimCamera.zoom) > 0.001f) &&
        frameStats->environmentViewValid) {
        smoke->environmentCameraFeedback = true;
    }
    smoke->environmentZoomOut = smoke->environmentZoomOut ||
                                (frame >= 9 && output.viewScale > 1.35f &&
                                 frameStats->environmentViewValid);
    if (frame < ACCEPT_START) {
        /* Counted for the render phase alone. The gameplay phase carves and
           splits bodies on purpose, and every one of those is a legitimate
           new upload. */
        smoke->terrainTextureUpdates += frameStats->terrainBodyTextureUpdates;
        smoke->renderDetaches = game->detach.stats.autoDetachSucceeded;
    }
    if (frameStats->terrainBodyDrawCalls > smoke->terrainMaximumDrawCalls) {
        smoke->terrainMaximumDrawCalls = frameStats->terrainBodyDrawCalls;
    }
    if (frameStats->terrainBodyTextureMemoryBytes >
        smoke->terrainMaximumTextureBytes) {
        smoke->terrainMaximumTextureBytes =
            frameStats->terrainBodyTextureMemoryBytes;
    }
    smoke->terrainRendered = smoke->terrainRendered ||
                             (frameStats->visibleTerrainBodies == 1u &&
                              frameStats->cachedTerrainBodies == 1u &&
                              frameStats->terrainBodyDrawCalls >= 2u);
    smoke->terrainCollisionObserved =
        smoke->terrainCollisionObserved ||
        game->dynamicTerrain.stats.collisionContacts > 0;
    {
        const TerrainBody *body =
            DynamicTerrainGetConst(&game->dynamicTerrain, smoke->terrainBody);

        if (body != NULL) {
            smoke->terrainMoved =
                smoke->terrainMoved ||
                Vector2Distance(body->position, smoke->terrainStartPosition) >
                    0.05f;
            smoke->terrainRotated =
                smoke->terrainRotated ||
                fabsf(body->angle - smoke->terrainStartAngle) > 0.01f;
        }
    }
    if (frame == ACCEPT_START) {
        smoke->terrainCacheReleased =
            frameStats->cachedTerrainBodies == 0u &&
            frameStats->visibleTerrainBodies == 0u &&
            frameStats->terrainBodyTextureMemoryBytes == 0u;
    }
    smoke->bloomResized =
        smoke->bloomResized ||
        (frameStats->bloomEnabled &&
         frameStats->bloomWidth == (SMOKE_WINDOW_WIDTH - 320) / 2 &&
         frameStats->bloomHeight == (SMOKE_WINDOW_HEIGHT - 180) / 2);
    smoke->bloomRestored = smoke->bloomRestored ||
                           (smoke->bloomResized && frame >= 7 &&
                            frameStats->bloomWidth == SMOKE_WINDOW_WIDTH / 2 &&
                            frameStats->bloomHeight == SMOKE_WINDOW_HEIGHT / 2);
    if (frameStats->bloomEnabled && frame >= 8) {
        smoke->bloomSubmissionTotal += frameStats->bloomSubmissionMilliseconds;
        smoke->bloomSubmissionMaximum =
            fmax(smoke->bloomSubmissionMaximum,
                 frameStats->bloomSubmissionMilliseconds);
        ++smoke->bloomFrames;
    }
    if (frame >= 8 && worldStats != NULL) {
        smoke->prepareTotal += worldStats->preparationMilliseconds;
        smoke->prepareMaximum =
            fmax(smoke->prepareMaximum, worldStats->preparationMilliseconds);
        smoke->lightTotal += frameStats->lightMilliseconds;
        smoke->lightMaximum = fmax(smoke->lightMaximum,
                                   frameStats->lightMilliseconds);
        ++smoke->prepareFrames;
    }
    smoke->lightingObserved = smoke->lightingObserved ||
                              (frameStats->lightingEnabled &&
                               frameStats->lightUploads > 0u);
    smoke->resizeObserved =
        smoke->resizeObserved ||
        (frameStats->targetWidth == SMOKE_WINDOW_WIDTH - 320 &&
         frameStats->targetHeight == SMOKE_WINDOW_HEIGHT - 180);
    smoke->resizeRestored = smoke->resizeRestored ||
                            (smoke->resizeObserved && frame >= 7 &&
                             frameStats->targetWidth == SMOKE_WINDOW_WIDTH &&
                             frameStats->targetHeight == SMOKE_WINDOW_HEIGHT);
}

bool SmokeTestHidesHud(const SmokeTest *smoke)
{
    /* The two beam frames are photographs of a beam, and the debug panel sits
       exactly where the character does at the start of the run. */
    return smoke->frame == 3 || smoke->frame == 10;
}

void SmokeTestCapture(SmokeTest *smoke)
{
    int frame = smoke->frame;

    /* Whatever is still queued in the batch is part of this frame. */
    rlDrawRenderBatchActive();
    /* Mid-beam, so the frame shows where a beam actually leaves the
       character. It left the chest for a long time and nobody could tell from
       the reference screenshot, which is taken after the beam has stopped.
       Frame three: the beam is held and the explosion has not fired yet, so
       the picture is the beam and nothing else. Frame ten is the cryo beam for
       the same reason. */
    if (frame == 3) {
        TakeScreenshot("build/emberfall-beam.png");
    }
    if (frame == 10) {
        TakeScreenshot("build/emberfall-cryo.png");
    }
    if (frame == 9) {
        TakeScreenshot("build/emberfall-smoke-ember.png");
    } else if (frame == 10) {
        TakeScreenshot("build/emberfall-smoke-abyss.png");
    } else if (frame == 11) {
        TakeScreenshot("build/emberfall-smoke-storm.png");
    } else if (frame == 12) {
        TakeScreenshot("build/emberfall-smoke-dunes.png");
    } else if (frame == 13) {
        TakeScreenshot("build/emberfall-smoke-glacier.png");
    }
    if (frame == 16) {
        TakeScreenshot("build/emberfall-night.png");
    }
    if (frame == 26) {
        TakeScreenshot("build/emberfall-space.png");
    }
    if (frame == 10) {
        TakeScreenshot("build/emberfall-smoke.png");
    }
    /* Mid-hold, so the frame shows the telekinetic beam actually reaching a
       slab rather than the aftermath of having moved it. */
    if (frame == ACCEPT_GRAB + 30) {
        TakeScreenshot("build/emberfall-grab.png");
    }
    if (frame == ACCEPT_SHOT) {
        TakeScreenshot("build/emberfall-gameplay.png");
    }
    if (frame == MOVE_SHOT_FRAME) {
        TakeScreenshot("build/emberfall-movement.png");
    }
}

bool SmokeTestAdvance(SmokeTest *smoke)
{
    return ++smoke->frame >= MOVE_END;
}

int SmokeTestReport(const SmokeTest *smoke, const GameState *game,
                    const Renderer *renderer)
{
    const RendererFrameStats *frameStats = RendererStats(renderer);
    const SmokeMovement *movement = &smoke->movement;
    const SmokeAcceptance *acceptance = &smoke->acceptance;
    bool passed;

    if (smoke->bloomFrames > 0) {
        printf("Smoke render: bloom=%dx%d passes=%u targets=%u "
               "submit_avg=%.3fms submit_max=%.3fms "
               "prepare_avg=%.3fms prepare_max=%.3fms "
               "light_avg=%.3fms light_max=%.3fms light_uploads=%u "
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
               "move_cruise=%.1f move_boost=%.1f move_peak=%.1f "
               "move_drill=%.1f->%.1f move_turn=%.1f move_drilled=%d "
               "move_brake=%.1f->%.1f in %d frames move_reverse=%.1f "
               "move_final=%.1f\n",
               frameStats->bloomWidth, frameStats->bloomHeight,
               frameStats->offscreenPasses, frameStats->renderTargets,
               smoke->bloomSubmissionTotal / (double)smoke->bloomFrames,
               smoke->bloomSubmissionMaximum,
               smoke->prepareFrames > 0
                   ? smoke->prepareTotal / (double)smoke->prepareFrames
                   : 0.0,
               smoke->prepareMaximum,
               smoke->prepareFrames > 0
                   ? smoke->lightTotal / (double)smoke->prepareFrames
                   : 0.0,
               smoke->lightMaximum, frameStats->lightUploads,
               smoke->resizeObserved, smoke->resizeRestored, smoke->bloomResized,
               smoke->bloomRestored, smoke->targetsSynchronized,
               (unsigned int)frameStats->peakFx,
               (unsigned int)frameStats->droppedFx,
               smoke->terrainMaximumDrawCalls, smoke->terrainTextureUpdates,
               (double)smoke->terrainMaximumTextureBytes / 1024.0,
               smoke->terrainCollisionObserved, smoke->terrainCacheReleased,
               game->detach.stats.autoDetachSucceeded, smoke->autoDetachEvent,
               game->detach.stats.detachChecks, game->detach.stats.autoDetachCells,
               game->detach.stats.autoDetachRejectedAnchored,
               game->detach.stats.autoDetachRejectedUnknown,
               game->detach.stats.autoDetachRejectedTooSmall,
               game->detach.stats.autoDetachRejectedTooLarge,
               game->detach.stats.autoDetachRejectedBudget,
               (double)smoke->lightSpeed, (double)smoke->heavySpeed,
               (double)smoke->thrownSpin, smoke->massMattered,
               (double)smoke->forceSpeedBefore, (double)smoke->forceSpeedAfter,
               (double)smoke->forceShiftX,
               game->impulses.stats.bodiesAffectedByForce, smoke->forceMovedBody,
               game->impulses.stats.bodyImpulseApplications,
               (unsigned int)smoke->environmentPaletteMask,
               (unsigned int)smoke->environmentMaximumSceneDrawCalls,
               (unsigned int)smoke->environmentMaximumEmissiveDrawCalls,
               smoke->environmentCameraFeedback, smoke->environmentZoomOut,
               acceptance->detached, acceptance->pushed,
               (double)acceptance->pushSpeed, acceptance->grabbed,
               acceptance->dragged, (double)acceptance->dragDistance,
               acceptance->threw, (double)acceptance->throwSpeed,
               acceptance->carved, acceptance->split, acceptance->fragments,
               acceptance->fxDuringPlay, acceptance->cameraFeedbackDuringPlay,
               (double)movement->cruiseSpeed, (double)movement->boostSpeed,
               (double)movement->peakSpeed, (double)movement->drillEntrySpeed,
               (double)movement->drillLowSpeed, (double)movement->turnLateral,
               movement->drilled, (double)movement->brakeFrom,
               (double)movement->finalSpeed, movement->brakeFrames,
               (double)movement->reverseSpeed, (double)movement->finalSpeed);
    }

    passed = smoke->reactionObserved && smoke->laserHitObserved &&
             smoke->explosionObserved && smoke->forceObserved &&
             smoke->cryoObserved && smoke->boostObserved &&
             smoke->collisionObserved && smoke->drillObserved &&
             smoke->fireContained && smoke->bloomObserved &&
             smoke->targetsSynchronized && smoke->presentationFxObserved &&
             smoke->environmentObserved && smoke->environmentViewValid &&
             smoke->environmentCameraFeedback && smoke->environmentZoomOut &&
             smoke->environmentPaletteMask ==
                 (uint8_t)((1u << ENVIRONMENT_PALETTE_COUNT) - 1u) &&
             smoke->terrainExtracted && smoke->terrainWorldCleared &&
             smoke->terrainRendered && smoke->terrainMoved &&
             smoke->terrainRotated && smoke->terrainCollisionObserved &&
             smoke->terrainCacheReleased && smoke->autoDetachObserved &&
             smoke->autoDetachEvent && smoke->massMattered &&
             smoke->forceMovedBody && acceptance->detached &&
             acceptance->pushed && acceptance->grabbed && acceptance->dragged &&
             acceptance->threw && acceptance->carved && acceptance->split &&
             acceptance->fragments >= 2 && acceptance->fxDuringPlay &&
             acceptance->cameraFeedbackDuringPlay &&
             movement->cruiseSpeed >= 40.0f &&
             movement->boostSpeed >= movement->cruiseSpeed * 1.5f &&
             movement->peakSpeed >= movement->boostSpeed &&
             /* Rock is free: the flight leaves the wall no slower than it
                entered it, give or take the frame the impulse landed on. */
             movement->drillLowSpeed >= movement->drillEntrySpeed * 0.92f &&
             movement->turned && movement->drilled > 0 && movement->reversed &&
             movement->stopped && fabsf(smoke->thrownSpin) > 0.0f &&
             /* Two uploads — scene and emissive — for each body that ever
                existed, and not one more: a body whose raster never changes
                must not be re-uploaded per frame. */
             smoke->terrainTextureUpdates ==
                 2u * (1u + (unsigned int)smoke->renderDetaches) &&
             smoke->lightingObserved && smoke->mostActiveChunks > 0 &&
             smoke->mostActiveChunks <
                 game->world.chunkColumns * game->world.chunkRows;
    if (passed) {
        return 0;
    }
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
            "move=cruise%.0f/boost%.0f/peak%.0f/drill%.0f->%.0f/turn%.0f/"
            "drilled%d/rev%d/stop%d "
            "updates=%u lighting=%d chunks=%d(peak)/%d\n",
            smoke->reactionObserved, smoke->laserHitObserved,
            smoke->explosionObserved, smoke->forceObserved, smoke->cryoObserved,
            smoke->boostObserved, smoke->collisionObserved, smoke->drillObserved,
            smoke->fireContained, smoke->resizeObserved, smoke->resizeRestored,
            smoke->bloomObserved, smoke->bloomResized, smoke->bloomRestored,
            smoke->targetsSynchronized, smoke->presentationFxObserved,
            smoke->terrainExtracted, smoke->terrainWorldCleared,
            smoke->terrainRendered, smoke->terrainMoved, smoke->terrainRotated,
            smoke->terrainCollisionObserved, smoke->terrainCacheReleased,
            smoke->autoDetachObserved, smoke->autoDetachEvent,
            smoke->massMattered, smoke->forceMovedBody, (double)smoke->thrownSpin,
            smoke->environmentObserved, smoke->environmentViewValid,
            smoke->environmentCameraFeedback, smoke->environmentZoomOut,
            (unsigned int)smoke->environmentPaletteMask, acceptance->detached,
            acceptance->pushed, acceptance->grabbed, acceptance->dragged,
            acceptance->threw, acceptance->carved, acceptance->split,
            acceptance->fragments, acceptance->fxDuringPlay,
            acceptance->cameraFeedbackDuringPlay, (double)movement->cruiseSpeed,
            (double)movement->boostSpeed, (double)movement->peakSpeed,
            (double)movement->drillEntrySpeed, (double)movement->drillLowSpeed,
            (double)movement->turnLateral, movement->drilled, movement->reversed,
            movement->stopped, smoke->terrainTextureUpdates,
            smoke->lightingObserved, smoke->mostActiveChunks,
            game->world.chunkColumns * game->world.chunkRows);
    return 2;
}
