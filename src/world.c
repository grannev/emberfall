#include "world.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <raymath.h>

#define FIRE_NEIGHBOR_HEAT_PER_TICK 0.65f
/* Lava heats whatever it touches, but a rock cell must never reach its melt
   threshold from lava alone: otherwise one pocket turns the entire map to lava,
   the way an unbudgeted fire would burn every connected dirt cell. Rock relaxes
   toward ambient at 0.6% of the gap per tick, so at the 720C threshold it sheds
   about 4.2C per tick. Keeping the per-neighbour contribution well under that
   share leaves a boundary cell glowing near 700C forever without melting. */
#define LAVA_NEIGHBOR_HEAT_PER_TICK 3.0f
/* The cap, not the rate, is what keeps a pocket from melting its lining: even
   a cell heated from eight sides at once lands well under rock's 720C. */
#define LAVA_PASSIVE_HEAT_CAP 660.0f
/* Resting temperature of every cell. A cell more than half a degree away from
   it counts as thermally active and keeps its chunk awake, so fresh storage
   must start here — zeroed cells would read as hot and never let chunks sleep. */
#define AMBIENT_TEMPERATURE 20.0f
/* Friction heat left on a drilled tunnel wall. Deliberately below the water
   steam point (108) and far below the dirt ignition point (175). */
#define DRILL_WALL_TEMPERATURE 96.0f

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
    int minimumChunkX;
    int maximumChunkX;
    int minimumChunkY;
    int maximumChunkY;
    int chunkY;

    if (world->activeChunks == NULL || world->nextActiveChunks == NULL ||
        !WorldInBounds(world, x, y)) {
        return;
    }

    /* A cell only ever influences its immediate neighbours, so it needs to wake
       an adjacent chunk only when it sits against that chunk's border. Waking a
       full 3x3 block from the middle of a chunk marked nine chunks - over nine
       thousand cells - for a change that could not leave one of them. */
    centerChunkX = x / WORLD_CHUNK_SIZE;
    centerChunkY = y / WORLD_CHUNK_SIZE;
    minimumChunkX = centerChunkX - (x % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    maximumChunkX = centerChunkX +
                    (x % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    minimumChunkY = centerChunkY - (y % WORLD_CHUNK_SIZE == 0 ? 1 : 0);
    maximumChunkY = centerChunkY +
                    (y % WORLD_CHUNK_SIZE == WORLD_CHUNK_SIZE - 1 ? 1 : 0);
    for (chunkY = minimumChunkY; chunkY <= maximumChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = minimumChunkX; chunkX <= maximumChunkX; ++chunkX) {
            size_t index;

            if (chunkX < 0 || chunkX >= world->chunkColumns ||
                chunkY < 0 || chunkY >= world->chunkRows) {
                continue;
            }
            index = (size_t)chunkY * (size_t)world->chunkColumns + (size_t)chunkX;
            world->activeChunks[index] = 1u;
            world->nextActiveChunks[index] = 1u;
            if (world->dirtyChunks != NULL) {
                world->dirtyChunks[index] = 1u;
            }
        }
    }
}

static bool MaterialIsDynamic(CellMaterial material);
static float MaterialInitialTemperature(CellMaterial material);

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

/* Everything a material *is* lives in this one table; only what a material
   *does* per tick stays as code. Adding a material used to mean finding seven
   separate switch statements, and a forgotten case failed silently. */
typedef struct MaterialInfo {
    const char *name;
    Color color;
    /* Per-channel spread of the coordinate-hash variation, in halves, so a
       material can dither one channel harder than another. */
    signed char variationR;
    signed char variationG;
    signed char variationB;
    float initialTemperature;
    /* Relaxation toward selfHeatTarget, plus a flat per-tick drop for materials
       that simply cool off. */
    float selfHeatTarget;
    float selfHeatRate;
    float linearCoolRate;
    /* Thermal phase change. phaseOnCooling flips the comparison. */
    CellMaterial phaseTarget;
    float phaseThreshold;
    bool phaseOnCooling;
    bool dynamic;
    bool solid;
} MaterialInfo;

