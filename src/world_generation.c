/* World generation lifecycle and the spawn query. Procedural biome composition
   lives in world_biomes.c; keeping reset here makes all World-owned transient
   state return to one well-defined baseline before terrain is written. */
#include "world_internal.h"

#include <string.h>

/* Generation draws from a stream of its own rather than from World.rng, so
   that later gameplay effects cannot shift the terrain a seed produces. */
#define WORLD_RNG_STREAM_EFFECTS 2u

void WorldGenerate(World *world, uint64_t seed)
{
    size_t cellIndex;
    size_t cellCount;
    size_t chunkCount;

    if (world == NULL || world->cells == NULL) {
        return;
    }

    world->seed = seed;
    RngSeed(&world->rng, RngStreamSeed(seed, WORLD_RNG_STREAM_EFFECTS));
    cellCount = (size_t)world->width * (size_t)world->height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    memset(world->cells, 0, cellCount * sizeof(*world->cells));
    memset(world->activeChunks, 0, chunkCount * sizeof(*world->activeChunks));
    memset(world->nextActiveChunks, 0, chunkCount * sizeof(*world->nextActiveChunks));
    memset(world->activeRowCount, 0,
           (size_t)world->chunkRows * sizeof(*world->activeRowCount));
    memset(world->nextRowCount, 0,
           (size_t)world->chunkRows * sizeof(*world->nextRowCount));
    world->simulating = false;
    memset(world->dirtyChunks, 1, chunkCount * sizeof(*world->dirtyChunks));
    memset(world->lightDirtyChunks, 1,
           chunkCount * sizeof(*world->lightDirtyChunks));
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        world->cells[cellIndex].temperature = AMBIENT_TEMPERATURE;
    }
    world->tick = 0;
    world->effectSerial = 0;
    world->lastTickStats = (WorldTickStats){0};
    world->reactionCount = 0;
    world->activeChunkCount = 0;
    world->lightSolved = false;
    /* Damage recorded against the world that just ceased to exist. Carrying it
       across would send the next detach check to coordinates that now describe
       completely different terrain. */
    world->destructionCount = 0;
    world->destructionDropped = 0;

    WorldGenerateBiomeTerrain(world);
    WorldCountActiveState(world);
}

Vector2 WorldPlayerSpawn(const World *world)
{
    int x;
    int y;

    if (world == NULL || world->cells == NULL) {
        return (Vector2){0.0f, 0.0f};
    }

    /* The central plateau is feature-free, but derive the spawn from actual
       cells so tuning any biome surface cannot place the player inside it. */
    x = world->width / 2;
    for (y = 0; y < world->height; ++y) {
        if (MaterialIsSolid(WorldMaterialAt(world, x, y))) {
            break;
        }
    }
    y -= 10;
    if (y < 4) {
        y = 4;
    }
    return (Vector2){(float)x + 0.5f, (float)y + 0.5f};
}
