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
    MATERIAL_LAVA
} CellMaterial;

typedef struct Cell {
    CellMaterial material;
    uint32_t updatedTick;
    uint32_t effectStamp;
    uint8_t rockDamage;
} Cell;

typedef struct World {
    int width;
    int height;
    Cell *cells;
    Color *pixels;
    Texture2D texture;
    uint32_t tick;
    uint32_t effectSerial;
    int activeCells;
} World;

bool WorldInit(World *world, int width, int height);
void WorldUnload(World *world);
void WorldGenerate(World *world);
void WorldUpdate(World *world);
void WorldDraw(World *world);

CellMaterial WorldGetCell(const World *world, int x, int y);
void WorldSetCell(World *world, int x, int y, CellMaterial material);
void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance);
Vector2 WorldScreenToCell(const World *world, Vector2 screenPosition, Camera2D camera);

void WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius, float deltaTime);
const char *WorldMaterialName(CellMaterial material);

#endif