static const MaterialInfo MATERIALS[MATERIAL_COUNT] = {
    [MATERIAL_EMPTY] = {
        .name = "EMPTY", .color = {5, 10, 18, 255},
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_EMPTY,
    },
    [MATERIAL_DIRT] = {
        .name = "DIRT", .color = {111, 73, 43, 255},
        .variationR = 2, .variationG = 1,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_FIRE, .phaseThreshold = 175.0f,
        .solid = true,
    },
    [MATERIAL_ROCK] = {
        .name = "ROCK", .color = {72, 77, 86, 255},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_LAVA, .phaseThreshold = 720.0f,
        .solid = true,
    },
    [MATERIAL_SAND] = {
        .name = "SAND", .color = {218, 184, 91, 255},
        .variationR = 2, .variationG = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_EMPTY, .phaseThreshold = 280.0f,
        .dynamic = true, .solid = true,
    },
    [MATERIAL_WATER] = {
        .name = "WATER", .color = {32, 111, 190, 225},
        .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_STEAM, .phaseThreshold = 108.0f,
        .dynamic = true,
    },
    [MATERIAL_LAVA] = {
        .name = "LAVA", .color = {245, 73, 18, 255},
        .variationG = 4,
        .initialTemperature = 900.0f,
        .selfHeatTarget = 900.0f, .selfHeatRate = 0.08f,
        .phaseTarget = MATERIAL_LAVA,
        .dynamic = true,
    },
    [MATERIAL_STEAM] = {
        .name = "STEAM", .color = {204, 222, 229, 178},
        .variationR = 2, .variationG = 2,
        .initialTemperature = 125.0f,
        .linearCoolRate = 0.42f,
        .phaseTarget = MATERIAL_WATER, .phaseThreshold = 58.0f,
        .phaseOnCooling = true,
        .dynamic = true,
    },
    [MATERIAL_SMOKE] = {
        .name = "SMOKE", .color = {83, 88, 94, 205},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = 75.0f,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_SMOKE,
        .dynamic = true,
    },
    [MATERIAL_FIRE] = {
        .name = "FIRE", .color = {255, 132, 24, 245},
        .variationG = 6,
        .initialTemperature = 650.0f,
        .selfHeatTarget = 650.0f, .selfHeatRate = 0.12f,
        .phaseTarget = MATERIAL_FIRE,
        .dynamic = true,
    },
    [MATERIAL_ASH] = {
        .name = "ASH", .color = {112, 108, 104, 255},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .phaseTarget = MATERIAL_ASH,
        .dynamic = true,
    },
};

static const MaterialInfo *MaterialAt(CellMaterial material)
{
    if (material < 0 || material >= MATERIAL_COUNT) {
        return &MATERIALS[MATERIAL_EMPTY];
    }
    return &MATERIALS[material];
}

static bool MaterialIsDynamic(CellMaterial material)
{
    return MaterialAt(material)->dynamic;
}

static float MaterialInitialTemperature(CellMaterial material)
{
    return MaterialAt(material)->initialTemperature;
}

static unsigned char ChannelWithVariation(unsigned char base, signed char spread,
                                         int variation)
{
    int value = (int)base + variation * (int)spread / 2;

    return (unsigned char)Clamp((float)value, 0.0f, 255.0f);
}

/* Solid cells glow toward ember as they approach their own phase threshold, so
   a laser preheating rock and a freshly drilled tunnel wall are both readable. */
static Color MaterialHeatTint(Color base, const MaterialInfo *info, float temperature)
{
    float heat;

    if (temperature < 60.0f || !info->solid || info->phaseOnCooling ||
        info->phaseThreshold <= 60.0f) {
        return base;
    }

    /* Square root keeps the low end readable: rock melts at 720, so a linear
       ramp would hide every temperature a drill or a short laser burst leaves. */
    heat = sqrtf(Clamp((temperature - 60.0f) / (info->phaseThreshold - 60.0f),
                       0.0f, 1.0f));
    base.r = (unsigned char)((float)base.r + (245.0f - (float)base.r) * heat);
    base.g = (unsigned char)((float)base.g + (96.0f - (float)base.g) * heat * 0.8f);
    base.b = (unsigned char)((float)base.b * (1.0f - heat * 0.75f));
    return base;
}

