#include "world.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <raymath.h>

static bool WorldInBounds(const World *world, int x, int y)
{
    return x >= 0 && x < world->width && y >= 0 && y < world->height;
}

static size_t WorldIndex(const World *world, int x, int y)
{
    return (size_t)y * (size_t)world->width + (size_t)x;
}

static Cell *WorldCell(World *world, int x, int y)
{
    return &world->cells[WorldIndex(world, x, y)];
}

static const Cell *WorldCellConst(const World *world, int x, int y)
{
    return &world->cells[WorldIndex(world, x, y)];
}

static bool MaterialIsDynamic(CellMaterial material)
{
    return material == MATERIAL_SAND || material == MATERIAL_WATER ||
           material == MATERIAL_LAVA;
}

static void WorldSetCellRaw(World *world, int x, int y, CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    cell->material = material;
    cell->rockDamage = 0;
    cell->effectStamp = 0;
}

static void WorldFillEllipse(World *world, int centerX, int centerY,
                             int radiusX, int radiusY, CellMaterial material)
{
    int x;
    int y;

    for (y = centerY - radiusY; y <= centerY + radiusY; ++y) {
        for (x = centerX - radiusX; x <= centerX + radiusX; ++x) {
            float dx = (float)(x - centerX) / (float)radiusX;
            float dy = (float)(y - centerY) / (float)radiusY;

            if (dx * dx + dy * dy <= 1.0f) {
                WorldSetCellRaw(world, x, y, material);
            }
        }
    }
}

static uint32_t CoordinateHash(int x, int y)
{
    uint32_t value = (uint32_t)x * 0x45d9f3bu;

    value ^= (uint32_t)y * 0x27d4eb2du;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return value;
}

static Color MaterialColor(CellMaterial material, int x, int y)
{
    int variation = (int)(CoordinateHash(x, y) % 13u) - 6;

    switch (material) {
        case MATERIAL_DIRT:
            return (Color){(unsigned char)(111 + variation),
                           (unsigned char)(73 + variation / 2), 43, 255};
        case MATERIAL_ROCK:
            return (Color){(unsigned char)(72 + variation),
                           (unsigned char)(77 + variation),
                           (unsigned char)(86 + variation), 255};
        case MATERIAL_SAND:
            return (Color){(unsigned char)(218 + variation),
                           (unsigned char)(184 + variation), 91, 255};
        case MATERIAL_WATER:
            return (Color){32, (unsigned char)(111 + variation),
                           (unsigned char)(190 + variation), 225};
        case MATERIAL_LAVA:
            return (Color){245, (unsigned char)(73 + variation * 2), 18, 255};
        case MATERIAL_EMPTY:
        default: {
            unsigned char glow = (unsigned char)(10 + (y * 10) / 288);
            return (Color){5, glow, (unsigned char)(18 + glow), 255};
        }
    }
}

static void WorldMoveCell(World *world, int fromX, int fromY, int toX, int toY)
{
    Cell *from = WorldCell(world, fromX, fromY);
    Cell *to = WorldCell(world, toX, toY);
    Cell moving = *from;

    *from = *to;
    *to = moving;
    to->updatedTick = world->tick;
    from->updatedTick = world->tick;
}

static bool WorldTryMoveInto(World *world, int x, int y, int targetX, int targetY,
                             bool allowWaterSwap)
{
    CellMaterial target;

    if (!WorldInBounds(world, targetX, targetY)) {
        return false;
    }

    target = WorldGetCell(world, targetX, targetY);
    if (target == MATERIAL_EMPTY || (allowWaterSwap && target == MATERIAL_WATER)) {
        WorldMoveCell(world, x, y, targetX, targetY);
        return true;
    }

    return false;
}

static void WorldUpdateSand(World *world, int x, int y, int direction)
{
    if (WorldTryMoveInto(world, x, y, x, y + 1, true)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y + 1, true)) {
        return;
    }
    (void)WorldTryMoveInto(world, x, y, x - direction, y + 1, true);
}

static void WorldUpdateLiquid(World *world, int x, int y, int direction, bool viscous)
{
    if (viscous && ((world->tick + (uint32_t)x + (uint32_t)y) % 3u != 0u)) {
        return;
    }

    if (WorldTryMoveInto(world, x, y, x, y + 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y + 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x - direction, y + 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y, false)) {
        return;
    }
    (void)WorldTryMoveInto(world, x, y, x - direction, y, false);
}

