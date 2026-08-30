#include "world.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <raymath.h>

static void WorldCountActiveState(World *world);

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

static void WorldWakeCellAndNeighbors(World *world, int x, int y)
{
    int centerChunkX;
    int centerChunkY;
    int chunkY;

    if (world->activeChunks == NULL || world->nextActiveChunks == NULL ||
        !WorldInBounds(world, x, y)) {
        return;
    }

    centerChunkX = x / WORLD_CHUNK_SIZE;
    centerChunkY = y / WORLD_CHUNK_SIZE;
    for (chunkY = centerChunkY - 1; chunkY <= centerChunkY + 1; ++chunkY) {
        int chunkX;

        for (chunkX = centerChunkX - 1; chunkX <= centerChunkX + 1; ++chunkX) {
            size_t index;

            if (chunkX < 0 || chunkX >= world->chunkColumns ||
                chunkY < 0 || chunkY >= world->chunkRows) {
                continue;
            }
            index = (size_t)chunkY * (size_t)world->chunkColumns + (size_t)chunkX;
            world->activeChunks[index] = 1u;
            world->nextActiveChunks[index] = 1u;
        }
    }
}

static bool MaterialIsDynamic(CellMaterial material)
{
    return material == MATERIAL_SAND || material == MATERIAL_WATER ||
           material == MATERIAL_LAVA || material == MATERIAL_STEAM ||
           material == MATERIAL_SMOKE || material == MATERIAL_FIRE ||
           material == MATERIAL_ASH;
}

static float MaterialInitialTemperature(CellMaterial material)
{
    switch (material) {
        case MATERIAL_LAVA: return 900.0f;
        case MATERIAL_FIRE: return 650.0f;
        case MATERIAL_STEAM: return 125.0f;
        case MATERIAL_SMOKE: return 75.0f;
        default: return 20.0f;
    }
}

static void WorldSetCellRaw(World *world, int x, int y, CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    cell->material = material;
    cell->temperature = MaterialInitialTemperature(material);
    cell->lifetime = 0;
    cell->effectStamp = 0;
    WorldWakeCellAndNeighbors(world, x, y);
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
        case MATERIAL_STEAM:
            return (Color){(unsigned char)(204 + variation),
                           (unsigned char)(222 + variation), 229, 178};
        case MATERIAL_SMOKE:
            return (Color){(unsigned char)(83 + variation),
                           (unsigned char)(88 + variation),
                           (unsigned char)(94 + variation), 205};
        case MATERIAL_FIRE:
            return (Color){255, (unsigned char)(132 + variation * 3), 24, 245};
        case MATERIAL_ASH:
            return (Color){(unsigned char)(112 + variation),
                           (unsigned char)(108 + variation),
                           (unsigned char)(104 + variation), 255};
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
    WorldWakeCellAndNeighbors(world, fromX, fromY);
    WorldWakeCellAndNeighbors(world, toX, toY);
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

static void WorldUpdateGasMotion(World *world, int x, int y, int direction, bool slow)
{
    if (slow && ((world->tick + (uint32_t)x + (uint32_t)y) & 1u) != 0u) {
        return;
    }

    if (WorldTryMoveInto(world, x, y, x, y - 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y - 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x - direction, y - 1, false)) {
        return;
    }
    if (WorldTryMoveInto(world, x, y, x + direction, y, false)) {
        return;
    }
    (void)WorldTryMoveInto(world, x, y, x - direction, y, false);
}

static void WorldHeatNeighbors(World *world, int x, int y, float heat)
{
    static const int offsets[8][2] = {
        {0, 1}, {1, 0}, {0, -1}, {-1, 0},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1}
    };
    int i;

    for (i = 0; i < 8; ++i) {
        int targetX = x + offsets[i][0];
        int targetY = y + offsets[i][1];

        if (WorldInBounds(world, targetX, targetY) &&
            WorldGetCell(world, targetX, targetY) != MATERIAL_EMPTY) {
            WorldCell(world, targetX, targetY)->temperature += heat;
            WorldWakeCellAndNeighbors(world, targetX, targetY);
        }
    }
}

static bool WorldTryThermalTransition(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);
    CellMaterial next = cell->material;

    switch (cell->material) {
        case MATERIAL_DIRT:
            if (cell->temperature >= 175.0f) next = MATERIAL_FIRE;
            break;
        case MATERIAL_SAND:
            if (cell->temperature >= 280.0f) next = MATERIAL_EMPTY;
            break;
        case MATERIAL_ROCK:
            if (cell->temperature >= 720.0f) next = MATERIAL_LAVA;
            break;
        case MATERIAL_WATER:
            if (cell->temperature >= 108.0f) next = MATERIAL_STEAM;
            break;
        case MATERIAL_STEAM:
            if (cell->temperature <= 58.0f) next = MATERIAL_WATER;
            break;
        default:
            break;
    }

    if (next == cell->material) {
        return false;
    }

    WorldSetCellRaw(world, x, y, next);
    WorldCell(world, x, y)->updatedTick = world->tick;
    return true;
}

