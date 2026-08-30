#ifndef WORLD_H
#define WORLD_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

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
    MATERIAL_ASH
} CellMaterial;

typedef struct Cell {
    CellMaterial material;
    uint32_t updatedTick;
    uint32_t effectStamp;
    float temperature;
    uint16_t lifetime;
} Cell;

#define MAX_WORLD_REACTIONS 64

typedef struct WorldReactionEvent {
    Vector2 position;
} WorldReactionEvent;

typedef struct LaserResult {
    Vector2 position;
    CellMaterial material;
    bool hit;
} LaserResult;

typedef struct World {
    int width;
    int height;
    Cell *cells;
    Color *pixels;
    Texture2D texture;
    uint32_t tick;
    uint32_t effectSerial;
    int activeCells;
    WorldReactionEvent reactions[MAX_WORLD_REACTIONS];
    int reactionCount;
} World;

bool WorldInit(World *world, int width, int height);
void WorldUnload(World *world);
void WorldGenerate(World *world);
void WorldUpdate(World *world);
void WorldDraw(World *world);

CellMaterial WorldGetCell(const World *world, int x, int y);
float WorldGetTemperature(const World *world, int x, int y);
bool WorldMaterialIsSolid(CellMaterial material);
void WorldSetCell(World *world, int x, int y, CellMaterial material);
void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius);
Vector2 WorldScreenToCell(const World *world, Vector2 screenPosition, Camera2D camera);

LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime);
const char *WorldMaterialName(CellMaterial material);

#endif
