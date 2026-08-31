#include "world.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <raymath.h>

#include "world_render_data.h"

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

/* Ambient floor: even sealed rock stays legible, and a light of zero would make
   an unlit tunnel unplayable rather than atmospheric. Lighting must read as
   atmosphere, not as an unlit image; sealed ground is meant to be dim and
   obviously solid, never a black hole in the picture. */
#define WORLD_MINIMUM_LIGHT 0.40f
/* Transmission per light cell, i.e. per WORLD_LIGHT_SCALE world cells. Open air
   carries light across a large cavern; solid material swallows it over a few
   dozen cells, which is what makes a deep bore go dark while a shallow one still
   sees daylight. */
#define WORLD_LIGHT_OPEN_TRANSMISSION 0.97f
#define WORLD_LIGHT_SOLID_TRANSMISSION 0.74f
/* The solved light is quantised to this many steps. A pixel channel is one byte,
   so a finer change cannot alter the image; quantising lets the renderer compare
   light exactly instead of against a tolerance. A tolerance drifts: a sample
   that moves less than it each frame is never rebuilt, and the texture wanders
   arbitrarily far from the light it should be showing. */
#define WORLD_LIGHT_STEPS 512.0f
/* Temperature at which material starts to glow on its own, and the span over
   which that glow reaches full strength. */
#define WORLD_LIGHT_HEAT_FLOOR 180.0f
#define WORLD_LIGHT_HEAT_SPAN 520.0f

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
                world->lightDirtyChunks[index] = 1u;
            }
        }
    }
}

static bool MaterialIsDynamic(CellMaterial material);
static float MaterialInitialTemperature(CellMaterial material);

/* Generation writes millions of cells, but none of them has interacted yet.
   Keeping them asleep lets a huge map stream its simulation around the player;
   WorldActivateRegion wakes generated dynamics before they enter play, while
   every actual mutation still uses the ordinary local wake path. */
static void WorldSetGeneratedCell(World *world, int x, int y,
                                  CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    cell->material = (uint8_t)material;
    cell->temperature = MaterialInitialTemperature(material);
    cell->lifetime = 0;
    cell->effectStamp = 0;
}

static void WorldSetCellRaw(World *world, int x, int y, CellMaterial material)
{
    Cell *cell;

    if (!WorldInBounds(world, x, y)) {
        return;
    }

    cell = WorldCell(world, x, y);
    cell->material = (uint8_t)material;
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
                WorldSetGeneratedCell(world, x, y, material);
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
typedef struct MaterialPhase {
    bool enabled;
    CellMaterial target;
    float threshold;
} MaterialPhase;

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
    /* Thermal phase changes, one in each direction, because water both boils
       and freezes. `enabled` exists so that the zero value of a forgotten field
       is inert: encoding "no transition" as a target equal to the material
       itself looks tidy but means an unwritten field reads back as "become
       MATERIAL_EMPTY at 0C", and the cryo beam duly deleted every rock, dirt
       and sand cell it touched. */
    MaterialPhase onHeat;
    MaterialPhase onCool;
    bool dynamic;
    bool solid;
    /* How much light the material gives off by itself, 0..1. Heat adds more on
       top of this, so a laser-blasted rock face lights its own crater. */
    float emission;
    /* Degrees per second the laser pours into this material. Zero means the
       laser does not work it. Keeping it here rather than in a switch is what
       makes a new material one table entry: the first version of ice was solid,
       stopped nothing, and could not be melted, because the laser still asked
       for three material names by hand. */
    float laserHeatRate;
} MaterialInfo;