static void WorldBurnDirt(World *world, int x, int y)
{
    static const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    int i;

    if ((world->tick + CoordinateHash(x, y)) % 9u != 0u) {
        return;
    }

    for (i = 0; i < 4; ++i) {
        int targetX = x + offsets[i][0];
        int targetY = y + offsets[i][1];

        if (WorldInBounds(world, targetX, targetY) &&
            WorldGetCell(world, targetX, targetY) == MATERIAL_DIRT) {
            WorldSetCellRaw(world, targetX, targetY, MATERIAL_EMPTY);
            break;
        }
    }
}

static void WorldRecordReaction(World *world, int x1, int y1, int x2, int y2)
{
    WorldReactionEvent *event;

    if (world->reactionCount >= MAX_WORLD_REACTIONS) {
        return;
    }

    event = &world->reactions[world->reactionCount++];
    event->position = (Vector2){((float)x1 + (float)x2 + 1.0f) * 0.5f,
                                ((float)y1 + (float)y2 + 1.0f) * 0.5f};
}

static bool WorldTryMaterialReaction(World *world, int x, int y)
{
    static const int offsets[8][2] = {
        {0, 1}, {1, 0}, {0, -1}, {-1, 0},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1}
    };
    CellMaterial material = WorldGetCell(world, x, y);
    CellMaterial targetMaterial;
    int firstOffset;
    int i;

    if (material != MATERIAL_WATER && material != MATERIAL_LAVA) {
        return false;
    }
    targetMaterial = material == MATERIAL_WATER ? MATERIAL_LAVA : MATERIAL_WATER;
    firstOffset = (int)(CoordinateHash(x, y) % 8u);

    for (i = 0; i < 8; ++i) {
        int offsetIndex = (firstOffset + i) % 8;
        int targetX = x + offsets[offsetIndex][0];
        int targetY = y + offsets[offsetIndex][1];
        int waterX;
        int waterY;
        int lavaX;
        int lavaY;

        if (!WorldInBounds(world, targetX, targetY) ||
            WorldGetCell(world, targetX, targetY) != targetMaterial) {
            continue;
        }

        if (material == MATERIAL_WATER) {
            waterX = x;
            waterY = y;
            lavaX = targetX;
            lavaY = targetY;
        } else {
            waterX = targetX;
            waterY = targetY;
            lavaX = x;
            lavaY = y;
        }

        WorldSetCellRaw(world, lavaX, lavaY, MATERIAL_ROCK);
        WorldSetCellRaw(world, waterX, waterY, MATERIAL_EMPTY);
        WorldCell(world, lavaX, lavaY)->updatedTick = world->tick;
        WorldCell(world, waterX, waterY)->updatedTick = world->tick;
        WorldRecordReaction(world, waterX, waterY, lavaX, lavaY);
        return true;
    }

    return false;
}