static Color MaterialPixel(const World *world, const Cell *cell, int x, int y)
{
    const MaterialInfo *info = MaterialAt(cell->material);
    Color color = info->color;
    int variation;

    if (cell->material == MATERIAL_EMPTY) {
        /* Empty space is a depth gradient rather than a flat colour. */
        unsigned char glow = (unsigned char)(10 + (y * 10) / world->height);

        return (Color){5, glow, (unsigned char)(18 + glow), 255};
    }

    variation = (int)(CoordinateHash(x, y) % 13u) - 6;
    color.r = ChannelWithVariation(color.r, info->variationR, variation);
    color.g = ChannelWithVariation(color.g, info->variationG, variation);
    color.b = ChannelWithVariation(color.b, info->variationB, variation);
    return MaterialHeatTint(color, info, cell->temperature);
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

/* cap <= 0 means the source imposes no ceiling of its own. */
static void WorldHeatNeighbors(World *world, int x, int y, float heat, float cap)
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
            Cell *target = WorldCell(world, targetX, targetY);

            /* Once a neighbour has saturated, more heat can neither change it
               nor ever push it over a threshold. Skipping it lets a settled
               lava lake stop waking its surroundings every single tick. */
            if (cap > 0.0f && target->temperature >= cap) {
                continue;
            }
            target->temperature += heat;
            WorldWakeCellAndNeighbors(world, targetX, targetY);
        }
    }
}

static bool WorldTryThermalTransition(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);
    const MaterialInfo *info = MaterialAt(cell->material);
    CellMaterial next = info->phaseTarget;
    bool crossed;

    if (next == cell->material) {
        return false;
    }
    crossed = info->phaseOnCooling ? cell->temperature <= info->phaseThreshold
                                   : cell->temperature >= info->phaseThreshold;
    if (!crossed) {
        return false;
    }

    WorldSetCellRaw(world, x, y, next);
    WorldCell(world, x, y)->updatedTick = world->tick;
    return true;
}

static bool WorldUpdateTemperatureState(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);
    const MaterialInfo *info = MaterialAt(cell->material);

    if (cell->material == MATERIAL_EMPTY) {
        return false;
    }

    cell->temperature += (info->selfHeatTarget - cell->temperature) *
                         info->selfHeatRate;
    cell->temperature -= info->linearCoolRate;

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
    /* One burning cell cannot ignite an unlimited chain of ordinary dirt. */
    WorldHeatNeighbors(world, x, y, FIRE_NEIGHBOR_HEAT_PER_TICK, 0.0f);

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
    world->dirtyChunks = malloc(chunkCount * sizeof(*world->dirtyChunks));
    if (world->dirtyChunks != NULL) {
        /* Nothing has been uploaded yet, so every chunk owes the texture a
           first full write. */
        memset(world->dirtyChunks, 1, chunkCount * sizeof(*world->dirtyChunks));
    }
    if (world->cells != NULL) {
        size_t cellIndex;

        for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            world->cells[cellIndex].temperature = AMBIENT_TEMPERATURE;
        }
    }

    if (world->cells == NULL || world->pixels == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL || world->dirtyChunks == NULL) {
        free(world->cells);
        free(world->pixels);
        free(world->activeChunks);
        free(world->nextActiveChunks);
        free(world->dirtyChunks);
        memset(world, 0, sizeof(*world));
        return false;
    }

    return true;
}