static const MaterialInfo MATERIALS[MATERIAL_COUNT] = {
    [MATERIAL_EMPTY] = {
        .name = "EMPTY", .color = {5, 10, 18, 255},
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
            },
    [MATERIAL_DIRT] = {
        .name = "DIRT", .color = {111, 73, 43, 255},
        .variationR = 2, .variationG = 1,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_FIRE, 175.0f},
        .solid = true,
        .laserHeatRate = 2500.0f,
    },
    [MATERIAL_ROCK] = {
        .name = "ROCK", .color = {72, 77, 86, 255},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_LAVA, 720.0f},
        .solid = true,
        .laserHeatRate = 1080.0f,
    },
    [MATERIAL_SAND] = {
        .name = "SAND", .color = {218, 184, 91, 255},
        .variationR = 2, .variationG = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_EMPTY, 280.0f},
        .dynamic = true, .solid = true,
        .laserHeatRate = 3100.0f,
    },
    [MATERIAL_WATER] = {
        .name = "WATER", .color = {32, 111, 190, 225},
        .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .onHeat = {true, MATERIAL_STEAM, 108.0f},
        .onCool = {true, MATERIAL_ICE, -4.0f},
        .dynamic = true,
    },
    [MATERIAL_LAVA] = {
        .name = "LAVA", .color = {245, 73, 18, 255},
        .variationG = 4,
        .initialTemperature = 900.0f,
        .selfHeatTarget = 900.0f, .selfHeatRate = 0.08f,
                /* Lava will not cool this far on its own — it relaxes back toward 900 —
           so this threshold only ever fires under the cryo beam. */
        .onCool = {true, MATERIAL_ROCK, 620.0f},
        .dynamic = true,
        .emission = 1.0f,
    },
    [MATERIAL_STEAM] = {
        .name = "STEAM", .color = {204, 222, 229, 178},
        .variationR = 2, .variationG = 2,
        .initialTemperature = 125.0f,
        .linearCoolRate = 0.42f,
        .onCool = {true, MATERIAL_WATER, 58.0f},
        .dynamic = true,
    },
    [MATERIAL_SMOKE] = {
        .name = "SMOKE", .color = {83, 88, 94, 205},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = 75.0f,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
        .dynamic = true,
    },
    [MATERIAL_FIRE] = {
        .name = "FIRE", .color = {255, 132, 24, 245},
        .variationG = 6,
        .initialTemperature = 650.0f,
        .selfHeatTarget = 650.0f, .selfHeatRate = 0.12f,
        /* Chilled fire is put out and leaves smoke, the same residue it leaves
           when it burns out on its own. Fire relaxes back toward 650C, so
           nothing but the cryo beam ever reaches this. */
        .onCool = {true, MATERIAL_SMOKE, 120.0f},
        .dynamic = true,
        .emission = 0.92f,
    },
    [MATERIAL_ICE] = {
        .name = "ICE", .color = {152, 203, 231, 245},
        .variationG = 2, .variationB = 2,
        .initialTemperature = -14.0f,
        /* Ice does not drift back to ambient. A slow drift cannot work here: a
           cell whose temperature moves less than the sleep threshold each tick
           never wakes its own chunk, so it would simply stop being simulated and
           the ice would be permanent anyway, only unpredictably so. Making it
           stable on purpose is honest and gives the player the one thing no
           other power does — a way to add material to the world. Anything warm
           still melts it: a laser, a fire, a lava flow. */
        .selfHeatTarget = -14.0f, .selfHeatRate = 0.0f,
        .onHeat = {true, MATERIAL_WATER, 2.0f},
        .solid = true,
        .laserHeatRate = 600.0f,
    },
    [MATERIAL_ASH] = {
        .name = "ASH", .color = {112, 108, 104, 255},
        .variationR = 2, .variationG = 2, .variationB = 2,
        .initialTemperature = AMBIENT_TEMPERATURE,
        .selfHeatTarget = AMBIENT_TEMPERATURE, .selfHeatRate = 0.006f,
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

    if (temperature < 60.0f || !info->solid || !info->onHeat.enabled ||
        info->onHeat.threshold <= 60.0f) {
        return base;
    }

    /* Square root keeps the low end readable: rock melts at 720, so a linear
       ramp would hide every temperature a drill or a short laser burst leaves. */
    heat = sqrtf(Clamp((temperature - 60.0f) / (info->onHeat.threshold - 60.0f),
                       0.0f, 1.0f));
    base.r = (unsigned char)((float)base.r + (245.0f - (float)base.r) * heat);
    base.g = (unsigned char)((float)base.g + (96.0f - (float)base.g) * heat * 0.8f);
    base.b = (unsigned char)((float)base.b * (1.0f - heat * 0.75f));
    return base;
}

/* Takes the light level rather than sampling it: this runs for every cell of
   every dirty chunk, and doing the bilinear lookup here — with its floor and its
   clamps — cost more than the rest of drawing put together. The caller walks a
   chunk in order and can hoist all of that out of the loop. */