bool WorldInit(World *world, int width, int height)
{
    Image image;
    size_t cellCount;

    if (world == NULL || width <= 0 || height <= 0) {
        return false;
    }

    memset(world, 0, sizeof(*world));
    world->width = width;
    world->height = height;
    cellCount = (size_t)width * (size_t)height;
    world->cells = calloc(cellCount, sizeof(*world->cells));
    world->pixels = malloc(cellCount * sizeof(*world->pixels));

    if (world->cells == NULL || world->pixels == NULL) {
        free(world->cells);
        free(world->pixels);
        memset(world, 0, sizeof(*world));
        return false;
    }

    image = GenImageColor(width, height, BLACK);
    world->texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (world->texture.id == 0u) {
        free(world->cells);
        free(world->pixels);
        memset(world, 0, sizeof(*world));
        return false;
    }

    SetTextureFilter(world->texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(world->texture, TEXTURE_WRAP_CLAMP);
    return true;
}

void WorldUnload(World *world)
{
    if (world == NULL) {
        return;
    }

    if (world->texture.id != 0u) {
        UnloadTexture(world->texture);
    }
    free(world->cells);
    free(world->pixels);
    memset(world, 0, sizeof(*world));
}

void WorldGenerate(World *world)
{
    int surface[512];
    int x;
    int y;
    int cave;

    if (world == NULL || world->cells == NULL || world->width > 512) {
        return;
    }

    memset(world->cells, 0,
           (size_t)world->width * (size_t)world->height * sizeof(*world->cells));
    world->tick = 0;
    world->effectSerial = 0;

    for (x = 0; x < world->width; ++x) {
        float rolling = sinf((float)x * 0.035f) * 9.0f +
                        sinf((float)x * 0.011f + 1.7f) * 13.0f;
        surface[x] = 103 + (int)rolling;

        for (y = surface[x]; y < world->height; ++y) {
            int depth = y - surface[x];
            CellMaterial material = depth > 58 + (int)(CoordinateHash(x, y) % 17u)
                                    ? MATERIAL_ROCK
                                    : MATERIAL_DIRT;
            WorldSetCellRaw(world, x, y, material);
        }
    }

    for (cave = 0; cave < 31; ++cave) {
        int centerX = GetRandomValue(20, world->width - 21);
        int centerY = GetRandomValue(139, world->height - 25);
        int radiusX = GetRandomValue(8, 28);
        int radiusY = GetRandomValue(5, 15);

        WorldFillEllipse(world, centerX, centerY, radiusX, radiusY, MATERIAL_EMPTY);
    }

    /* A loose sand bank which immediately demonstrates falling-cell physics. */
    for (x = 132; x < 202 && x < world->width; ++x) {
        int sandTop = surface[x] - 1 - (int)(8.0f * sinf((float)(x - 132) / 70.0f * PI));
        for (y = sandTop; y < surface[x] + 18 && y < world->height; ++y) {
            WorldSetCellRaw(world, x, y, MATERIAL_SAND);
        }
    }

    /* Enclosed water cavern. */
    WorldFillEllipse(world, 82, 176, 34, 22, MATERIAL_ROCK);
    WorldFillEllipse(world, 82, 173, 30, 18, MATERIAL_EMPTY);
    for (y = 171; y <= 188; ++y) {
        for (x = 52; x <= 112; ++x) {
            float dx = (float)(x - 82) / 30.0f;
            float dy = (float)(y - 173) / 18.0f;
            if (dx * dx + dy * dy <= 1.0f) {
                WorldSetCellRaw(world, x, y, MATERIAL_WATER);
            }
        }
    }

    /* A rock-lined lava pocket near the deep right side. */
    WorldFillEllipse(world, 421, 235, 31, 18, MATERIAL_ROCK);
    WorldFillEllipse(world, 421, 232, 27, 14, MATERIAL_EMPTY);
    for (y = 231; y <= 245; ++y) {
        for (x = 394; x <= 448; ++x) {
            float dx = (float)(x - 421) / 27.0f;
            float dy = (float)(y - 232) / 14.0f;
            if (dx * dx + dy * dy <= 1.0f) {
                WorldSetCellRaw(world, x, y, MATERIAL_LAVA);
            }
        }
    }

    /* Breakable dirt columns make good first laser targets. */
    for (x = 274; x < 284; ++x) {
        for (y = 74; y < 151; ++y) {
            WorldSetCellRaw(world, x, y, MATERIAL_DIRT);
        }
    }
}

void WorldUpdate(World *world)
{
    int y;
    size_t index;
    size_t cellCount;

    if (world == NULL || world->cells == NULL) {
        return;
    }

    world->reactionCount = 0;
    ++world->tick;
    if (world->tick == 0u) {
        world->tick = 1u;
    }

    for (y = world->height - 2; y >= 0; --y) {
        bool reverse = ((world->tick + (uint32_t)y) & 1u) != 0u;
        int start = reverse ? world->width - 1 : 0;
        int end = reverse ? -1 : world->width;
        int step = reverse ? -1 : 1;
        int x;

        for (x = start; x != end; x += step) {
            Cell *cell = WorldCell(world, x, y);
            int direction = ((CoordinateHash(x, y) + world->tick) & 1u) != 0u ? 1 : -1;

            if (cell->updatedTick == world->tick) {
                continue;
            }

            switch (cell->material) {
                case MATERIAL_SAND:
                    WorldUpdateSand(world, x, y, direction);
                    break;
                case MATERIAL_WATER:
                    if (!WorldTryMaterialReaction(world, x, y)) {
                        WorldUpdateLiquid(world, x, y, direction, false);
                    }
                    break;
                case MATERIAL_LAVA:
                    if (!WorldTryMaterialReaction(world, x, y)) {
                        WorldBurnDirt(world, x, y);
                        WorldUpdateLiquid(world, x, y, direction, true);
                    }
                    break;
                default:
                    break;
            }
        }
    }

    world->activeCells = 0;
    cellCount = (size_t)world->width * (size_t)world->height;
    for (index = 0; index < cellCount; ++index) {
        if (MaterialIsDynamic(world->cells[index].material)) {
            ++world->activeCells;
        }
    }
}

void WorldDraw(World *world)
{
    int x;
    int y;

    if (world == NULL || world->pixels == NULL || world->texture.id == 0u) {
        return;
    }

    for (y = 0; y < world->height; ++y) {
        for (x = 0; x < world->width; ++x) {
            world->pixels[WorldIndex(world, x, y)] =
                MaterialColor(WorldGetCell(world, x, y), x, y);
        }
    }

    UpdateTexture(world->texture, world->pixels);
    DrawTexture(world->texture, 0, 0, WHITE);
}

CellMaterial WorldGetCell(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return MATERIAL_ROCK;
    }
    return WorldCellConst(world, x, y)->material;
}