bool WorldInitRenderer(World *world)
{
    Image image;

    if (world == NULL || world->cells == NULL) {
        return false;
    }
    if (world->texture.id != 0u) {
        return true;
    }

    image = GenImageColor(world->width, world->height, BLACK);
    world->texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (world->texture.id == 0u) {
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
    free(world->dirtyChunks);
    memset(world, 0, sizeof(*world));
}

/* The surface is a sum of three sine octaves with randomised phase, evaluated
   per column. Keeping it a function instead of a precomputed array is what
   removes the old fixed 512-wide stack buffer and its width limit. */
typedef struct SurfaceProfile {
    float base;
    float amplitude[3];
    float frequency[3];
    float phase[3];
} SurfaceProfile;

static float RandomRange(float minimum, float maximum)
{
    return minimum + (float)GetRandomValue(0, 10000) / 10000.0f * (maximum - minimum);
}

static int SurfaceHeightAt(const SurfaceProfile *profile, int x)
{
    float height = profile->base;
    int octave;

    for (octave = 0; octave < 3; ++octave) {
        height += sinf((float)x * profile->frequency[octave] +
                       profile->phase[octave]) * profile->amplitude[octave];
    }
    return (int)height;
}

/* A rock-lined pocket holding a liquid: the shell keeps the contents from
   draining into whatever caves the generator carved next to it. */
static void WorldPlacePocket(World *world, int centerX, int centerY,
                             int radiusX, int radiusY, CellMaterial fill)
{
    WorldFillEllipse(world, centerX, centerY, radiusX + 4, radiusY + 4,
                     MATERIAL_ROCK);
    WorldFillEllipse(world, centerX, centerY, radiusX, radiusY, MATERIAL_EMPTY);
    WorldFillEllipse(world, centerX, centerY + 2, radiusX - 2, radiusY - 2, fill);
}

void WorldGenerate(World *world)
{
    SurfaceProfile profile;
    float areaRatio;
    int caveCount;
    int pocketCount;
    int bankCount;
    int pillarCount;
    int dirtDepth;
    int feature;
    int octave;
    int x;
    size_t cellIndex;
    size_t cellCount;
    size_t chunkCount;

    if (world == NULL || world->cells == NULL) {
        return;
    }

    cellCount = (size_t)world->width * (size_t)world->height;
    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    memset(world->cells, 0, cellCount * sizeof(*world->cells));
    memset(world->activeChunks, 0, chunkCount * sizeof(*world->activeChunks));
    memset(world->nextActiveChunks, 0, chunkCount * sizeof(*world->nextActiveChunks));
    memset(world->dirtyChunks, 1, chunkCount * sizeof(*world->dirtyChunks));
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
        world->cells[cellIndex].temperature = AMBIENT_TEMPERATURE;
    }
    world->tick = 0;
    world->effectSerial = 0;

    /* Layout is expressed as fractions of the world, but feature sizes stay
       absolute: a larger world gets more caves and pockets of the same scale,
       not the same layout stretched out. */
    profile.base = (float)world->height * 0.36f;
    profile.amplitude[0] = (float)world->height * 0.045f;
    profile.amplitude[1] = (float)world->height * 0.031f;
    profile.amplitude[2] = (float)world->height * 0.014f;
    profile.frequency[0] = RandomRange(0.008f, 0.014f);
    profile.frequency[1] = RandomRange(0.026f, 0.042f);
    profile.frequency[2] = RandomRange(0.070f, 0.110f);
    for (octave = 0; octave < 3; ++octave) {
        profile.phase[octave] = RandomRange(0.0f, 6.283f);
    }
    dirtDepth = (int)((float)world->height * 0.20f);

    for (x = 0; x < world->width; ++x) {
        int surfaceY = SurfaceHeightAt(&profile, x);
        int y;

        if (surfaceY < 1) {
            surfaceY = 1;
        }
        for (y = surfaceY; y < world->height; ++y) {
            int depth = y - surfaceY;
            CellMaterial material =
                depth > dirtDepth + (int)(CoordinateHash(x, y) % 17u)
                    ? MATERIAL_ROCK
                    : MATERIAL_DIRT;

            WorldSetCellRaw(world, x, y, material);
        }
    }

    areaRatio = (float)world->width * (float)world->height / (512.0f * 288.0f);
    caveCount = (int)(31.0f * areaRatio);
    pocketCount = (int)(2.0f * areaRatio);
    bankCount = (int)(0.9f * areaRatio);
    pillarCount = (int)(1.0f * areaRatio);
    if (caveCount < 1) caveCount = 1;
    if (pocketCount < 1) pocketCount = 1;
    if (bankCount < 1) bankCount = 1;
    if (pillarCount < 1) pillarCount = 1;

    /* Each cave is a short chain of overlapping ellipses, which reads as a
       hollowed-out chamber rather than the identical egg a single ellipse
       gives. */
    for (feature = 0; feature < caveCount; ++feature) {
        int centerX = GetRandomValue(20, world->width - 21);
        int centerY = GetRandomValue((int)((float)world->height * 0.48f),
                                     (int)((float)world->height * 0.91f));
        int lobes = GetRandomValue(2, 4);
        int lobe;

        for (lobe = 0; lobe < lobes; ++lobe) {
            int radiusX = GetRandomValue(8, 24);
            int radiusY = GetRandomValue(5, 13);

            WorldFillEllipse(world, centerX, centerY, radiusX, radiusY,
                             MATERIAL_EMPTY);
            centerX += GetRandomValue(-18, 18);
            centerY += GetRandomValue(-9, 9);
        }
    }

    /* Water sits above the lava band so the two only meet when the player digs
       between them. */
    for (feature = 0; feature < pocketCount; ++feature) {
        int centerX = GetRandomValue(40, world->width - 41);
        int centerY = GetRandomValue((int)((float)world->height * 0.55f),
                                     (int)((float)world->height * 0.70f));

        WorldPlacePocket(world, centerX, centerY, GetRandomValue(18, 30),
                         GetRandomValue(11, 18), MATERIAL_WATER);
    }
    for (feature = 0; feature < pocketCount; ++feature) {
        int centerX = GetRandomValue(40, world->width - 41);
        int centerY = GetRandomValue((int)((float)world->height * 0.78f),
                                     (int)((float)world->height * 0.93f));

        WorldPlacePocket(world, centerX, centerY, GetRandomValue(16, 27),
                         GetRandomValue(9, 14), MATERIAL_LAVA);
    }

    /* Loose sand banks on the surface, which immediately demonstrate falling
       cell physics wherever the player happens to start. */
    for (feature = 0; feature < bankCount; ++feature) {
        int bankWidth = GetRandomValue(48, 92);
        int bankStart = GetRandomValue(4, world->width - bankWidth - 5);
        int bankDepth = GetRandomValue(12, 24);
        int column;

        for (column = 0; column < bankWidth; ++column) {
            int worldX = bankStart + column;
            int surfaceY = SurfaceHeightAt(&profile, worldX);
            int crown = (int)(8.0f * sinf((float)column / (float)bankWidth * PI));
            int y;

            for (y = surfaceY - 1 - crown; y < surfaceY + bankDepth; ++y) {
                WorldSetCellRaw(world, worldX, y, MATERIAL_SAND);
            }
        }
    }

    /* Breakable dirt pillars: obvious first laser and drill targets. */
    for (feature = 0; feature < pillarCount; ++feature) {
        /* Wide and low enough to read as a standing butte. Narrow, tall columns
           looked like stray needles poking out of the skyline. One height for
           the whole pillar: rolling it per column made them ragged. */
        int pillarWidth = GetRandomValue(15, 28);
        int pillarX = GetRandomValue(4, world->width - pillarWidth - 5);
        int pillarHeight = GetRandomValue(16, 34);
        int column;

        for (column = 0; column < pillarWidth; ++column) {
            int worldX = pillarX + column;
            int surfaceY = SurfaceHeightAt(&profile, worldX);
            int y;

            for (y = surfaceY - pillarHeight; y < surfaceY + 48; ++y) {
                WorldSetCellRaw(world, worldX, y, MATERIAL_DIRT);
            }
        }
    }

    WorldCountActiveState(world);
}