static Color MaterialPixel(const World *world, const Cell *cell, int x, int y,
                           float red, float green, float blue)
{
    const MaterialInfo *info = MaterialAt(cell->material);
    Color color = info->color;
    int variation;

    if (cell->material == MATERIAL_EMPTY) {
        /* Empty space is a depth gradient rather than a flat colour. */
        unsigned char glow = (unsigned char)(10 + (y * 10) / world->height);

        color = (Color){5, glow, (unsigned char)(18 + glow), 255};
    } else {
        variation = (int)(CoordinateHash(x, y) % 13u) - 6;
        color.r = ChannelWithVariation(color.r, info->variationR, variation);
        color.g = ChannelWithVariation(color.g, info->variationG, variation);
        color.b = ChannelWithVariation(color.b, info->variationB, variation);
        color = MaterialHeatTint(color, info, cell->temperature);

        /* An emitter lights itself; dimming lava by its own falloff would make
           the middle of a lake darker than its shore. */
        if (info->emission >= 0.999f) {
            return color;
        }
    }

    /* Alpha carries material translucency and has nothing to do with light.
       Only the warm channel can exceed the original value, so a single ceiling
       test per channel is enough and no libm clamp is needed. */
    red *= (float)color.r;
    green *= (float)color.g;
    blue *= (float)color.b;
    color.r = (unsigned char)(red > 255.0f ? 255.0f : red);
    color.g = (unsigned char)(green > 255.0f ? 255.0f : green);
    color.b = (unsigned char)(blue > 255.0f ? 255.0f : blue);
    return color;
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
    CellMaterial next = cell->material;

    if (info->onHeat.enabled && cell->temperature >= info->onHeat.threshold) {
        next = info->onHeat.target;
    } else if (info->onCool.enabled &&
               cell->temperature <= info->onCool.threshold) {
        next = info->onCool.target;
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
    size_t lightCount;

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
    world->activeChunks = calloc(chunkCount, sizeof(*world->activeChunks));
    world->nextActiveChunks = calloc(chunkCount, sizeof(*world->nextActiveChunks));
    world->lightColumns = (width + WORLD_LIGHT_SCALE - 1) / WORLD_LIGHT_SCALE;
    world->lightRows = (height + WORLD_LIGHT_SCALE - 1) / WORLD_LIGHT_SCALE;
    lightCount = (size_t)world->lightColumns * (size_t)world->lightRows;
    world->lightSky = calloc(lightCount, sizeof(*world->lightSky));
    world->lightEmber = calloc(lightCount, sizeof(*world->lightEmber));
    world->lightShownSky = calloc(lightCount, sizeof(*world->lightShownSky));
    world->lightShownEmber = calloc(lightCount, sizeof(*world->lightShownEmber));
    world->lightEmission = calloc(lightCount, sizeof(*world->lightEmission));
    world->lightOpacity = calloc(lightCount, sizeof(*world->lightOpacity));
    world->dirtyChunks = malloc(chunkCount * sizeof(*world->dirtyChunks));
    world->lightDirtyChunks = malloc(chunkCount * sizeof(*world->lightDirtyChunks));
    if (world->dirtyChunks != NULL) {
        /* Nothing has been uploaded yet, so every chunk owes the texture a
           first full write. */
        memset(world->dirtyChunks, 1, chunkCount * sizeof(*world->dirtyChunks));
    }
    if (world->lightDirtyChunks != NULL) {
        memset(world->lightDirtyChunks, 1,
               chunkCount * sizeof(*world->lightDirtyChunks));
    }
    if (world->cells != NULL) {
        size_t cellIndex;

        for (cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
            world->cells[cellIndex].temperature = AMBIENT_TEMPERATURE;
        }
    }

    if (world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL || world->dirtyChunks == NULL ||
        world->lightDirtyChunks == NULL ||
        world->lightSky == NULL || world->lightEmber == NULL ||
        world->lightShownSky == NULL || world->lightShownEmber == NULL ||
        world->lightEmission == NULL || world->lightOpacity == NULL) {
        WorldUnload(world);
        return false;
    }

    /* The shown copies start impossible so the first draw re-lights every
       chunk. */
    {
        size_t lightIndex;

        for (lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
            world->lightShownSky[lightIndex] = -1.0f;
            world->lightShownEmber[lightIndex] = -1.0f;
        }
    }

    return true;
}

void WorldUnload(World *world)
{
    if (world == NULL) {
        return;
    }

    free(world->cells);
    free(world->activeChunks);
    free(world->nextActiveChunks);
    free(world->dirtyChunks);
    free(world->lightDirtyChunks);
    free(world->lightSky);
    free(world->lightEmber);
    free(world->lightShownSky);
    free(world->lightShownEmber);
    free(world->lightEmission);
    free(world->lightOpacity);
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
    memset(world->lightDirtyChunks, 1,
           chunkCount * sizeof(*world->lightDirtyChunks));
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

            WorldSetGeneratedCell(world, x, y, material);
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
                WorldSetGeneratedCell(world, worldX, y, MATERIAL_SAND);
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
                WorldSetGeneratedCell(world, worldX, y, MATERIAL_DIRT);
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

void WorldActivateRegion(World *world, Rectangle region)
{
    int firstChunkX;
    int lastChunkX;
    int firstChunkY;
    int lastChunkY;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->activeChunks == NULL ||
        world->nextActiveChunks == NULL || region.width <= 0.0f ||
        region.height <= 0.0f) {
        return;
    }

    firstChunkX = (int)floorf(region.x / (float)WORLD_CHUNK_SIZE);
    lastChunkX = (int)floorf((region.x + region.width - 0.001f) /
                            (float)WORLD_CHUNK_SIZE);
    firstChunkY = (int)floorf(region.y / (float)WORLD_CHUNK_SIZE);
    lastChunkY = (int)floorf((region.y + region.height - 0.001f) /
                            (float)WORLD_CHUNK_SIZE);
    if (firstChunkX < 0) firstChunkX = 0;
    if (firstChunkY < 0) firstChunkY = 0;
    if (lastChunkX >= world->chunkColumns) lastChunkX = world->chunkColumns - 1;
    if (lastChunkY >= world->chunkRows) lastChunkY = world->chunkRows - 1;
    if (firstChunkX > lastChunkX || firstChunkY > lastChunkY) {
        return;
    }

    for (chunkY = firstChunkY; chunkY <= lastChunkY; ++chunkY) {
        int chunkX;

        for (chunkX = firstChunkX; chunkX <= lastChunkX; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int y;
            bool needsSimulation = false;

            if (world->activeChunks[chunkIndex] != 0u ||
                world->nextActiveChunks[chunkIndex] != 0u) {
                continue;
            }
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;

            for (y = minimumY; y < maximumY && !needsSimulation; ++y) {
                int x;

                for (x = minimumX; x < maximumX; ++x) {
                    const Cell *cell = WorldCellConst(world, x, y);
                    CellMaterial material = (CellMaterial)cell->material;

                    if (MaterialIsDynamic(material) ||
                        fabsf(cell->temperature -
                              MaterialInitialTemperature(material)) > 0.05f) {
                        needsSimulation = true;
                        break;
                    }
                }
            }
            if (needsSimulation) {
                world->activeChunks[chunkIndex] = 1u;
                world->nextActiveChunks[chunkIndex] = 1u;
            }
        }
    }
    WorldCountActiveState(world);
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
    world->lastTickStats = (WorldTickStats){0};
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
                if (y == maximumY) {
                    ++world->lastTickStats.processedChunks;
                    world->lastTickStats.processedCells +=
                        (uint64_t)(maximumX - minimumX) *
                        (uint64_t)(maximumY - minimumY + 1);
                }
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
            world->lightDirtyChunks[chunkIndex] |= world->activeChunks[chunkIndex];
        }
        world->activeChunks = world->nextActiveChunks;
        world->nextActiveChunks = previousChunks;
    }
    WorldCountActiveState(world);
}


/* ---- Lighting ------------------------------------------------------------
 *
 * Light is solved on a grid WORLD_LIGHT_SCALE times coarser than the cells.
 * Two derived fields feed it: `lightEmission`, how much a block of cells gives
 * off, and `lightOpacity`, how much of it is solid. Both are refreshed only for
 * dirty chunks, because terrain only changes where the simulation is awake.
 *
 * The solve itself is a seed followed by two raster sweeps. Sky light is filled
 * per column from the top and needs no iteration, which is what keeps the open
 * surface uniformly bright no matter how tall the world is; the sweeps then
 * carry that light, plus every emitter, sideways and into overhangs, attenuated
 * by whatever it passes through. Two sweeps are not an exact flood fill around
 * a hairpin corridor, but they are stable, allocation-free, and close enough
 * that the error is invisible at four cells per sample.
 */

static int WorldLightIndex(const World *world, int lightX, int lightY)
{
    return lightY * world->lightColumns + lightX;
}

