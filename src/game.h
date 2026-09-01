#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "game_events.h"
#include "game_input.h"
#include "particles.h"
#include "player.h"
#include "abilities.h"
#include "dynamic_terrain.h"
#include "world.h"

typedef struct GameConfig {
    int worldWidth;
    int worldHeight;
    float fixedStep;
    float activeRadiusX;
    float activeRadiusY;
    /* Seed for the first world. Zero means "pick one at init", which is what a
       normal play session wants; a fixed value replays a session exactly,
       including the worlds that later regenerations produce. */
    uint64_t seed;
} GameConfig;

typedef struct GameState {
    World world;
    Player player;
    AbilitySystem abilities;
    /* Transitional ownership: gameplay debris can mutate World, while visual
       particles will move to presentation in the next phase. */
    ParticleSystem particles;
    /* Pieces of terrain that have stopped being part of the cellular world.
       Owned here because it is gameplay state with the same lifetime as the
       world it was cut from. Explicit extraction, fixed-step physics and
       read-only presentation are separate consumers; automatic detach is not
       connected to ordinary gameplay yet. */
    DynamicTerrainSystem dynamicTerrain;
    GameConfig config;
    /* The seed of the world currently loaded, and the stream that chooses the
       next one. Keeping the chooser in game state is what makes a whole session
       — including every regeneration — reproducible from GameConfig.seed. */
    uint64_t worldSeed;
    Rng seedSequence;
    float simulationAccumulator;
    int activatedPlayerChunkX;
    int activatedPlayerChunkY;
} GameState;

GameConfig GameDefaultConfig(void);
bool GameInit(GameState *game, GameConfig config);
/* Regenerates the world from `seed` and resets every gameplay system. */
void GameReset(GameState *game, uint64_t seed);
/* Draws the next seed from the session's own sequence and resets to it. */
void GameRegenerate(GameState *game);
void GameUpdate(GameState *game, const GameInput *input, float deltaTime,
                GameEventBuffer *events);
void GameUnload(GameState *game);

#endif
