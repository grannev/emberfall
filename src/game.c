#include <math.h>
#include "game.h"

#include <stddef.h>
#include <string.h>

#include <time.h>

#include <raymath.h>

#include "terrain_physics.h"
#include "terrain_weld.h"

#define DEFAULT_WORLD_WIDTH 16384
/* All of what this buys above WORLD_GROUND_ROWS is sky. The ground is laid out
   in a band of that many rows at the bottom whatever the height, so raising
   this raises the ceiling and lengthens the climb to space without deepening
   the soil or growing the hills, and the rows of sky it adds cost no memory
   until something is written into them.

   At 4096 the surface sits some 2000 cells under the cloud line, the
   atmosphere the character burns through on the way down is a corridor over
   a thousand cells deep, and above it there are five hundred cells of open
   space to fly in — enough that leaving and re-entering are journeys with a
   middle rather than a line crossed twice in a second. The cell array is
   805 MiB of address space and the same ~162 MiB resident as before, since
   not one of the added rows is ever written; the light field, which is
   solved eight cells at a time, doubles to 10 MiB. */
#define DEFAULT_WORLD_HEIGHT 4096
#define DEFAULT_FIXED_STEP (1.0f / 60.0f)
#define DEFAULT_ACTIVE_RADIUS_X 480.0f
#define DEFAULT_ACTIVE_RADIUS_Y 288.0f

GameConfig GameDefaultConfig(void)
{
    return (GameConfig){
        .worldWidth = DEFAULT_WORLD_WIDTH,
        .worldHeight = DEFAULT_WORLD_HEIGHT,
        .fixedStep = DEFAULT_FIXED_STEP,
        .activeRadiusX = DEFAULT_ACTIVE_RADIUS_X,
        .activeRadiusY = DEFAULT_ACTIVE_RADIUS_Y,
    };
}

float GameDaylightAt(float dayPhase)
{
    /* Wrapped rather than clamped: the phase is a position on a circle, and a
       caller that has let it run past one is asking about the next day. */
    float phase = dayPhase - floorf(dayPhase);
    /* A cosine peaking at 0.25 and troughing at 0.75, then pushed toward its
       ends so that noon and midnight are flat and the transitions are quick. */
    float wave = 0.5f - 0.5f * cosf((phase + 0.25f) * 2.0f * PI);
    float shaped = wave * wave * (3.0f - 2.0f * wave);

    /* Never quite black. A moonless world where the surface is as dark as
       sealed rock is one where flying at night is indistinguishable from flying
       inside a mountain, and the player has no horizon to steer by. */
    return 0.06f + 0.94f * shaped;
}

bool GameInit(GameState *game, GameConfig config)
{
    if (game == NULL || config.worldWidth <= 0 || config.worldHeight <= 0 ||
        config.fixedStep <= 0.0f || config.activeRadiusX <= 0.0f ||
        config.activeRadiusY <= 0.0f) {
        return false;
    }

    memset(game, 0, sizeof(*game));
    /* Same reasoning as the material table: a malformed ability table cannot
       produce a game anyone can play, and failing at init names it. */
    if (!AbilitiesValidate()) {
        return false;
    }
    game->config = config;
    if (!WorldInit(&game->world, config.worldWidth, config.worldHeight)) {
        return false;
    }
    if (!DynamicTerrainInit(&game->dynamicTerrain)) {
        WorldUnload(&game->world);
        return false;
    }
    TerrainDetachInit(&game->detach);
    TerrainWeldInit(&game->weld);
    TerrainImpulseInit(&game->impulses);
    TerrainDamageInit(&game->damage);
    TerrainInteractionInit(&game->interaction);
    FluidInteractionInit(&game->fluid);
    TerrainFluidInit(&game->bodyFluid);
    TerrainStabilityInit(&game->stability);
    AtmosphereInit(&game->atmosphere);
    /* A session with no configured seed still has to be describable after the
       fact, so one is drawn once here and every world in the session follows
       from it. The debug HUD shows the world's seed for that reason. */
    RngSeed(&game->seedSequence,
            config.seed != 0u ? config.seed : (uint64_t)time(NULL));
    GameRegenerate(game);
    return true;
}