/* Rebuilds emission and opacity for one chunk's worth of light cells. */
static void WorldRefreshLightBlock(World *world, int chunkX, int chunkY)
{
    int firstLightX = chunkX * WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int firstLightY = chunkY * WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lastLightX = firstLightX + WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lastLightY = firstLightY + WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;
    int lightX;
    int lightY;

    if (lastLightX > world->lightColumns) lastLightX = world->lightColumns;
    if (lastLightY > world->lightRows) lastLightY = world->lightRows;

    for (lightY = firstLightY; lightY < lastLightY; ++lightY) {
        for (lightX = firstLightX; lightX < lastLightX; ++lightX) {
            int firstX = lightX * WORLD_LIGHT_SCALE;
            int firstY = lightY * WORLD_LIGHT_SCALE;
            int lastX = firstX + WORLD_LIGHT_SCALE;
            int lastY = firstY + WORLD_LIGHT_SCALE;
            float emission = 0.0f;
            int solid = 0;
            int samples = 0;
            int x;
            int y;

            if (lastX > world->width) lastX = world->width;
            if (lastY > world->height) lastY = world->height;

            for (y = firstY; y < lastY; ++y) {
                for (x = firstX; x < lastX; ++x) {
                    const Cell *cell = WorldCellConst(world, x, y);
                    const MaterialInfo *info = MaterialAt(cell->material);
                    float heatGlow = (cell->temperature - WORLD_LIGHT_HEAT_FLOOR) /
                                     WORLD_LIGHT_HEAT_SPAN;

                    ++samples;
                    if (info->solid) {
                        ++solid;
                    }
                    /* The brightest cell in the block wins rather than the mean:
                       a single lava cell in a wall is a light source, and
                       averaging would dim it into nothing. */
                    if (info->emission > emission) {
                        emission = info->emission;
                    }
                    if (cell->material != MATERIAL_EMPTY && heatGlow > emission) {
                        emission = Clamp(heatGlow, 0.0f, 1.0f);
                    }
                }
            }

            world->lightEmission[WorldLightIndex(world, lightX, lightY)] = emission;
            world->lightOpacity[WorldLightIndex(world, lightX, lightY)] =
                samples > 0 ? (float)solid / (float)samples : 0.0f;
        }
    }
}

static void WorldSeedLight(World *world)
{
    int lightX;
    int lightY;
    size_t lightCount = (size_t)world->lightColumns * (size_t)world->lightRows;
    size_t index;

    for (index = 0; index < lightCount; ++index) {
        world->lightSky[index] = 0.0f;
        world->lightEmber[index] = world->lightEmission[index];
    }

    /* Sky light: fill each column from the top while it stays open. Doing this
       as a column walk rather than as propagation is what lets open air stay at
       full brightness however deep the world is. */
    for (lightX = 0; lightX < world->lightColumns; ++lightX) {
        for (lightY = 0; lightY < world->lightRows; ++lightY) {
            int index2 = WorldLightIndex(world, lightX, lightY);

            if (world->lightOpacity[index2] > 0.35f) {
                break;
            }
            world->lightSky[index2] = 1.0f;
        }
    }

    /* The player's own lamp is ember rather than sky: it should warm a tunnel
       the way a flare does, not read as a hole cut through to daylight. */
    if (world->pointLightStrength > 0.0f && world->pointLightRadius > 0.0f) {
        float radius = world->pointLightRadius / (float)WORLD_LIGHT_SCALE;
        int centerX = (int)(world->pointLight.x / (float)WORLD_LIGHT_SCALE);
        int centerY = (int)(world->pointLight.y / (float)WORLD_LIGHT_SCALE);
        int span = (int)ceilf(radius);

        for (lightY = centerY - span; lightY <= centerY + span; ++lightY) {
            for (lightX = centerX - span; lightX <= centerX + span; ++lightX) {
                float dx = (float)(lightX - centerX);
                float dy = (float)(lightY - centerY);
                float distance = sqrtf(dx * dx + dy * dy);
                float value;
                int index2;

                if (lightX < 0 || lightY < 0 || lightX >= world->lightColumns ||
                    lightY >= world->lightRows || distance > radius) {
                    continue;
                }
                value = world->pointLightStrength * (1.0f - distance / radius);
                index2 = WorldLightIndex(world, lightX, lightY);
                if (value > world->lightEmber[index2]) {
                    world->lightEmber[index2] = value;
                }
            }
        }
    }
}

static float WorldLightTransmission(const World *world, int index)
{
    float opacity = world->lightOpacity[index];

    return WORLD_LIGHT_OPEN_TRANSMISSION +
           (WORLD_LIGHT_SOLID_TRANSMISSION - WORLD_LIGHT_OPEN_TRANSMISSION) * opacity;
}

/* Carries both channels across one edge. They share the geometry, so solving
   them together costs far less than two separate sweeps. */
static void WorldSpreadLight(World *world, int sourceIndex, float transmission,
                             float *bestSky, float *bestEmber)
{
    float sky = world->lightSky[sourceIndex] * transmission;
    float ember = world->lightEmber[sourceIndex] * transmission;

    if (sky > *bestSky) {
        *bestSky = sky;
    }
    if (ember > *bestEmber) {
        *bestEmber = ember;
    }
}

static float WorldQuantiseLight(float value)
{
    return floorf(Clamp(value, 0.0f, 1.0f) * WORLD_LIGHT_STEPS) / WORLD_LIGHT_STEPS;
}

