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
#define GAME_RNG_STREAM_POWERS 11u
#define GAME_RNG_STREAM_PARTICLES 12u

void GameReset(GameState *game, uint64_t seed)
{
    if (game == NULL || game->world.cells == NULL) {
        return;
    }

    game->worldSeed = seed;
    WorldGenerate(&game->world, seed);
    PlayerInit(&game->player, WorldPlayerSpawn(&game->world));
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
    if (player->impactStrength >= 14.0f) {
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_PLAYER_IMPACT,
            .position = player->impactPosition,
            .direction = player->impactNormal,
            .strength = player->impactStrength,
        });
        ParticlesSpawnImpact(&game->particles, player->impactPosition,
                             player->impactNormal, player->impactStrength);
    }
    if (player->boostTrailEmitted) {
        ParticlesSpawnBoostTrail(&game->particles, player->position,
                                 player->velocity);
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
        TerrainDetachProcess(&game->detach, &game->world, &game->dynamicTerrain,
                             events);
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
    PlayerUpdate(&game->player, &game->world, input->move, input->boostHeld,
                 deltaTime);
    GameActivatePlayerRegion(game);
    GamePublishPlayerFeedback(game, events);

    AbilitiesUpdate(&game->abilities, &game->world, &game->dynamicTerrain,
                    &game->damage, &game->impulses,
                    &game->particles, events,
                    PlayerBeamOrigin(&game->player, input->aimWorld),
                    input->aimWorld, deltaTime, input->ability);
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