/* Independent streams derived from the world seed, so that adding a draw to
   one system cannot shift what another produces. */
/* Falling pieces of back wall one cut may hand to presentation; the rest
   of what came away simply goes. */
#define GAME_BACK_WALL_PIECES 48

#define GAME_RNG_STREAM_POWERS 11u
#define GAME_RNG_STREAM_PARTICLES 12u

void GameReset(GameState *game, uint64_t seed)
{
    if (game == NULL || game->world.cells == NULL) {
        return;
    }

    game->worldSeed = seed;
    game->wrapShift = 0.0f;
    game->wraps = 0;
    WorldGenerate(&game->world, seed);
    PlayerInit(&game->player, WorldPlayerSpawn(&game->world));
    /* A new world starts on foot, standing on it. */
    game->player.mode = PLAYER_MODE_WALK;
    AbilitiesInit(&game->abilities, RngStreamSeed(seed, GAME_RNG_STREAM_POWERS));
    ParticlesInit(&game->particles, RngStreamSeed(seed, GAME_RNG_STREAM_PARTICLES));
    /* A new world cannot keep pieces cut from the old one. WorldGenerate has
       already dropped the damage log for the same reason. */
    DynamicTerrainReset(&game->dynamicTerrain);
    TerrainDetachResetStats(&game->detach);
    TerrainWeldInit(&game->weld);
    /* A blast queued against a world that no longer exists must not land in the
       new one, and a hold cannot survive the body it was on. */
    TerrainImpulseInit(&game->impulses);
    TerrainDamageInit(&game->damage);
    TerrainInteractionInit(&game->interaction);
    FluidInteractionInit(&game->fluid);
    TerrainFluidInit(&game->bodyFluid);
    TerrainStabilityInit(&game->stability);
    AtmosphereInit(&game->atmosphere);
    game->simulationAccumulator = 0.0f;
    game->activatedPlayerChunkX = -1;
    game->activatedPlayerChunkY = -1;
}

void GameRegenerate(GameState *game)
{
    if (game == NULL) {
        return;
    }
    GameReset(game, RngNext(&game->seedSequence));
}

static void GameActivatePlayerRegion(GameState *game)
{
    int chunkX = (int)game->player.position.x / WORLD_CHUNK_SIZE;
    int chunkY = (int)game->player.position.y / WORLD_CHUNK_SIZE;

    if (chunkX == game->activatedPlayerChunkX &&
        chunkY == game->activatedPlayerChunkY) {
        return;
    }

    WorldActivateRegion(
        &game->world,
        (Rectangle){game->player.position.x - game->config.activeRadiusX,
                    game->player.position.y - game->config.activeRadiusY,
                    game->config.activeRadiusX * 2.0f,
                    game->config.activeRadiusY * 2.0f});
    game->activatedPlayerChunkX = chunkX;
    game->activatedPlayerChunkY = chunkY;
}

