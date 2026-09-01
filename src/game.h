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
#include "terrain_detach.h"
#include "terrain_weld.h"
#include "terrain_damage.h"
#include "terrain_impulse.h"
#include "terrain_interaction.h"
#include "world.h"

/* Seconds in one full day. Long enough that a day is a stretch of play rather
   than a flicker, short enough that a session sees both a noon and a midnight.
   Emberfall is a game about carrying a light into the dark, and a night that
   never comes would waste that. */
#define GAME_DAY_SECONDS 420.0f

/* Daylight at a given phase, 0..1. Dawn at 0, noon at 0.25, dusk at 0.5,
   midnight at 0.75. The curve holds near full through the middle of the day and
   near zero through the middle of the night, and moves quickly between: a long
   even twilight reads as a bug in the lighting rather than as an evening. */
float GameDaylightAt(float dayPhase);

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
       read-only presentation are separate consumers. */
    DynamicTerrainSystem dynamicTerrain;
    /* Turns destructive damage into bodies. Owned beside the terrain it feeds
       and run on the fixed step, after the world has finished its own tick. */
    TerrainDetachSystem detach;
    TerrainWeldSystem weld;
    /* Blasts abilities want delivered to those bodies, held until the fixed
       step so that a fragment freed by a blast can be thrown by it. */
    TerrainImpulseSystem impulses;
    /* Cutting into bodies, and splitting them when a cut goes through. */
    TerrainDamageSystem damage;
    /* Everything the player does to a body directly: standing on it, shoving
       it, carrying it, throwing it. */
    TerrainInteractionSystem interaction;
    GameConfig config;
    /* The seed of the world currently loaded, and the stream that chooses the
       next one. Keeping the chooser in game state is what makes a whole session
       — including every regeneration — reproducible from GameConfig.seed. */
    uint64_t worldSeed;
    Rng seedSequence;
    /* Where the day has got to, 0..1 for one full cycle starting at dawn.
       Advanced on the fixed step, so the sky moves at the rate the simulation
       runs and not at the rate the machine draws. */
    float dayPhase;
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
