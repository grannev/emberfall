#include "game.h"

#include <stddef.h>
#include <string.h>

#include <time.h>

#include <raymath.h>

#define DEFAULT_WORLD_WIDTH 16384
#define DEFAULT_WORLD_HEIGHT 864
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

    if (player->boostStageChanged != PLAYER_BOOST_NONE) {
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_BOOST_STAGE,
            .position = player->position,
            .direction = player->velocity,
            .count = (int)player->boostStageChanged,
        });
        ParticlesSpawnBoostBurst(&game->particles, player->position,
                                 player->velocity,
                                 (int)player->boostStageChanged);
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
                                 player->velocity, (int)player->boostStage);
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

    PlayerUpdate(&game->player, &game->world, input->move, input->boostHeld,
                 deltaTime);
    GameActivatePlayerRegion(game);
    GamePublishPlayerFeedback(game, events);

    AbilitiesUpdate(&game->abilities, &game->world, &game->particles, events,
                    game->player.position, input->aimWorld, deltaTime,
                    input->ability);
    GameApplyAbilityFeedback(game, events);

    ParticlesUpdate(&game->particles, &game->world, deltaTime);
    game->simulationAccumulator += deltaTime;
    GameAdvanceWorld(game, events);
    PlayerResolveWorldCollision(&game->player, &game->world);
}

void GameUnload(GameState *game)
{
    if (game == NULL) {
        return;
    }
    WorldUnload(&game->world);
    memset(game, 0, sizeof(*game));
}