Vector2 WorldPlayerSpawn(const World *world)
{
    int x;
    int y;

    if (world == NULL || world->cells == NULL) {
        return (Vector2){0.0f, 0.0f};
    }

    /* Drop straight down from the middle of the sky and stop short of the first
       solid cell, so the spawn is open air whatever the generator produced. */
    x = world->width / 2;
    for (y = 0; y < world->height; ++y) {
        if (WorldMaterialIsSolid(WorldGetCell(world, x, y))) {
            break;
        }
    }
    y -= 10;
    if (y < 4) {
        y = 4;
    }
    return (Vector2){(float)x + 0.5f, (float)y + 0.5f};
}

static void WorldUpdateCellAt(World *world, int x, int y)
{
    Cell *cell = WorldCell(world, x, y);
    int direction = ((CoordinateHash(x, y) + world->tick) & 1u) != 0u ? 1 : -1;
    float temperatureBefore;

    if (cell->updatedTick == world->tick) {
        return;
    }

    /* A chunk stays awake because something actually happened in it, not merely
       because it contains a dynamic or hot cell. Movement and material changes
       already wake their own neighbourhood, so a meaningful temperature change
       is the remaining case. This is what lets a settled sand pile or the
       interior of a lava lake sleep while its boundary keeps working: whatever
       later disturbs them - a drill, an explosion, a cell moving nearby - wakes
       the surrounding chunks on its way through. */
    temperatureBefore = cell->temperature;
    if (WorldUpdateTemperatureState(world, x, y)) {
        return;
    }
    if (fabsf(cell->temperature - temperatureBefore) > 0.05f) {
        WorldWakeCellAndNeighbors(world, x, y);
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
                WorldHeatNeighbors(world, x, y, LAVA_NEIGHBOR_HEAT_PER_TICK,
                                   LAVA_PASSIVE_HEAT_CAP);
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

    world->activeChunkCount = 0;
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;

            if (world->activeChunks[chunkIndex] != 0u) {
                ++world->activeChunkCount;
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
        size_t chunkIndex;

        /* Mark the set that was actually simulated, not the one that will run
           next tick: a chunk that settles and goes to sleep still owes the
           texture its final frame. */
        for (chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
            world->dirtyChunks[chunkIndex] |= world->activeChunks[chunkIndex];
        }
        world->activeChunks = world->nextActiveChunks;
        world->nextActiveChunks = previousChunks;
    }
    WorldCountActiveState(world);
}

void WorldDraw(World *world)
{
    size_t chunkCount;
    int minimumRow;
    int maximumRow;
    int chunkY;

    if (world == NULL || world->pixels == NULL || world->dirtyChunks == NULL ||
        world->texture.id == 0u) {
        return;
    }

    chunkCount = (size_t)world->chunkColumns * (size_t)world->chunkRows;
    minimumRow = world->chunkRows;
    maximumRow = -1;

    /* Rebuild only the chunks that changed. The simulation sleeps on a settled
       world, and so must the renderer. */
    for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
        bool rowDirty = false;
        int chunkX;

        for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int y;

            if (world->dirtyChunks[chunkIndex] == 0u) {
                continue;
            }
            rowDirty = true;
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;

            for (y = minimumY; y < maximumY; ++y) {
                int x;

                for (x = minimumX; x < maximumX; ++x) {
                    world->pixels[WorldIndex(world, x, y)] =
                        MaterialPixel(world, WorldCellConst(world, x, y), x, y);
                }
            }
        }

        if (rowDirty) {
            if (chunkY < minimumRow) minimumRow = chunkY;
            if (chunkY > maximumRow) maximumRow = chunkY;
        }
    }

    if (maximumRow >= 0) {
        /* One upload covering the dirty rows. A full-width band keeps the
           source rows contiguous, so no staging copy is needed. */
        int bandStart = minimumRow * WORLD_CHUNK_SIZE;
        int bandEnd = (maximumRow + 1) * WORLD_CHUNK_SIZE;
        Rectangle band;

        if (bandEnd > world->height) {
            bandEnd = world->height;
        }
        band = (Rectangle){0.0f, (float)bandStart, (float)world->width,
                           (float)(bandEnd - bandStart)};
        UpdateTextureRec(world->texture, band,
                         &world->pixels[WorldIndex(world, 0, bandStart)]);
        memset(world->dirtyChunks, 0, chunkCount * sizeof(*world->dirtyChunks));
    }

    DrawTexture(world->texture, 0, 0, WHITE);
}