static void GameApplyLanding(GameState *game, GameEventBuffer *events)
{
    Player *player = &game->player;
    float strength;
    float radius;
    Vector2 at;

    if (player->landingSpeed < PLAYER_HEAVY_LANDING_SPEED) return;
    strength = Clamp((player->landingSpeed - PLAYER_HEAVY_LANDING_SPEED) /
                      (PLAYER_FALL_SPEED_LIMIT - PLAYER_HEAVY_LANDING_SPEED), 0.0f, 1.0f);
    radius = Lerp(8.0f, 28.0f, strength);
    at = player->landingPosition;
    if (!player->landingOnBody) {
        int step;
        /* Feet stop just clear of the cell: put the dent on its surface. */
        at.y += 0.6f;
        WorldApplyPunch(&game->world, at, (Vector2){0.0f, 1.0f}, (int)radius,
                        5 + (int)(strength * 7.0f), 14 + (int)(strength * 38.0f));
        /* Follow the newly formed shallow bowl with bounded collision steps.
           If it opened into a cave the hero resumes falling, never teleports. */
        for (step = 0; step < 44; ++step) {
            Vector2 next = {player->position.x, player->position.y + 0.5f};
            if (PlayerCollidesAt(player, &game->world, next) ||
                next.y > (float)game->world.height - PlayerExtent(player)) break;
            player->position = next;
        }
        player->grounded = PlayerCollidesAt(player, &game->world,
            (Vector2){player->position.x, player->position.y + 0.6f});
    }
    WorldApplyShockwave(&game->world, (int)floorf(at.x), (int)floorf(at.y),
                        0, (int)(radius * 3.0f));
    (void)TerrainImpulseQueueBlast(&game->impulses, (TerrainBlast){
        .shape = TERRAIN_BLAST_RADIAL,
        .origin = at,
        .radius = radius * 3.0f,
        .momentum = Lerp(16000.0f, 90000.0f, strength),
        .carveRadius = player->landingOnBody ? radius * 0.4f : 0.0f,
    });
    ParticlesSpawnImpact(&game->particles, at, (Vector2){0.0f, -1.0f},
                         player->landingSpeed);
    (void)GameEventsPush(events, (GameEvent){
        .type = GAME_EVENT_HEAVY_LANDING,
        .position = at,
        .direction = {0.0f, -1.0f},
        .strength = strength,
        .radius = radius * 3.0f,
    });
    player->landingSpeed = 0.0f;
}

static void GamePublishPlayerFeedback(GameState *game, GameEventBuffer *events)
{
    Player *player = &game->player;

    if (player->boostEngaged) {
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_BOOST_ENGAGED,
            .position = player->position,
            .direction = player->velocity,
        });
        ParticlesSpawnBoostBurst(&game->particles, player->position,
                                 player->velocity);
    }
    /* Only a boosted collision is an impact the player is shown: bumping a
       wall or landing in ordinary flight has no flash, no dust, no shake and
       no sound — the player asked for them gone, it is how the character
       moves every second and it is not an event. */
    if (player->impactStrength >= 14.0f && player->boosting) {
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_PLAYER_IMPACT,
            .position = player->impactPosition,
            .direction = player->impactNormal,
            .strength = player->impactStrength,
        });
        ParticlesSpawnImpact(&game->particles, player->impactPosition,
                             player->impactNormal, player->impactStrength);
    }
    if (player->brushedLeaves > 0) {
        ParticlesSpawnLeaves(&game->particles, player->position, player->velocity,
                             player->brushedLeaves);
    }
    if (player->drilledCells > 0) {
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_PLAYER_DRILL,
            .position = player->drillPosition,
            .direction = player->velocity,
            .material = player->drillMaterial,
            .count = player->drilledCells,
        });
        ParticlesSpawnDrillDebris(&game->particles, player->drillPosition,
                                  player->velocity, player->drilledCells);
    }
}

/* Presentation and physics reactions that are the same for every ability: the
   pose it puts the player in, and any knockback it published. Adding a power
   does not add a case here. */
static void GameApplyAbilityFeedback(GameState *game, const GameEventBuffer *events)
{
    uint16_t index;
    int id;

    for (id = 0; id < ABILITY_COUNT; ++id) {
        const AbilityState *state = &game->abilities.states[id];
        const AbilityDefinition *definition = AbilityDefinitionAt((AbilityId)id);

        if (state->active && definition->poseHold > 0.0f) {
            PlayerSetPose(&game->player, definition->pose, definition->poseHold);
        }
    }
    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];

        if (event->playerImpulse.x != 0.0f || event->playerImpulse.y != 0.0f) {
            PlayerApplyImpulse(&game->player, event->playerImpulse);
        }
    }
}

