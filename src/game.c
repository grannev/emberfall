#include "game.h"

#include <stddef.h>
#include <string.h>

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
    game->config = config;
    if (!WorldInit(&game->world, config.worldWidth, config.worldHeight)) {
        return false;
    }
    GameReset(game);
    return true;
}

void GameReset(GameState *game)
{
    if (game == NULL || game->world.cells == NULL) {
        return;
    }

    WorldGenerate(&game->world);
    PlayerInit(&game->player, WorldPlayerSpawn(&game->world));
    PowersInit(&game->powers);
    ParticlesInit(&game->particles);
    game->simulationAccumulator = 0.0f;
    game->activatedPlayerChunkX = -1;
    game->activatedPlayerChunkY = -1;
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

static void GameApplyPowerFeedback(GameState *game, GameEventBuffer *events)
{
    PowerSystem *powers = &game->powers;
    Player *player = &game->player;

    if (powers->laserActive) {
        PlayerSetPose(player, PLAYER_POSE_LASER, 0.06f);
        if (powers->laserHit) {
            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_LASER_HIT,
                .position = powers->laserEnd,
                .material = powers->laserHitMaterial,
            });
        }
    } else if (powers->chillActive) {
        PlayerSetPose(player, PLAYER_POSE_CHILL, 0.06f);
        if (powers->chillHit) {
            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_CRYO_HIT,
                .position = powers->chillEnd,
            });
        }
    }

    if (powers->forceTriggered) {
        PlayerSetPose(player, PLAYER_POSE_BLAST, 0.28f);
        PlayerApplyImpulse(player,
                           (Vector2){-powers->forceDirection.x * powers->forceRecoil,
                                     -powers->forceDirection.y * powers->forceRecoil});
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_FORCE,
            .position = powers->forceOrigin,
            .direction = powers->forceDirection,
            .strength = powers->forceRecoil,
            .radius = powers->forceLength,
        });
    }
    if (powers->explosionTriggered) {
        PlayerApplyExplosionImpulse(player, powers->explosionPosition,
                                    powers->explosionShockRadius, 145.0f);
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_EXPLOSION,
            .position = powers->explosionPosition,
            .radius = powers->explosionShockRadius,
            .strength = 145.0f,
        });
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
        GameReset(game);
    }

    PlayerUpdate(&game->player, &game->world, input->move, input->boostHeld,
                 deltaTime);
    GameActivatePlayerRegion(game);
    GamePublishPlayerFeedback(game, events);

    PowersUpdate(&game->powers, &game->world, &game->particles,
                 game->player.position, input->aimWorld, deltaTime,
                 input->laserHeld, input->explosionPressed,
                 input->forcePressed, input->chillHeld);
    GameApplyPowerFeedback(game, events);

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
