#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#define WORLD_CHUNK_SIZE 32
/* Light is solved on a coarser grid than the cells. Eight divides the chunk
   size, so every light cell belongs to exactly one chunk and the dirty-chunk
   bookkeeping stays exact. The field is smooth and sampled bilinearly, so a
   finer grid buys no visible detail and costs four times the solve. */
#define WORLD_LIGHT_SCALE 8

typedef enum CellMaterial {
    MATERIAL_EMPTY = 0,
    MATERIAL_DIRT,
    MATERIAL_ROCK,
    MATERIAL_SAND,
    MATERIAL_WATER,
    MATERIAL_LAVA,
    MATERIAL_STEAM,
    MATERIAL_SMOKE,
    MATERIAL_FIRE,
    MATERIAL_ASH,
    MATERIAL_ICE,
    MATERIAL_COUNT
} CellMaterial;

typedef struct Cell {
    uint32_t updatedTick;
    uint32_t effectStamp;
    float temperature;
    uint16_t lifetime;
    /* MATERIAL_COUNT is deliberately kept below 256. Storing the enum as an
       int wasted four bytes in every cell; on the 16384-wide world that was
       about 54 MiB for no gameplay value. */
    uint8_t material;
} Cell;

_Static_assert(MATERIAL_COUNT <= UINT8_MAX, "Cell.material no longer fits in uint8_t");
_Static_assert(sizeof(Cell) == 16, "Cell layout grew; recheck large-world memory");

#define MAX_WORLD_REACTIONS 64

typedef struct WorldReactionEvent {
    Vector2 position;
} WorldReactionEvent;

typedef struct LaserResult {
    Vector2 position;
    CellMaterial material;
    bool hit;
} LaserResult;

/* Work performed by the most recent fixed simulation tick. These counters are
   deliberately structural rather than time-based: they stay meaningful across
   machines and make performance regressions testable without flaky deadlines. */
typedef struct WorldTickStats {
    uint64_t processedCells;
    uint32_t processedChunks;
} WorldTickStats;

typedef struct World {
    int width;
    int height;
    Cell *cells;
    Color *pixels;
    Texture2D texture;
    uint32_t tick;
    uint32_t effectSerial;
    WorldTickStats lastTickStats;
    WorldReactionEvent reactions[MAX_WORLD_REACTIONS];
    int reactionCount;
    int chunkColumns;
    int chunkRows;
    int activeChunkCount;
    uint8_t *activeChunks;
    uint8_t *nextActiveChunks;
    /* Chunks whose pixels changed since the last upload. The simulation already
       tracks where work happens; the renderer reuses that instead of rebuilding
       the whole texture every frame. */
    uint8_t *dirtyChunks;
    /* Chunks whose light inputs are stale. Separate from `dirtyChunks` because
       the two are consumed at different times: light must be refreshed once,
       wherever the terrain changed, while a pixel rebuild waits until the chunk
       is on screen and so may stay pending for many frames. Sharing one flag
       makes the light refresh re-scan every off-screen chunk every frame. */
    uint8_t *lightDirtyChunks;
    /* Coarse light field. `emission` and `opacity` are derived from the cells and
       refreshed only where chunks are dirty; `light` is solved from them every
       draw; `lightShown` is the copy the current texture was built from, so a
       chunk can be re-lit without anything in it having changed. */
    int lightColumns;
    int lightRows;
    /* Two channels, not one. A single intensity can darken but cannot colour,
       so a lava lake lit its own cavern in grey. `lightSky` is daylight reaching
       down from the surface, `lightEmber` is everything that burns, and the
       difference between them is what warms the light near a fire. */
    float *lightSky;
    float *lightEmber;
    float *lightShownSky;
    float *lightShownEmber;
    float *lightEmission;
    float *lightOpacity;
    /* One movable light the caller owns, so the player can carry their own glow
       into a tunnel that has no other source. */
    Vector2 pointLight;
    float pointLightRadius;
    float pointLightStrength;
    /* State of the light the last solve was run for, so a still scene can skip
       the solve entirely. */
    Vector2 solvedPointLight;
    float solvedPointLightStrength;
    uint32_t solvedTick;
    bool lightSolved;
} World;

bool WorldInit(World *world, int width, int height);
/* Creates the GPU texture. Separate from WorldInit so the simulation can run
   headlessly in tests, where no window or GL context exists. */
bool WorldInitRenderer(World *world);
void WorldUnload(World *world);
void WorldGenerate(World *world);
Vector2 WorldPlayerSpawn(const World *world);
/* Wakes generated dynamic or heated cells inside a streamed gameplay region.
   Actual cell mutations wake themselves regardless of this region. */
void WorldActivateRegion(World *world, Rectangle region);
void WorldUpdate(World *world);
void WorldDraw(World *world, Rectangle visible);
/* Position of the caller-owned light, applied on the next draw. A strength of
   zero disables it. */
void WorldSetPointLight(World *world, Vector2 position, float radius, float strength);
float WorldLightAt(const World *world, int x, int y);

CellMaterial WorldGetCell(const World *world, int x, int y);
int WorldCountDynamicCells(const World *world);
float WorldGetTemperature(const World *world, int x, int y);
void WorldSetTemperature(World *world, int x, int y, float temperature);
bool WorldMaterialIsSolid(CellMaterial material);
void WorldSetCell(World *world, int x, int y, CellMaterial material);
void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
int WorldDrillCircle(World *world, int centerX, int centerY, int radius);
void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius);
/* One heavy blow along a cone: throws dynamic cells a long way and scours a thin
   layer off the exposed face of anything solid. `spreadCosine` is the cosine of
   the cone's half angle; `reach` is how far the nearest cells are thrown. */
void WorldApplyForceBlast(World *world, Vector2 origin, Vector2 direction,
                          float length, float spreadCosine, int reach);
LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
/* Thermal inverse of the laser: chills everything along the ray, freezing water
   to ice and settling lava back into rock. */
LaserResult WorldApplyChill(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
const char *WorldMaterialName(CellMaterial material);

#endif