static void GameAdvanceWorld(GameState *game, GameEventBuffer *events)
{
    while (game->simulationAccumulator >= game->config.fixedStep) {
        int reaction;

        WorldUpdate(&game->world);
        /* The same log, read before it is drained: an opening that was just
           made is where a ceiling may have lost its support. */
        TerrainStabilityNoteDestruction(&game->stability, &game->world);
        /* Between the world's tick and the bodies': the world has finished
           every cell write it was going to make, so connectivity now describes
           a state that actually existed, and a piece that comes loose here is
           integrated by the very next line instead of hanging for a tick.

           This is also the only place automatic detachment runs. It does no
           scanning of its own — it drains the damage the destructive powers
           recorded, and a tick with no destruction in it does nothing at all. */
        {
            /* The back layer is asked after the terrain: what came loose
               here has left the world by now, and a wall that stood only
               because that did stands on nothing. The log is copied first,
               because the detach check drains it. */
            WorldDestructionRegion cut[MAX_WORLD_DESTRUCTION_REGIONS];
            int cuts = game->world.destructionCount;
            int index;

            memcpy(cut, game->world.destruction, (size_t)cuts * sizeof(cut[0]));
            TerrainDetachProcess(&game->detach, &game->world, &game->dynamicTerrain,
                                 events);
            for (index = 0; index < cuts; ++index) {
                WorldBackWallPiece pieces[GAME_BACK_WALL_PIECES];
                int count = WorldBreakBackWalls(&game->world, cut[index].minimumX,
                                                cut[index].minimumY, cut[index].maximumX,
                                                cut[index].maximumY, pieces,
                                                GAME_BACK_WALL_PIECES);
                int piece;

                for (piece = 0; piece < count; ++piece) {
                    (void)GameEventsPush(events, (GameEvent){
                        .type = GAME_EVENT_BACK_WALL_FALL,
                        .position = {(float)pieces[piece].x, (float)pieces[piece].y},
                        .material = (CellMaterial)pieces[piece].material,
                        .count = (int)pieces[piece].mask,
                    });
                }
            }
        }
        /* After the detach check has drained the log: what crumbles here is
           logged for the next tick's check, so a slab undermined by a
           cave-in comes loose the way a slab cut by a blast does. */
        (void)TerrainStabilityProcess(&game->stability, &game->world);
        /* After detachment and before integration. Both halves matter: a piece
           the blast just cut free has to exist before it can be thrown, and it
           has to be thrown before it is stepped, or the throw would arrive a
           tick late. The queue empties here, so a blast lands exactly once
           however many fixed steps this frame runs. */
        (void)TerrainImpulseApply(&game->impulses, &game->dynamicTerrain,
                                  &game->damage, &game->world);
        /* On the fixed step, beside the world: bodies must advance at the same
           rate the simulation does, never at the renderer's frame rate. The
           world goes in as a const pointer, which is what makes it impossible
           for collision to change a cell. */
        /* The sky moves on the fixed step, like everything else the world
           agrees on, so the same seed and the same inputs see the same
           midnight. */
        game->dayPhase += game->config.fixedStep / GAME_DAY_SECONDS;
        if (game->dayPhase >= 1.0f) game->dayPhase -= floorf(game->dayPhase);
        WorldSetDaylight(&game->world, GameDaylightAt(game->dayPhase));
        /* Before the bodies are stepped: the liquid a body is in changes the
           velocity the step integrates, and a body that breaks the surface
           this step shoves the liquid before the liquid's next tick. */
        /* Before the bodies are integrated, like the liquid: the air a body
           is falling through changes how fast it is going and how hot it is
           by the time it lands. */
        AtmosphereUpdateBodies(&game->atmosphere, &game->dynamicTerrain,
                               &game->damage, &game->world, events,
                               game->config.fixedStep);
        TerrainFluidUpdate(&game->bodyFluid, &game->dynamicTerrain, &game->world,
                           events, game->config.fixedStep);
        TerrainPhysicsUpdate(&game->dynamicTerrain, &game->world,
                             game->config.fixedStep);
        /* Right after the contacts that stopped them: a body that hit the
           ground this step harder than rock bears cracks from where it hit,
           and the pieces are bodies before the next step. */
        (void)TerrainDamageImpactFractures(&game->damage, &game->dynamicTerrain);
        /* After integration, so a body that settled on this very step starts
           its rest here rather than a step late. Rubble that has lain still
           long enough stops being a body and becomes ground again, which is
           what returns its slot and its share of the dynamic cell budget to a
           player who keeps demolishing things. */
        (void)TerrainWeldProcess(&game->weld, &game->world,
                                 &game->dynamicTerrain, game->player.position,
                                 game->config.fixedStep);
        for (reaction = 0; reaction < game->world.reactionCount; ++reaction) {
            Vector2 position = game->world.reactions[reaction].position;

            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_MATERIAL_REACTION,
                .position = position,
            });
            ParticlesSpawnSteam(&game->particles, position);
        }
        game->simulationAccumulator -= game->config.fixedStep;
    }
}

