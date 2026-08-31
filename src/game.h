#ifndef GAME_H
#define GAME_H

#include <stdbool.h>

#include "game_events.h"
#include "game_input.h"
#include "particles.h"
#include "player.h"
#include "powers.h"
#include "world.h"

typedef struct GameConfig {
    int worldWidth;
    int worldHeight;
    float fixedStep;
    float activeRadiusX;
    float activeRadiusY;
} GameConfig;

typedef struct GameState {
    World world;
    Player player;
    PowerSystem powers;
    /* Transitional ownership: gameplay debris can mutate World, while visual
       particles will move to presentation in the next phase. */
    ParticleSystem particles;
    GameConfig config;
    float simulationAccumulator;
    int activatedPlayerChunkX;
    int activatedPlayerChunkY;
} GameState;

GameConfig GameDefaultConfig(void);
bool GameInit(GameState *game, GameConfig config);
void GameReset(GameState *game);
void GameUpdate(GameState *game, const GameInput *input, float deltaTime,
                GameEventBuffer *events);
void GameUnload(GameState *game);

#endif