/* Walking cells to produce one debug number is not worth doing every tick, so
   the exact count is computed only when something actually asks for it. */
int WorldCountDynamicCells(const World *world)
{
    int count = 0;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL) {
        return 0;
    }

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
                        ++count;
                    }
                }
            }
        }
    }
    return count;
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

void WorldSetTemperature(World *world, int x, int y, float temperature)
{
    if (world == NULL || world->cells == NULL || !WorldInBounds(world, x, y)) {
        return;
    }

    WorldCell(world, x, y)->temperature = temperature;
    WorldWakeCellAndNeighbors(world, x, y);
}

bool WorldMaterialIsSolid(CellMaterial material)
{
    return MaterialAt(material)->solid;
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

int WorldDrillCircle(World *world, int centerX, int centerY, int radius)
{
    int destroyed = 0;
    int radiusSquared = radius * radius;
    int rimSquared = (radius + 1) * (radius + 1);
    int y;

    if (world == NULL || world->cells == NULL || radius < 0) {
        return 0;
    }

    for (y = centerY - radius - 1; y <= centerY + radius + 1; ++y) {
        int x;

        for (x = centerX - radius - 1; x <= centerX + radius + 1; ++x) {
            int dx = x - centerX;
            int dy = y - centerY;
            int distanceSquared = dx * dx + dy * dy;
            Cell *cell;

            if (distanceSquared > rimSquared || !WorldInBounds(world, x, y)) {
                continue;
            }

            cell = WorldCell(world, x, y);
            if (distanceSquared > radiusSquared ||
                !WorldMaterialIsSolid(cell->material)) {
                /* Everything the drill cannot cut is only warmed. The cap stays
                   under every phase threshold, so a tunnel can never ignite
                   dirt or boil water on its own. */
                if (cell->material != MATERIAL_EMPTY &&
                    cell->temperature < DRILL_WALL_TEMPERATURE) {
                    cell->temperature = DRILL_WALL_TEMPERATURE;
                    WorldWakeCellAndNeighbors(world, x, y);
                }
                continue;
            }

            if (GetRandomValue(0, 99) < 3) {
                WorldSetCellRaw(world, x, y, MATERIAL_ASH);
            } else {
                WorldSetCellRaw(world, x, y, MATERIAL_EMPTY);
            }
            ++destroyed;
        }
    }

    return destroyed;
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
    return MaterialAt(material)->name;
}