/* The world wraps: its right edge is joined to its left. The character is
   kept inside the map — moved back a whole width when it crosses the seam —
   and everything that moves is kept within half a turn of the world of it,
   in the copy of the world the character is in. Terrain bodies and particles
   hold unwrapped positions, and the world answers any column by wrapping it,
   so this is only about agreeing on which copy: a body a few cells past the
   seam from the character must be a few cells away in coordinates too, or
   the two would never meet.

   What cannot agree is what lies exactly opposite the character, half the
   world away: two bodies there may straddle the line where "nearest copy"
   flips, and would not collide with each other. Nothing the player can see
   is ever there. */
static float GameWrapAround(GameState *game, float x)
{
    float width = (float)game->world.width;
    float offset = x - game->player.position.x;

    if (offset > width * 0.5f) {
        x -= width * ceilf((offset - width * 0.5f) / width);
    } else if (offset < -width * 0.5f) {
        x += width * ceilf((-offset - width * 0.5f) / width);
    }
    return x;
}

static void GameKeepInWorld(GameState *game)
{
    float width = (float)game->world.width;
    int index;

    game->wrapShift = 0.0f;
    if (game->player.position.x < 0.0f || game->player.position.x >= width) {
        float shift = -floorf(game->player.position.x / width) * width;

        game->player.position.x += shift;
        game->player.landingPosition.x += shift;
        for (index = 0; index < ABILITY_COUNT; ++index) {
            game->abilities.states[index].origin.x += shift;
            game->abilities.states[index].endpoint.x += shift;
            game->abilities.states[index].dwellPoint.x += shift;
        }
        game->wrapShift = shift;
        ++game->wraps;
    }
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        TerrainBody *body = &game->dynamicTerrain.bodies[index];

        if (body->active) {
            body->position.x = GameWrapAround(game, body->position.x);
        }
    }
    for (index = 0; index < MAX_PARTICLES; ++index) {
        Particle *particle = &game->particles.particles[index];

        if (particle->active) {
            particle->position.x = GameWrapAround(game, particle->position.x);
        }
    }
}