void WorldSetCell(World *world, int x, int y, CellMaterial material)
{
    if (world == NULL || world->cells == NULL) {
        return;
    }
    WorldSetCellRaw(world, x, y, material);
}

void WorldDestroyCircle(World *world, int centerX, int centerY, int radius,
                        float rockToLavaChance)
{
    int x;
    int y;
    int radiusSquared = radius * radius;
    int chance = (int)(Clamp(rockToLavaChance, 0.0f, 1.0f) * 1000.0f);

    if (world == NULL || world->cells == NULL) {
        return;
    }

    for (y = centerY - radius; y <= centerY + radius; ++y) {
        for (x = centerX - radius; x <= centerX + radius; ++x) {
            int dx = x - centerX;
            int dy = y - centerY;
            CellMaterial material;

            if (dx * dx + dy * dy > radiusSquared || !WorldInBounds(world, x, y)) {
                continue;
            }

            material = WorldGetCell(world, x, y);
            if (material == MATERIAL_ROCK && GetRandomValue(0, 999) < chance) {
                WorldSetCellRaw(world, x, y, MATERIAL_LAVA);
            } else {
                WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            }
        }
    }
}

Vector2 WorldScreenToCell(const World *world, Vector2 screenPosition, Camera2D camera)
{
    Vector2 point = GetScreenToWorld2D(screenPosition, camera);

    if (world == NULL) {
        return (Vector2){0.0f, 0.0f};
    }

    point.x = Clamp(floorf(point.x), 0.0f, (float)(world->width - 1));
    point.y = Clamp(floorf(point.y), 0.0f, (float)(world->height - 1));
    return point;
}

void WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius, float deltaTime)
{
    Vector2 delta = {end.x - start.x, end.y - start.y};
    float length = sqrtf(delta.x * delta.x + delta.y * delta.y);
    int steps = (int)ceilf(length / 1.25f);
    int step;
    uint32_t stamp;

    if (world == NULL || world->cells == NULL || length < 0.001f) {
        return;
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        int centerX = (int)floorf(start.x + delta.x * amount);
        int centerY = (int)floorf(start.y + delta.y * amount);
        int brush = (int)ceilf(radius);
        int x;
        int y;

        for (y = centerY - brush; y <= centerY + brush; ++y) {
            for (x = centerX - brush; x <= centerX + brush; ++x) {
                int dx = x - centerX;
                int dy = y - centerY;
                Cell *cell;

                if ((float)(dx * dx + dy * dy) > radius * radius ||
                    !WorldInBounds(world, x, y)) {
                    continue;
                }

                cell = WorldCell(world, x, y);
                if (cell->effectStamp == stamp) {
                    continue;
                }
                cell->effectStamp = stamp;

                if (cell->material == MATERIAL_DIRT || cell->material == MATERIAL_SAND) {
                    WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
                } else if (cell->material == MATERIAL_ROCK) {
                    int damage = (int)ceilf(deltaTime * 70.0f);
                    int total = (int)cell->rockDamage + damage;

                    cell->rockDamage = (uint8_t)(total > 255 ? 255 : total);
                    if (cell->rockDamage >= 42u) {
                        WorldSetCellRaw(world, x, y, MATERIAL_LAVA);
                    }
                }
            }
        }
    }
}

const char *WorldMaterialName(CellMaterial material)
{
    switch (material) {
        case MATERIAL_DIRT: return "DIRT";
        case MATERIAL_ROCK: return "ROCK";
        case MATERIAL_SAND: return "SAND";
        case MATERIAL_WATER: return "WATER";
        case MATERIAL_LAVA: return "LAVA";
        case MATERIAL_EMPTY:
        default: return "EMPTY";
    }
}