static bool WorldUpdateTemperatureState(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);

    switch (cell->material) {
        case MATERIAL_EMPTY:
            return false;
        case MATERIAL_LAVA:
            cell->temperature += (900.0f - cell->temperature) * 0.08f;
            break;
        case MATERIAL_FIRE:
            cell->temperature += (650.0f - cell->temperature) * 0.12f;
            break;
        case MATERIAL_STEAM:
            cell->temperature -= 0.42f;
            break;
        default:
            cell->temperature += (20.0f - cell->temperature) * 0.006f;
            break;
    }

    return WorldTryThermalTransition(world, x, y);
}

static void WorldUpdateGas(World *world, int x, int y, int direction, bool smoke)
{
    Cell *cell = WorldCell(world, x, y);
    uint16_t maximumLife = smoke
                               ? (uint16_t)(150u + CoordinateHash(x, y) % 100u)
                               : 420u;

    if (cell->lifetime < UINT16_MAX) {
        ++cell->lifetime;
    }
    if (cell->lifetime >= maximumLife) {
        WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
        WorldCell(world, x, y)->updatedTick = world->tick;
        return;
    }

    WorldUpdateGasMotion(world, x, y, direction, smoke);
}

static void WorldUpdateFire(World *world, int x, int y, int direction)
{
    Cell *cell = WorldCell(world, x, y);
    uint16_t maximumLife = (uint16_t)(42u + CoordinateHash(x, y) % 48u);

    if (cell->lifetime < UINT16_MAX) {
        ++cell->lifetime;
    }
    WorldHeatNeighbors(world, x, y, 11.0f);

    if (cell->lifetime % 12u == 0u && WorldGetCell(world, x, y - 1) == MATERIAL_EMPTY) {
        WorldSetCellRaw(world, x, y - 1, MATERIAL_SMOKE);
        WorldCell(world, x, y - 1)->updatedTick = world->tick;
    }

    if (cell->lifetime >= maximumLife) {
        CellMaterial residue = (CoordinateHash(x, y) + world->tick) % 4u == 0u
                                   ? MATERIAL_ASH
                                   : MATERIAL_SMOKE;
        WorldSetCellRaw(world, x, y, residue);
        WorldCell(world, x, y)->updatedTick = world->tick;
        return;
    }

    if (cell->lifetime > 8u) {
        WorldUpdateGasMotion(world, x, y, direction, true);
    }
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
            WorldSetCellRaw(world, targetX, targetY, MATERIAL_FIRE);
            WorldCell(world, targetX, targetY)->updatedTick = world->tick;
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
        WorldSetCellRaw(world, waterX, waterY, MATERIAL_STEAM);
        WorldCell(world, lavaX, lavaY)->temperature = 185.0f;
        WorldCell(world, waterX, waterY)->temperature = 125.0f;
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
    size_t chunkCount;

    if (world == NULL || width <= 0 || height <= 0) {
        return false;
    }

    memset(world, 0, sizeof(*world));
    world->width = width;
    world->height = height;
    world->chunkColumns = (width + WORLD_CHUNK_SIZE - 1) / WORLD_CHUNK_SIZE;
    world->chunkRows = (height + WORLD_CHUNK_SIZE - 1) / WORLD_CHUNK_SIZE;
    cellCount = (size_t)width * (size_t)height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    world->cells = calloc(cellCount, sizeof(*world->cells));
    world->pixels = malloc(cellCount * sizeof(*world->pixels));
    world->activeChunks = calloc(chunkCount, sizeof(*world->activeChunks));
    world->nextActiveChunks = calloc(chunkCount, sizeof(*world->nextActiveChunks));

    if (world->cells == NULL || world->pixels == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL) {
        free(world->cells);
        free(world->pixels);
        free(world->activeChunks);
        free(world->nextActiveChunks);
        memset(world, 0, sizeof(*world));
        return false;
    }

    image = GenImageColor(width, height, BLACK);
    world->texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (world->texture.id == 0u) {
        free(world->cells);
        free(world->pixels);
        free(world->activeChunks);
        free(world->nextActiveChunks);
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
    free(world->activeChunks);
    free(world->nextActiveChunks);
    memset(world, 0, sizeof(*world));
}

void WorldGenerate(World *world)
{
    int surface[512];
    int x;
    int y;
    int cave;
    size_t cellIndex;
    size_t cellCount;
    size_t chunkCount;

    if (world == NULL || world->cells == NULL || world->width > 512) {
        return;
    }

    cellCount = (size_t)world->width * (size_t)world->height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    memset(world->cells, 0, cellCount * sizeof(*world->cells));
    memset(world->activeChunks, 0, chunkCount * sizeof(*world->activeChunks));
    memset(world->nextActiveChunks, 0, chunkCount * sizeof(*world->nextActiveChunks));
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        world->cells[cellIndex].temperature = 20.0f;
    }
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
    WorldCountActiveState(world);
}