void GameUpdate(GameState *game, const GameInput *input, float deltaTime,
                GameEventBuffer *events)
{
    if (events == NULL) {
        return;
    }
    GameEventsClear(events);
    if (game == NULL || input == NULL || game->world.cells == NULL) {
        return;
    }
    game->wrapShift = 0.0f;
    deltaTime = Clamp(deltaTime, 0.0f, 0.05f);
    if (input->regeneratePressed) {
        GameRegenerate(game);
    }

    /* Before the character moves: the liquid they are in sets the drag they
       integrate this frame, and the surface they are about to break is
       still where they are about to break it. */
    /* On the frame, beside the liquid, and before the character moves: the
       air never slows them, it only heats them. */
    AtmosphereUpdatePlayer(&game->atmosphere, &game->player, &game->world,
                           events, deltaTime);
    FluidInteractionUpdatePlayer(&game->fluid, &game->player, &game->world,
                                 events, deltaTime);
    /* On foot, up is a jump as well as jump is; in flight only jump is, and
       two taps of it land. Shift is the boost in the air and a run on the
       ground. */
    game->player.jumpPressed =
        input->jumpPressed ||
        (game->player.mode == PLAYER_MODE_WALK && input->upPressed);
    game->player.jumpHeld = input->jumpHeld;
    game->player.runHeld = input->boostHeld;
    {
        bool grounded = game->player.grounded;
        PlayerMode mode = game->player.mode;
        int foot = (int)(game->player.walkPhase * 2.0f);
        PlayerUpdate(&game->player, &game->world, input->move, input->boostHeld,
                     deltaTime);
        {
            float speed = Vector2Length(game->player.velocity);
            float density = WorldGravityScaleAt(&game->world,
                                                game->player.position.y);

            if (speed < game->player.sonicSpeed * 0.85f || density <= 0.02f)
                game->player.sonicBreakArmed = true;
            if (game->player.mode == PLAYER_MODE_FLY &&
                game->player.sonicBreakArmed &&
                speed >= game->player.sonicSpeed && density > 0.06f) {
                (void)GameEventsPush(events, (GameEvent){
                    .type = GAME_EVENT_SONIC_BREAK,
                    .position = game->player.position,
                    .direction = Vector2Scale(game->player.velocity, 1.0f / speed),
                    .strength = density,
                    .radius = 36.0f,
                });
                game->player.sonicBreakArmed = false;
            }
        }
        if (game->player.boosting && Vector2Length(game->player.velocity) > game->player.maxSpeed &&
            (int)(game->player.animationTime * 40.0f) !=
            (int)((game->player.animationTime - deltaTime) * 40.0f)) {
            ParticlesSpawnBoostTrail(&game->particles, game->player.position, game->player.velocity);
        }
        if (grounded && !game->player.grounded && game->player.velocity.y < -40.0f) {
            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_TAKEOFF, .position = PlayerFeet(&game->player),
                .strength = input->boostHeld ? 1.0f : 0.4f,
            });
        } else if (mode == PLAYER_MODE_WALK && game->player.mode == PLAYER_MODE_FLY) {
            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_TAKEOFF, .position = PlayerFeet(&game->player),
                .strength = 0.25f,
            });
        } else if (game->player.grounded && fabsf(game->player.velocity.x) > 12.0f &&
                   foot != (int)(game->player.walkPhase * 2.0f)) {
            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_FOOTSTEP, .position = PlayerFeet(&game->player),
                .strength = Clamp(fabsf(game->player.velocity.x) / PLAYER_RUN_SPEED, 0.0f, 1.0f),
            });
        }
    }
    (void)PlayerBrushFlora(&game->player, &game->world);
    GameActivatePlayerRegion(game);
    GameApplyLanding(game, events);
    GamePublishPlayerFeedback(game, events);

    if (input->cancelCharge) AbilitiesCancelCharge(&game->abilities);
    {
        Vector2 eye = PlayerBeamOrigin(&game->player, input->aimWorld);
        Vector2 origins[ABILITY_COUNT];
        int ability;

        for (ability = 0; ability < ABILITY_COUNT; ++ability) origins[ability] = eye;
        origins[ABILITY_FORCE] = PlayerForceOrigin(&game->player, input->aimWorld);
        AbilitiesUpdateFromOrigins(&game->abilities, &game->world,
                                   &game->dynamicTerrain, &game->damage,
                                   &game->impulses, &game->particles, events,
                                   origins, input->aimWorld, deltaTime,
                                   input->ability);
    }
    GameApplyAbilityFeedback(game, events);

    ParticlesUpdate(&game->particles, &game->world, deltaTime);
    game->simulationAccumulator += deltaTime;
    GameAdvanceWorld(game, events);
    PlayerResolveWorldCollision(&game->player, &game->world);
    /* Last, so the correction a body applies to the player is the final word on
       where they are: nothing after this can push them back inside one. */
    /* Holding a slab is a two-handed gesture, and the hold is drawn from both
       hands, so the arms have to be out there. Refreshed every frame the hold
       lasts; the pose lapses on its own once it stops. */
    if (TerrainInteractionIsHolding(&game->interaction, &game->dynamicTerrain)) {
        PlayerSetPose(&game->player, PLAYER_POSE_CHILL, 0.12f);
    }
    TerrainInteractionUpdate(&game->interaction, &game->player,
                             &game->dynamicTerrain, &game->damage,
                             input->aimWorld, input->grabHeld, deltaTime);
    GameApplyLanding(game, events);
    GameKeepInWorld(game);
}

void GameUnload(GameState *game)
{
    if (game == NULL) {
        return;
    }
    DynamicTerrainUnload(&game->dynamicTerrain);
    WorldUnload(&game->world);
    memset(game, 0, sizeof(*game));
}