static void WorldSolveLight(World *world)
{
    int lightX;
    int lightY;
    /* Diagonal neighbours are one and a half cells away, near enough; the exact
       root of two costs a call and changes nothing visible. */
    const float diagonal = 0.87f;

    WorldSeedLight(world);

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        for (lightX = 0; lightX < world->lightColumns; ++lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            float transmission = WorldLightTransmission(world, index);
            float sky = world->lightSky[index];
            float ember = world->lightEmber[index];

            if (lightX > 0) {
                WorldSpreadLight(world, index - 1, transmission, &sky, &ember);
            }
            if (lightY > 0) {
                WorldSpreadLight(world, index - world->lightColumns, transmission,
                                 &sky, &ember);
                if (lightX > 0) {
                    WorldSpreadLight(world, index - world->lightColumns - 1,
                                     transmission * diagonal, &sky, &ember);
                }
                if (lightX + 1 < world->lightColumns) {
                    WorldSpreadLight(world, index - world->lightColumns + 1,
                                     transmission * diagonal, &sky, &ember);
                }
            }
            world->lightSky[index] = sky;
            world->lightEmber[index] = ember;
        }
    }

    for (lightY = world->lightRows - 1; lightY >= 0; --lightY) {
        for (lightX = world->lightColumns - 1; lightX >= 0; --lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            float transmission = WorldLightTransmission(world, index);
            float sky = world->lightSky[index];
            float ember = world->lightEmber[index];

            if (lightX + 1 < world->lightColumns) {
                WorldSpreadLight(world, index + 1, transmission, &sky, &ember);
            }
            if (lightY + 1 < world->lightRows) {
                WorldSpreadLight(world, index + world->lightColumns, transmission,
                                 &sky, &ember);
                if (lightX > 0) {
                    WorldSpreadLight(world, index + world->lightColumns - 1,
                                     transmission * diagonal, &sky, &ember);
                }
                if (lightX + 1 < world->lightColumns) {
                    WorldSpreadLight(world, index + world->lightColumns + 1,
                                     transmission * diagonal, &sky, &ember);
                }
            }
            world->lightSky[index] = WorldQuantiseLight(sky);
            world->lightEmber[index] = WorldQuantiseLight(ember);
        }
    }
}

/* A chunk whose light moved owes the texture a rebuild even though none of its
   cells changed. Without this the incremental renderer would show stale
   lighting: carving a shaft would brighten nothing until something moved. */
static void WorldMarkRelitChunks(World *world)
{
    int lightX;
    int lightY;
    int perChunk = WORLD_CHUNK_SIZE / WORLD_LIGHT_SCALE;

    for (lightY = 0; lightY < world->lightRows; ++lightY) {
        for (lightX = 0; lightX < world->lightColumns; ++lightX) {
            int index = WorldLightIndex(world, lightX, lightY);
            int chunkX;
            int chunkY;

            if (world->lightSky[index] == world->lightShownSky[index] &&
                world->lightEmber[index] == world->lightShownEmber[index]) {
                continue;
            }
            world->lightShownSky[index] = world->lightSky[index];
            world->lightShownEmber[index] = world->lightEmber[index];

            /* Cells sample the light field bilinearly, so a changed sample
               reaches one light cell in every direction and can therefore fall
               into a neighbouring chunk. */
            for (chunkY = (lightY - 1) / perChunk; chunkY <= (lightY + 1) / perChunk;
                 ++chunkY) {
                for (chunkX = (lightX - 1) / perChunk;
                     chunkX <= (lightX + 1) / perChunk; ++chunkX) {
                    if (chunkX >= 0 && chunkY >= 0 && chunkX < world->chunkColumns &&
                        chunkY < world->chunkRows) {
                        world->dirtyChunks[(size_t)chunkY *
                                               (size_t)world->chunkColumns +
                                           (size_t)chunkX] = 1u;
                    }
                }
            }
        }
    }
}

/* Resolves one axis of the bilinear sample: the two light rows or columns a cell
   falls between, and how far it sits between them. */
static void WorldLightAxis(int samples, int coordinate, int *low, int *high,
                           float *blend)
{
    float position = ((float)coordinate + 0.5f) / (float)WORLD_LIGHT_SCALE - 0.5f;
    int floored = (int)floorf(position);

    *blend = position - (float)floored;
    *low = floored < 0 ? 0 : (floored > samples - 1 ? samples - 1 : floored);
    *high = floored + 1 < 0 ? 0
                            : (floored + 1 > samples - 1 ? samples - 1 : floored + 1);
}

static float WorldSampleLightField(const float *field, int columns, int lowX,
                                   int highX, float blendX, int lowY, int highY,
                                   float blendY)
{
    const float *low = field + (size_t)lowY * (size_t)columns;
    const float *high = field + (size_t)highY * (size_t)columns;
    float top = low[lowX] + (low[highX] - low[lowX]) * blendX;
    float bottom = high[lowX] + (high[highX] - high[lowX]) * blendX;

    return top + (bottom - top) * blendY;
}

/* Turns the two light channels into a multiplier per colour channel. Light that
   is mostly ember rather than sky is warmed, so a lava cavern glows orange
   instead of merely being less dark. */
static void WorldLightTint(float sky, float ember, float *red, float *green,
                           float *blue)
{
    float brightest = ember > sky ? ember : sky;
    float level = WORLD_MINIMUM_LIGHT + (1.0f - WORLD_MINIMUM_LIGHT) * brightest;
    float warmth = ember > sky ? ember - sky : 0.0f;

    /* Plain comparisons rather than fmaxf: this runs for every cell of every
       dirty chunk, and a libm call per pixel is not free at that rate. */
    *red = level * (1.0f + 0.42f * warmth);
    *green = level * (1.0f - 0.06f * warmth);
    *blue = level * (1.0f - 0.44f * warmth);
}

/* Bilinear sample of the coarse field at a cell. Nearest sampling would show the
   light grid as visible blocks. */