static void WorldUpdateCellAt(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);
    int direction = ((CoordinateHash(x, y) + world->tick) & 1u) != 0u ? 1 : -1;

    if (cell->updatedTick == world->tick) {
        return;
    }
    if (MaterialIsDynamic(cell->material) || fabsf(cell->temperature - 20.0f) > 0.5f) {
        WorldWakeCellAndNeighbors(world, x, y);
    }
    if (WorldUpdateTemperatureState(world, x, y)) {
        return;
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
                WorldHeatNeighbors(world, x, y, 7.0f);
                WorldBurnDirt(world, x, y);
                WorldUpdateLiquid(world, x, y, direction, true);
            }
            break;
        case MATERIAL_STEAM:
            WorldUpdateGas(world, x, y, direction, false);
            break;
        case MATERIAL_SMOKE:
            WorldUpdateGas(world, x, y, direction, true);
            break;
        case MATERIAL_FIRE:
            WorldUpdateFire(world, x, y, direction);
            break;
        case MATERIAL_ASH:
            WorldUpdateSand(world, x, y, direction);
            break;
        default:
            break;
    }
}

static void WorldCountActiveState(World *world)
{
    int chunkY;

    world->activeCells = 0;
    world->activeChunkCount = 0;
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int y;

            if (world->activeChunks[chunkIndex] == 0u) {
                continue;
            }
            ++world->activeChunkCount;
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;

            for (y = minimumY; y < maximumY; ++y) {
                int x;

                for (x = minimumX; x < maximumX; ++x) {
                    if (MaterialIsDynamic(WorldGetCell(world, x, y))) {
                        ++world->activeCells;
                    }
                }
            }
        }
    }
}

void WorldUpdate(World *world)
{
    size_t chunkCount;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL) {
        return;
    }

    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    memset(world->nextActiveChunks, 0, chunkCount * sizeof(*world->nextActiveChunks));
    world->reactionCount = 0;
    ++world->tick;
    if (world->tick == 0u) {
        world->tick = 1u;
    }

    for (chunkY = world->chunkRows - 1; chunkY >= 0; --chunkY) {
        int minimumY = chunkY * WORLD_CHUNK_SIZE;
        int maximumY = minimumY + WORLD_CHUNK_SIZE - 1;
        int y;

        if (maximumY >= world->height) maximumY = world->height - 1;
        for (y = maximumY; y >= minimumY; --y) {
            bool reverse = ((world->tick + (uint32_t)y) & 1u) != 0u;
            int chunkStart = reverse ? world->chunkColumns - 1 : 0;
            int chunkEnd = reverse ? -1 : world->chunkColumns;
            int chunkStep = reverse ? -1 : 1;
            int chunkX;

            for (chunkX = chunkStart; chunkX != chunkEnd; chunkX += chunkStep) {
                size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                    (size_t)chunkX;
                int minimumX;
                int maximumX;
                int start;
                int end;
                int step;
                int x;

                if (world->activeChunks[chunkIndex] == 0u) {
                    continue;
                }
                minimumX = chunkX * WORLD_CHUNK_SIZE;
                maximumX = minimumX + WORLD_CHUNK_SIZE;
                if (maximumX > world->width) maximumX = world->width;
                start = reverse ? maximumX - 1 : minimumX;
                end = reverse ? minimumX - 1 : maximumX;
                step = reverse ? -1 : 1;

                for (x = start; x != end; x += step) {
                    WorldUpdateCellAt(world, x, y);
                }
            }
        }
    }

    {
        uint8_t *previousChunks = world->activeChunks;
        world->activeChunks = world->nextActiveChunks;
        world->nextActiveChunks = previousChunks;
    }
    WorldCountActiveState(world);
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

float WorldGetTemperature(const World *world, int x, int y)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return 20.0f;
    }
    return WorldCellConst(world, x, y)->temperature;
}