float WorldLightAt(const World *world, int x, int y)
{
    int lowX;
    int highX;
    int lowY;
    int highY;
    float blendX;
    float blendY;
    float sky;
    float ember;

    if (world == NULL || world->lightSky == NULL) {
        return 1.0f;
    }

    WorldLightAxis(world->lightColumns, x, &lowX, &highX, &blendX);
    WorldLightAxis(world->lightRows, y, &lowY, &highY, &blendY);
    sky = WorldSampleLightField(world->lightSky, world->lightColumns, lowX, highX,
                                blendX, lowY, highY, blendY);
    ember = WorldSampleLightField(world->lightEmber, world->lightColumns, lowX,
                                  highX, blendX, lowY, highY, blendY);
    return WORLD_MINIMUM_LIGHT + (1.0f - WORLD_MINIMUM_LIGHT) * fmaxf(sky, ember);
}

/* The light only has to be re-solved when its source has actually moved far
   enough to change a sample; a light drifting inside one light cell changes
   nothing the field can represent. */
static bool WorldPointLightMoved(const World *world)
{
    return fabsf(world->pointLight.x - world->solvedPointLight.x) >=
               (float)WORLD_LIGHT_SCALE * 0.5f ||
           fabsf(world->pointLight.y - world->solvedPointLight.y) >=
               (float)WORLD_LIGHT_SCALE * 0.5f ||
           world->pointLightStrength != world->solvedPointLightStrength;
}

void WorldSetPointLight(World *world, Vector2 position, float radius, float strength)
{
    if (world == NULL) {
        return;
    }
    world->pointLight = position;
    world->pointLightRadius = radius;
    world->pointLightStrength = strength;
}