bool WorldMaterialIsSolid(CellMaterial material)
{
    return material == MATERIAL_DIRT || material == MATERIAL_ROCK ||
           material == MATERIAL_SAND;
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

void WorldApplyShockwave(World *world, int centerX, int centerY, int innerRadius,
                         int outerRadius)
{
    uint32_t stamp;
    int band;

    if (world == NULL || world->cells == NULL || innerRadius < 0 ||
        outerRadius <= innerRadius) {
        return;
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    /* Outer bands move first, so displaced cells cannot be pushed twice. */
    for (band = outerRadius; band > innerRadius; --band) {
        int y;

        for (y = centerY - band; y <= centerY + band; ++y) {
            int x;

            for (x = centerX - band; x <= centerX + band; ++x) {
                float dx = (float)(x - centerX);
                float dy = (float)(y - centerY);
                float distanceSquared = dx * dx + dy * dy;
                float distance;
                float directionX;
                float directionY;
                float strength;
                int pushDistance;
                int push;
                Cell *cell;

                if (!WorldInBounds(world, x, y) ||
                    distanceSquared > (float)(band * band) ||
                    distanceSquared <= (float)((band - 1) * (band - 1))) {
                    continue;
                }

                cell = WorldCell(world, x, y);
                if (!MaterialIsDynamic(cell->material) || cell->effectStamp == stamp) {
                    continue;
                }

                distance = sqrtf(distanceSquared);
                directionX = dx / distance;
                directionY = dy / distance;
                strength = 1.0f - (distance - (float)innerRadius) /
                                      (float)(outerRadius - innerRadius);
                pushDistance = 2 + (int)(Clamp(strength, 0.0f, 1.0f) * 10.0f);
                cell->effectStamp = stamp;

                for (push = pushDistance; push >= 1; --push) {
                    int targetX = (int)roundf((float)x + directionX * (float)push);
                    int targetY = (int)roundf((float)y + directionY * (float)push);

                    if ((targetX != x || targetY != y) &&
                        WorldInBounds(world, targetX, targetY) &&
                        WorldGetCell(world, targetX, targetY) == MATERIAL_EMPTY) {
                        WorldMoveCell(world, x, y, targetX, targetY);
                        break;
                    }
                }
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

LaserResult WorldApplyLaser(World *world, Vector2 start, Vector2 end, float radius,
                            float deltaTime)
{
    Vector2 delta = {end.x - start.x, end.y - start.y};
    float length = sqrtf(delta.x * delta.x + delta.y * delta.y);
    int steps = (int)ceilf(length / 0.65f);
    int step;
    uint32_t stamp;
    LaserResult result = {end, MATERIAL_EMPTY, false};

    if (world == NULL || world->cells == NULL || length < 0.001f) {
        result.position = start;
        return result;
    }

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        int centerX = (int)floorf(start.x + delta.x * amount);
        int centerY = (int)floorf(start.y + delta.y * amount);
        CellMaterial material;

        if (!WorldInBounds(world, centerX, centerY)) {
            break;
        }
        material = WorldGetCell(world, centerX, centerY);
        if (material == MATERIAL_DIRT || material == MATERIAL_SAND ||
            material == MATERIAL_ROCK) {
            result.position = (Vector2){start.x + delta.x * amount,
                                        start.y + delta.y * amount};
            result.material = material;
            result.hit = true;
            break;
        }
    }

    if (!result.hit) {
        return result;
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    {
        int centerX = (int)floorf(result.position.x);
        int centerY = (int)floorf(result.position.y);
        int brush = (int)ceilf(radius);
        int y;

        for (y = centerY - brush; y <= centerY + brush; ++y) {
            int x;

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

                if (cell->material == MATERIAL_DIRT) {
                    cell->temperature += deltaTime * 2500.0f;
                    WorldWakeCellAndNeighbors(world, x, y);
                    (void)WorldTryThermalTransition(world, x, y);
                } else if (cell->material == MATERIAL_SAND) {
                    cell->temperature += deltaTime * 3100.0f;
                    WorldWakeCellAndNeighbors(world, x, y);
                    (void)WorldTryThermalTransition(world, x, y);
                } else if (cell->material == MATERIAL_ROCK) {
                    cell->temperature += deltaTime * 1080.0f;
                    WorldWakeCellAndNeighbors(world, x, y);
                    (void)WorldTryThermalTransition(world, x, y);
                }
            }
        }
    }

    return result;
}

const char *WorldMaterialName(CellMaterial material)
{
    switch (material) {
        case MATERIAL_DIRT: return "DIRT";
        case MATERIAL_ROCK: return "ROCK";
        case MATERIAL_SAND: return "SAND";
        case MATERIAL_WATER: return "WATER";
        case MATERIAL_LAVA: return "LAVA";
        case MATERIAL_STEAM: return "STEAM";
        case MATERIAL_SMOKE: return "SMOKE";
        case MATERIAL_FIRE: return "FIRE";
        case MATERIAL_ASH: return "ASH";
        case MATERIAL_EMPTY:
        default: return "EMPTY";
    }
}