void WorldPrepareVisible(World *world, Rectangle visible,
                         WorldRenderChunkVisitor visitor, void *context)
{
    Color uploadPixels[WORLD_CHUNK_SIZE * WORLD_CHUNK_SIZE];
    int firstVisibleColumn;
    int lastVisibleColumn;
    int firstVisibleRow;
    int lastVisibleRow;
    int chunkY;

    if (world == NULL || world->cells == NULL || world->dirtyChunks == NULL ||
        visitor == NULL) {
        return;
    }

    /* Rebuilding costs what the player can see, not what the world is doing.
       Activity is spread over the whole map — a lava lake, a distant fire, a
       collapsing sand bank — while the camera shows a small window of it, and
       rebuilding a chunk nobody is looking at buys nothing. A skipped chunk
       keeps its dirty flag and is rebuilt on the frame it scrolls into view. */
    firstVisibleColumn = (int)floorf(visible.x / (float)WORLD_CHUNK_SIZE) - 1;
    lastVisibleColumn =
        (int)floorf((visible.x + visible.width) / (float)WORLD_CHUNK_SIZE) + 1;
    firstVisibleRow = (int)floorf(visible.y / (float)WORLD_CHUNK_SIZE) - 1;
    lastVisibleRow =
        (int)floorf((visible.y + visible.height) / (float)WORLD_CHUNK_SIZE) + 1;
    if (firstVisibleColumn < 0) firstVisibleColumn = 0;
    if (firstVisibleRow < 0) firstVisibleRow = 0;
    if (lastVisibleColumn > world->chunkColumns - 1) {
        lastVisibleColumn = world->chunkColumns - 1;
    }
    if (lastVisibleRow > world->chunkRows - 1) {
        lastVisibleRow = world->chunkRows - 1;
    }

    /* Light first: emission and opacity follow the terrain, so they only need
       refreshing where chunks are already dirty, but the solve is global and can
       dirty further chunks that were merely re-lit. Both must finish before any
       pixel is built.

       The solve is the one part of drawing that is not proportional to what
       changed, so it must not run on a world where nothing did. A renderer whose
       whole design is to sleep with the simulation cannot afford a few
       milliseconds of unconditional work every frame. */
    {
        bool sourceMoved = WorldPointLightMoved(world);
        bool terrainChanged = false;

        for (chunkY = 0; chunkY < world->chunkRows; ++chunkY) {
            int chunkX;

            for (chunkX = 0; chunkX < world->chunkColumns; ++chunkX) {
                size_t index = (size_t)chunkY * (size_t)world->chunkColumns +
                               (size_t)chunkX;

                if (world->lightDirtyChunks[index] != 0u) {
                    WorldRefreshLightBlock(world, chunkX, chunkY);
                    world->lightDirtyChunks[index] = 0u;
                    terrainChanged = true;
                }
            }
        }

        /* Terrain only changes on a fixed tick, so re-solving more often than
           the world ticks is pure waste at high frame rates. An effect that
           writes cells between ticks — a laser, a settling particle — waits at
           most one tick to be lit, which is below the threshold of notice. A
           moving light is the exception: it has to track the player smoothly. */
        if (sourceMoved || (terrainChanged && world->tick != world->solvedTick) ||
            !world->lightSolved) {
            WorldSolveLight(world);
            WorldMarkRelitChunks(world);
            world->solvedPointLight = world->pointLight;
            world->solvedPointLightStrength = world->pointLightStrength;
            world->solvedTick = world->tick;
            world->lightSolved = true;
        }
    }

    /* Rebuild only the chunks that changed. The simulation sleeps on a settled
       world, and so must the renderer. */
    for (chunkY = firstVisibleRow; chunkY <= lastVisibleRow; ++chunkY) {
        int chunkX;

        for (chunkX = firstVisibleColumn; chunkX <= lastVisibleColumn; ++chunkX) {
            size_t chunkIndex = (size_t)chunkY * (size_t)world->chunkColumns +
                                (size_t)chunkX;
            int minimumX;
            int maximumX;
            int minimumY;
            int maximumY;
            int columnLow[WORLD_CHUNK_SIZE];
            int columnHigh[WORLD_CHUNK_SIZE];
            float columnBlend[WORLD_CHUNK_SIZE];
            int x;
            int y;

            if (world->dirtyChunks[chunkIndex] == 0u) {
                continue;
            }
            world->dirtyChunks[chunkIndex] = 0u;
            minimumX = chunkX * WORLD_CHUNK_SIZE;
            maximumX = minimumX + WORLD_CHUNK_SIZE;
            minimumY = chunkY * WORLD_CHUNK_SIZE;
            maximumY = minimumY + WORLD_CHUNK_SIZE;
            if (maximumX > world->width) maximumX = world->width;
            if (maximumY > world->height) maximumY = world->height;

            /* Bilinear light weights are the same for every row of the chunk,
               so they are resolved once here instead of per pixel. */
            for (x = minimumX; x < maximumX; ++x) {
                WorldLightAxis(world->lightColumns, x, &columnLow[x - minimumX],
                               &columnHigh[x - minimumX],
                               &columnBlend[x - minimumX]);
            }

            for (y = minimumY; y < maximumY; ++y) {
                int rowLow;
                int rowHigh;
                float rowBlend;
                const float *skyLow;
                const float *skyHigh;
                const float *emberLow;
                const float *emberHigh;

                WorldLightAxis(world->lightRows, y, &rowLow, &rowHigh, &rowBlend);
                skyLow = world->lightSky +
                         (size_t)rowLow * (size_t)world->lightColumns;
                skyHigh = world->lightSky +
                          (size_t)rowHigh * (size_t)world->lightColumns;
                emberLow = world->lightEmber +
                           (size_t)rowLow * (size_t)world->lightColumns;
                emberHigh = world->lightEmber +
                            (size_t)rowHigh * (size_t)world->lightColumns;

                for (x = minimumX; x < maximumX; ++x) {
                    int slot = x - minimumX;
                    int lowX = columnLow[slot];
                    int highX = columnHigh[slot];
                    float blend = columnBlend[slot];
                    float topSky = skyLow[lowX] +
                                   (skyLow[highX] - skyLow[lowX]) * blend;
                    float bottomSky = skyHigh[lowX] +
                                      (skyHigh[highX] - skyHigh[lowX]) * blend;
                    float topEmber = emberLow[lowX] +
                                     (emberLow[highX] - emberLow[lowX]) * blend;
                    float bottomEmber = emberHigh[lowX] +
                                        (emberHigh[highX] - emberHigh[lowX]) * blend;
                    float red;
                    float green;
                    float blue;

                    WorldLightTint(topSky + (bottomSky - topSky) * rowBlend,
                                   topEmber + (bottomEmber - topEmber) * rowBlend,
                                   &red, &green, &blue);
                    Color pixel = MaterialPixel(world, WorldCellConst(world, x, y),
                                                x, y, red, green, blue);

                    uploadPixels[(size_t)(y - minimumY) *
                                     (size_t)(maximumX - minimumX) +
                                 (size_t)(x - minimumX)] = pixel;
                }
            }
            /* At 16384 cells wide, uploading one full-width band for a local
               change moves tens of MiB. A 32x32 stack staging block keeps the
               source contiguous without any frame allocation and uploads only
               the chunk that was rebuilt. */
            visitor(context,
                    (Rectangle){(float)minimumX, (float)minimumY,
                                (float)(maximumX - minimumX),
                                (float)(maximumY - minimumY)},
                    uploadPixels);
        }
    }
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

/* One heavy blow along a cone. Loose material is thrown a long way; solid
   material is not moved but is scoured, a thin layer of its exposed face turning
   to ash, so the blast leaves a visible mark where it landed and then blows the
   dust it just made downwind.
 *
 * Cells are visited from the far edge of the cone inwards so a cell thrown
 * outward cannot be picked up again by the same blow, and `effectStamp` makes
 * that guarantee exact.
 */
/* Angular resolution of the occlusion pre-pass. At the configured cone's far
   edge the arc is about one hundred cells across, so this is still finer than
   one ray per cell there. */
#define FORCE_BLAST_RAYS 160

void WorldApplyForceBlast(World *world, Vector2 origin, Vector2 direction,
                          float length, float spreadCosine, int reach)
{
    float blocked[FORCE_BLAST_RAYS];
    float centreAngle;
    float halfSpread;
    uint32_t stamp;
    int ray;
    int step;

    if (world == NULL || world->cells == NULL || length <= 0.0f || reach <= 0) {
        return;
    }

    /* A blow does not reach round a corner. Without this the cone shoves sand
       on the far side of a rock wall and scours the wall's back face, which
       reads as the blast passing straight through the world. One cheap ray per
       angular slice records where the cone first meets something solid; the cell
       pass then refuses to touch anything further along that slice. */
    centreAngle = atan2f(direction.y, direction.x);
    halfSpread = acosf(Clamp(spreadCosine, -1.0f, 1.0f));
    for (ray = 0; ray < FORCE_BLAST_RAYS; ++ray) {
        float angle = centreAngle - halfSpread +
                      2.0f * halfSpread * ((float)ray / (float)(FORCE_BLAST_RAYS - 1));
        float rayX = cosf(angle);
        float rayY = sinf(angle);
        float travelled;

        blocked[ray] = length;
        for (travelled = 1.0f; travelled <= length; travelled += 0.5f) {
            int sampleX = (int)floorf(origin.x + rayX * travelled);
            int sampleY = (int)floorf(origin.y + rayY * travelled);

            if (WorldMaterialIsSolid(WorldGetCell(world, sampleX, sampleY))) {
                blocked[ray] = travelled;
                break;
            }
        }
    }

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    for (step = (int)length; step >= 1; --step) {
        int extent = step + 1;
        int offsetY;

        for (offsetY = -extent; offsetY <= extent; ++offsetY) {
            int offsetX;

            for (offsetX = -extent; offsetX <= extent; ++offsetX) {
                float dx = (float)offsetX;
                float dy = (float)offsetY;
                float distance = sqrtf(dx * dx + dy * dy);
                int x;
                int y;
                float strength;
                int push;
                Cell *cell;

                /* One shell of the cone per step, so the ring order above holds. */
                if (distance < (float)step - 0.5f || distance >= (float)step + 0.5f) {
                    continue;
                }
                if (dx * direction.x + dy * direction.y < distance * spreadCosine) {
                    continue;
                }

                {
                    /* Which angular slice this cell sits in, and whether the
                       blast still reaches that far along it. The face that
                       blocked the ray is itself included, so the wall the blow
                       lands on is marked. */
                    float offset = atan2f(dy, dx) - centreAngle;
                    int slice;

                    while (offset > PI) offset -= 2.0f * PI;
                    while (offset < -PI) offset += 2.0f * PI;
                    slice = (int)roundf((offset + halfSpread) /
                                        (2.0f * halfSpread) *
                                        (float)(FORCE_BLAST_RAYS - 1));
                    slice = slice < 0 ? 0
                                      : (slice >= FORCE_BLAST_RAYS
                                             ? FORCE_BLAST_RAYS - 1
                                             : slice);
                    if (distance > blocked[slice] + 1.5f) {
                        continue;
                    }
                }

                x = (int)floorf(origin.x) + offsetX;
                y = (int)floorf(origin.y) + offsetY;
                if (!WorldInBounds(world, x, y)) {
                    continue;
                }
                cell = WorldCell(world, x, y);
                if (cell->material == MATERIAL_EMPTY || cell->effectStamp == stamp) {
                    continue;
                }
                strength = 1.0f - distance / length;
                if (strength <= 0.0f) {
                    continue;
                }
                cell->effectStamp = stamp;

                if (!MaterialIsDynamic(cell->material)) {
                    /* Static terrain holds, but the face that took the blow is
                       scoured to dust. Only cells that are actually exposed are
                       marked, so the dent follows the shape of the surface
                       instead of hollowing out the inside of a hill. */
                    int aheadX = x + (int)roundf(direction.x);
                    int aheadY = y + (int)roundf(direction.y);
                    int behindX = x - (int)roundf(direction.x);
                    int behindY = y - (int)roundf(direction.y);

                    if (!WorldMaterialIsSolid(cell->material) ||
                        (WorldMaterialIsSolid(WorldGetCell(world, aheadX, aheadY)) &&
                         WorldMaterialIsSolid(WorldGetCell(world, behindX, behindY)))) {
                        continue;
                    }
                    /* A central hit now has enough bite to leave a visible dent
                       in one or two presses. The exposure check above still
                       limits this to the face, so more power cannot hollow the
                       hill out behind its surface. */
                    if ((float)GetRandomValue(0, 999) < strength * 160.0f) {
                        WorldSetCellRaw(world, x, y, MATERIAL_ASH);
                    }
                    continue;
                }

                /* Linear rather than squared falloff: squaring leaves anything
                   past the first few cells barely moving, which reads as a weak
                   blow however large the numbers are. */
                push = 2 + (int)(strength * (float)reach);
                for (; push >= 1; --push) {
                    int targetX = (int)roundf((float)x + direction.x * (float)push);
                    int targetY = (int)roundf((float)y + direction.y * (float)push);

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

/* How fast the cryo beam pulls each material down, in degrees per second. Lava
   needs by far the most: it relaxes back toward 900C at 8% of the gap per tick,
   so anything gentler is simply undone between frames. */
static float MaterialChillRate(CellMaterial material)
{
    switch (material) {
    case MATERIAL_LAVA:
        /* Lava is pulled back toward 900C at 8% of the gap every tick, which at
           the 620C freezing point is 22 degrees a tick on its own. A beam that
           does not clearly beat that number does not cool lava at all — it just
           finds an equilibrium above the threshold and sits there. */
        return 1900.0f;
    case MATERIAL_FIRE:
        return 2200.0f;
    case MATERIAL_WATER:
        return 90.0f;
    case MATERIAL_STEAM:
        return 320.0f;
    case MATERIAL_EMPTY:
        return 0.0f;
    default:
        return 260.0f;
    }
}

/* The thermal inverse of the laser. Every other power removes matter; this one
   changes its phase, which is the only way the player can put something into the
   world instead of taking it out. It passes through what the laser passes
   through and chills everything on the way, so sweeping a pond freezes its
   surface and holding it on lava turns a lake back into rock. */
LaserResult WorldApplyChill(World *world, Vector2 start, Vector2 end, float radius,
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

    stamp = ++world->effectSerial;
    if (stamp == 0u) {
        stamp = ++world->effectSerial;
    }

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        Vector2 point = {start.x + delta.x * amount, start.y + delta.y * amount};
        int centerX = (int)floorf(point.x);
        int centerY = (int)floorf(point.y);
        int brush = (int)ceilf(radius);
        CellMaterial blocking;
        int y;

        if (!WorldInBounds(world, centerX, centerY)) {
            break;
        }

        for (y = centerY - brush; y <= centerY + brush; ++y) {
            int x;

            for (x = centerX - brush; x <= centerX + brush; ++x) {
                int dx = x - centerX;
                int dy = y - centerY;
                float rate;
                Cell *cell;

                if ((float)(dx * dx + dy * dy) > radius * radius ||
                    !WorldInBounds(world, x, y)) {
                    continue;
                }
                cell = WorldCell(world, x, y);
                if (cell->effectStamp == stamp || cell->material == MATERIAL_EMPTY) {
                    continue;
                }
                cell->effectStamp = stamp;

                rate = MaterialChillRate(cell->material);
                cell->temperature -= deltaTime * rate;
                WorldWakeCellAndNeighbors(world, x, y);
                (void)WorldTryThermalTransition(world, x, y);
            }
        }

        blocking = WorldGetCell(world, centerX, centerY);
        if (WorldMaterialIsSolid(blocking)) {
            result.position = point;
            result.material = blocking;
            result.hit = true;
            break;
        }
    }

    return result;
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
        if (WorldMaterialIsSolid(material)) {
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
                float rate;
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

                rate = MaterialAt(cell->material)->laserHeatRate;
                if (rate > 0.0f) {
                    cell->temperature += deltaTime * rate;
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
