/* What was built here before the player came, and what floats above it.
 *
 * The landscape world_biomes.c makes is a place; these are what make it a
 * place something happened in. On the surface, ruins of what the biome's
 * people built — a keep in the temperate lands, a temple the sand has half
 * buried, a watchtower frozen into the frost, a basalt shrine among the lava
 * fields, colonnades drowned on the sea floor. Underground, the dungeons and
 * mine workings they dug, and sealed crypts in the deep basalt. In the sky,
 * islands of earth that hang over the world with grass and trees on them.
 *
 * Everything is placed on a grid of its own and decided by the seed and the
 * grid cell alone, like every other feature, so adding a ruin never moves a
 * lake. A ruin is ruined deterministically too: which bricks are gone is a
 * hash of where they are, so the same seed crumbles the same keep the same
 * way.
 *
 * The world wraps, and nothing here clamps a column: a ruin across the seam
 * is built into both sides of it.
 */
#include "world_internal.h"

#include <math.h>

#include <raymath.h>

enum StructureChannel {
    STRUCTURE_RUINS = 40,
    STRUCTURE_DECAY = 41,
    STRUCTURE_DUNGEONS = 42,
    STRUCTURE_SHAFTS = 43,
    STRUCTURE_CRYPTS = 44,
    STRUCTURE_ISLANDS = 45,
    STRUCTURE_ISLAND_SHAPE = 46,
};

#define RUIN_SPACING 700
#define DUNGEON_SPACING 1700
#define SHAFT_SPACING 2300
#define CRYPT_SPACING 2600
#define ISLAND_SPACING 1500

static int StructureModulo(int value, int modulus)
{
    int result = value % modulus;

    return result < 0 ? result + modulus : result;
}

static void StructureSet(World *world, int x, int y, CellMaterial material)
{
    if (WorldInBounds(world, x, y)) {
        WorldSetGeneratedCell(world, x, y, material);
    }
}

static void StructureFill(World *world, int firstX, int firstY, int lastX, int lastY,
                          CellMaterial material)
{
    int y;

    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
            StructureSet(world, x, y, material);
        }
    }
}

/* A cell of masonry that time has taken: more of them toward the top of a
   wall and at its edges, none at all in a foundation. `height` is 0 at the
   foot of the wall and 1 at its top. */
static bool StructureDecayed(const World *world, int x, int y, float height,
                             float wear)
{
    float roll = WorldGenUnit(world->seed, x, y, STRUCTURE_DECAY);
    float chunk = WorldGenUnit(world->seed, x / 5, y / 4, STRUCTURE_DECAY + 1u);

    /* A few bricks knocked out anywhere, more toward the top; whole pieces
       gone only from the upper part of a wall, which is the part that
       falls. The lower walls of a ruin still stand. */
    return roll < wear * (0.02f + 0.14f * height * height) ||
           (height > 0.55f && chunk < wear * (height - 0.55f) * 1.6f);
}

/* Masonry, worn by `wear`: the cell is laid unless decay took it. */
static void StructureWall(World *world, int firstX, int firstY, int lastX, int lastY,
                          CellMaterial material, int baseY, int topY, float wear)
{
    int y;

    for (y = firstY; y <= lastY; ++y) {
        int x;
        float height = topY < baseY ? (float)(baseY - y) / (float)(baseY - topY) : 0.0f;

        if (height < 0.0f) height = 0.0f;
        for (x = firstX; x <= lastX; ++x) {
            if (StructureDecayed(world, x, y, height, wear)) {
                continue;
            }
            StructureSet(world, x, y, material);
        }
    }
}

/* The ground a footprint stands on: the lowest top across it, and how far
   the highest top is above that. A ruin needs a floor it can be levelled
   onto without carving half a hill away. */
static int StructureGround(const World *world, int firstX, int lastX, int *relief,
                           bool *wet)
{
    int lowest = -1;
    int highest = world->height;
    int x;

    *wet = false;
    for (x = firstX; x <= lastX; ++x) {
        int top = WorldGenSolidY(world, x);
        int y;

        if (top < 0) continue;
        /* Water standing on the ground is not ground to build on. */
        for (y = top - 1; y >= top - 3 && y >= 0; --y) {
            if (MaterialIsLiquid(WorldMaterialAt(world, x, y))) {
                *wet = true;
            }
        }
        if (MaterialIsLiquid(WorldMaterialAt(world, x, top))) {
            *wet = true;
        }
        if (top > lowest) lowest = top;
        if (top < highest) highest = top;
    }
    *relief = lowest - highest;
    return lowest;
}

/* Levels a footprint: everything above the floor inside it cleared, a
   foundation laid under it down to solid ground. */
static void StructureLevel(World *world, int firstX, int lastX, int floorY, int clearTo,
                           CellMaterial foundation)
{
    int x;

    for (x = firstX; x <= lastX; ++x) {
        int y;

        for (y = clearTo; y < floorY; ++y) {
            StructureSet(world, x, y, MATERIAL_EMPTY);
        }
        for (y = floorY; y < floorY + 6; ++y) {
            StructureSet(world, x, y, foundation);
        }
    }
}

/* Clears every cell of masonry or timber in the box that touches nothing:
   decay takes bricks at random, and a brick whose neighbours all went is a
   speck hanging in the air — which a player notices, and which the first
   blast nearby would tear off as a body of one cell. */
static void StructureSweep(World *world, int firstX, int firstY, int lastX, int lastY)
{
    int y;

    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
            CellMaterial material = WorldMaterialAt(world, x, y);
            bool joined = false;
            int offsetY;

            if (!WorldInBounds(world, x, y) ||
                (material != MATERIAL_BRICK && material != MATERIAL_WOOD &&
                 material != MATERIAL_BASALT && material != MATERIAL_CRYSTAL)) {
                continue;
            }
            for (offsetY = -1; offsetY <= 1 && !joined; ++offsetY) {
                int offsetX;

                for (offsetX = -1; offsetX <= 1; ++offsetX) {
                    if ((offsetX != 0 || offsetY != 0) &&
                        WorldInBounds(world, x + offsetX, y + offsetY) &&
                        WorldMaterialAt(world, x + offsetX, y + offsetY) !=
                            MATERIAL_EMPTY) {
                        joined = true;
                        break;
                    }
                }
            }
            if (!joined) {
                StructureSet(world, x, y, MATERIAL_EMPTY);
            }
        }
    }
}

/* A heap of what fell, beside a ruin. */
static void StructureRubble(World *world, int centerX, int groundY, int halfWidth,
                            int height)
{
    int x;

    for (x = centerX - halfWidth; x <= centerX + halfWidth; ++x) {
        float t = (float)(x - centerX) / (float)(halfWidth + 1);
        int pile = (int)((float)height * (1.0f - t * t));
        int y;

        for (y = groundY - 1; y >= groundY - pile; --y) {
            if (WorldMaterialAt(world, x, y) != MATERIAL_EMPTY) break;
            StructureSet(world, x, y, MATERIAL_RUBBLE);
        }
    }
}

/* A small brazier: a bowl of masonry with lava in it. What lights a ruin at
   night, and what the player can tip over. */
static void StructureBrazier(World *world, int x, int floorY, CellMaterial material)
{
    StructureFill(world, x - 2, floorY - 1, x + 2, floorY - 1, material);
    StructureSet(world, x - 2, floorY - 2, material);
    StructureSet(world, x + 2, floorY - 2, material);
    StructureFill(world, x - 1, floorY - 2, x + 1, floorY - 2, MATERIAL_LAVA);
}

/* ---- surface ruins --------------------------------------------------------- */

/* A keep: a hollow tower, two cells of wall, floors of timber every twelve
   rows with a gap for the stair, windows, a door, battlements — and then a
   century of weather on all of it. */
static void RuinTower(World *world, Rng *rng, int centerX, CellMaterial material,
                      bool brazier)
{
    int half = RngRange(rng, 8, 12);
    int height = RngRange(rng, 40, 80);
    int firstX = centerX - half;
    int lastX = centerX + half;
    int relief;
    bool wet;
    int floorY = StructureGround(world, firstX - 2, lastX + 2, &relief, &wet);
    int topY;
    int y;
    int x;

    if (floorY < 0 || relief > 22 || wet) return;
    topY = floorY - height;
    StructureLevel(world, firstX - 2, lastX + 2, floorY, floorY - relief - 2, material);
    /* The walls, worn from the top down, each column losing its own height. */
    for (x = firstX; x <= lastX; ++x) {
        bool wall = x <= firstX + 3 || x >= lastX - 3;
        int columnTop = topY + (int)(WorldGenUnit(world->seed, x / 3, floorY,
                                                  STRUCTURE_DECAY + 2u) *
                                     (float)height * 0.3f);

        if (!wall) continue;
        StructureWall(world, x, columnTop, x, floorY - 1, material, floorY, topY, 0.9f);
        /* Battlements where the top survived. */
        if (columnTop == topY && (x & 1) == 0) {
            StructureSet(world, x, topY - 1, material);
        }
    }
    /* Floors, with the stair gap on alternating sides. */
    for (y = floorY - 12; y > topY + 6; y -= 12) {
        int gapLeft = ((floorY - y) / 12) % 2 == 0;

        for (x = firstX + 4; x <= lastX - 4; ++x) {
            bool gap = gapLeft ? x < firstX + 8 : x > lastX - 8;

            if (!gap && !StructureDecayed(world, x, y, (float)(floorY - y) /
                                                         (float)height, 0.6f)) {
                StructureFill(world, x, y, x, y + 1, MATERIAL_WOOD);
            }
        }
        /* A window in one wall every other floor, the other wall the next. */
        if (((floorY - y) / 12) % 2 == 1) {
            if (gapLeft) {
                StructureFill(world, lastX - 3, y - 7, lastX, y - 4, MATERIAL_EMPTY);
            } else {
                StructureFill(world, firstX, y - 7, firstX + 3, y - 4, MATERIAL_EMPTY);
            }
        }
    }
    /* The door. */
    if (RngRange(rng, 0, 1) != 0) {
        StructureFill(world, firstX, floorY - 7, firstX + 3, floorY - 1, MATERIAL_EMPTY);
    } else {
        StructureFill(world, lastX - 3, floorY - 7, lastX, floorY - 1, MATERIAL_EMPTY);
    }
    if (brazier) {
        StructureBrazier(world, centerX, floorY, material);
    }
    StructureRubble(world, lastX + RngRange(rng, 4, 9), floorY, RngRange(rng, 4, 8),
                    RngRange(rng, 3, 6));
    StructureSweep(world, firstX - 2, topY - 2, lastX + 2, floorY);
}

/* A hall: long low walls, a door either end, the roof beams mostly fallen in,
   one end of it collapsed. */
static void RuinHall(World *world, Rng *rng, int centerX, CellMaterial material)
{
    int half = RngRange(rng, 16, 28);
    int height = RngRange(rng, 14, 22);
    int firstX = centerX - half;
    int lastX = centerX + half;
    int relief;
    bool wet;
    int floorY = StructureGround(world, firstX - 1, lastX + 1, &relief, &wet);
    int topY;
    int x;

    if (floorY < 0 || relief > 14 || wet) return;
    topY = floorY - height;
    StructureLevel(world, firstX - 1, lastX + 1, floorY, floorY - relief - 2, material);
    StructureWall(world, firstX, topY, firstX + 2, floorY - 1, material, floorY, topY, 0.7f);
    StructureWall(world, lastX - 2, topY, lastX, floorY - 1, material, floorY, topY, 1.0f);
    /* Inner pillars carrying the roof. */
    for (x = firstX + 10; x < lastX - 8; x += 10) {
        StructureWall(world, x, topY + 1, x + 2, floorY - 1, material, floorY, topY, 0.6f);
    }
    /* The roof: beams, most of them gone. */
    for (x = firstX; x <= lastX; ++x) {
        if (WorldGenUnit(world->seed, x / 3, topY, STRUCTURE_DECAY + 3u) < 0.6f) {
            StructureFill(world, x, topY - 1, x, topY, MATERIAL_WOOD);
        }
    }
    /* Doors. */
    StructureFill(world, firstX, floorY - 7, firstX + 2, floorY - 1, MATERIAL_EMPTY);
    StructureFill(world, lastX - 2, floorY - 7, lastX, floorY - 1, MATERIAL_EMPTY);
    StructureRubble(world, lastX - 5, floorY, 6, RngRange(rng, 3, 5));
    StructureSweep(world, firstX - 1, topY - 1, lastX + 1, floorY);
}

/* A colonnade: a row of pillars under a broken architrave. */
static void RuinColonnade(World *world, Rng *rng, int centerX, CellMaterial material,
                          bool underwater)
{
    int count = RngRange(rng, 3, 7);
    int spacing = RngRange(rng, 10, 15);
    int height = RngRange(rng, 18, 30);
    int firstX = centerX - count * spacing / 2;
    int lastX = firstX + (count - 1) * spacing + 3;
    int relief;
    bool wet;
    int floorY = StructureGround(world, firstX, lastX, &relief, &wet);
    int pillar;
    int topY;

    if (floorY < 0 || relief > 16 || (wet && !underwater)) return;
    topY = floorY - height;
    StructureFill(world, firstX - 1, floorY, lastX + 1, floorY + 1, material);
    for (pillar = 0; pillar < count; ++pillar) {
        int x = firstX + pillar * spacing;
        int broken = (int)(WorldGenUnit(world->seed, x, floorY, STRUCTURE_DECAY + 4u) *
                           (float)height * 0.8f);

        /* A fallen pillar is a stump. */
        if (broken > height / 2) {
            StructureWall(world, x, floorY - height / 3, x + 3, floorY - 1, material,
                          floorY, topY, 0.4f);
            continue;
        }
        StructureWall(world, x, topY, x + 3, floorY - 1, material, floorY, topY, 0.3f);
    }
    /* The architrave, only over the pillars that still reach it. */
    StructureWall(world, firstX, topY - 3, lastX, topY - 1, material, floorY, topY - 3, 1.0f);
    StructureSweep(world, firstX - 1, topY - 3, lastX + 1, floorY);
}

/* A temple the dunes have half swallowed: a stepped pyramid with a passage
   into a chamber, and sand drifted up both flanks. */
static void RuinTemple(World *world, Rng *rng, int centerX)
{
    int steps = RngRange(rng, 4, 6);
    int stepHeight = 7;
    int base = steps * 8 + 8;
    int relief;
    bool wet;
    int floorY = StructureGround(world, centerX - base, centerX + base, &relief, &wet);
    int step;
    int chamberY;
    int x;

    if (floorY < 0 || relief > 30 || wet) return;
    for (step = 0; step < steps; ++step) {
        int half = base - step * 8;
        int bottom = floorY - step * stepHeight;

        StructureWall(world, centerX - half, bottom - stepHeight + 1, centerX + half, bottom,
                      MATERIAL_BRICK, floorY, floorY - steps * stepHeight, 0.35f);
    }
    /* The chamber and the passage to it. */
    chamberY = floorY - stepHeight;
    StructureFill(world, centerX - 8, chamberY - 7, centerX + 8, chamberY - 1,
                  MATERIAL_EMPTY);
    StructureFill(world, centerX - base, chamberY - 4, centerX - 8, chamberY - 1,
                  MATERIAL_EMPTY);
    StructureBrazier(world, centerX, chamberY, MATERIAL_BRICK);
    /* A crystal on the summit, still burning. */
    StructureFill(world, centerX - 1, floorY - steps * stepHeight - 3, centerX + 1,
                  floorY - steps * stepHeight, MATERIAL_CRYSTAL);
    /* Drifts: wedges of sand up the flanks. */
    for (x = 0; x <= base + 10; ++x) {
        int drift = (int)((float)(steps * stepHeight) * 0.6f *
                          (1.0f - (float)x / (float)(base + 10)));
        int side;

        for (side = -1; side <= 1; side += 2) {
            int column = centerX + side * (base - x + 10);
            int y;

            for (y = floorY - 1; y >= floorY - drift; --y) {
                if (WorldMaterialAt(world, column, y) != MATERIAL_EMPTY) continue;
                StructureSet(world, column, y, MATERIAL_SAND);
            }
        }
    }
    StructureSweep(world, centerX - base - 12, floorY - steps * stepHeight - 4,
                   centerX + base + 12, floorY);
    (void)rng;
}

/* An obelisk with a crystal at its point. */
static void RuinObelisk(World *world, Rng *rng, int centerX, CellMaterial material)
{
    int height = RngRange(rng, 22, 42);
    int relief;
    bool wet;
    int floorY = StructureGround(world, centerX - 3, centerX + 3, &relief, &wet);
    int y;

    if (floorY < 0 || relief > 8 || wet) return;
    StructureFill(world, centerX - 3, floorY - 2, centerX + 3, floorY + 2, material);
    for (y = floorY - 3; y > floorY - height; --y) {
        int half = y > floorY - height + 6 ? 1 : 0;

        StructureFill(world, centerX - half - 1, y, centerX + half + 1, y, material);
    }
    StructureFill(world, centerX - 1, floorY - height - 3, centerX + 1, floorY - height,
                  MATERIAL_CRYSTAL);
}

void WorldGenerateRuins(World *world)
{
    int count;
    int feature;

    if (world->height < 400 || world->width < 512) return;
    count = (world->width + RUIN_SPACING - 1) / RUIN_SPACING;
    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_RUINS);
        int centerX = StructureModulo(feature * RUIN_SPACING + RUIN_SPACING / 2 +
                                          RngRange(&rng, -220, 220),
                                      world->width);
        int roll = RngRange(&rng, 0, 99);

        if (WorldGenNearSpawn(world, centerX) || roll >= 62) continue;
        switch (WorldBiomeAt(world, centerX)) {
        case WORLD_BIOME_TEMPERATE:
            if (roll < 25) {
                RuinTower(world, &rng, centerX, MATERIAL_BRICK, false);
            } else if (roll < 48) {
                RuinHall(world, &rng, centerX, MATERIAL_BRICK);
            } else {
                RuinColonnade(world, &rng, centerX, MATERIAL_BRICK, false);
            }
            break;
        case WORLD_BIOME_DUNES:
            if (roll < 40) {
                RuinTemple(world, &rng, centerX);
            } else {
                RuinObelisk(world, &rng, centerX, MATERIAL_BRICK);
            }
            break;
        case WORLD_BIOME_FROST:
            RuinTower(world, &rng, centerX, MATERIAL_BRICK, false);
            break;
        case WORLD_BIOME_VOLCANIC:
            if (roll < 35) {
                RuinTower(world, &rng, centerX, MATERIAL_BASALT, true);
            } else {
                RuinObelisk(world, &rng, centerX, MATERIAL_BASALT);
            }
            break;
        case WORLD_BIOME_OCEAN:
            /* On the sea floor, before the sea is poured over it. */
            RuinColonnade(world, &rng, centerX, MATERIAL_BRICK, true);
            break;
        case WORLD_BIOME_COUNT:
            break;
        }
    }
}

/* ---- underground ------------------------------------------------------------- */

/* A room: masonry walls two thick around an empty inside, floor and ceiling
   included, with a pillar or two and something that gives light. */
static void DungeonRoom(World *world, Rng *rng, int left, int top, int width, int height)
{
    int right = left + width;
    int bottom = top + height;
    int x;

    StructureWall(world, left - 2, top - 2, right + 2, bottom + 2, MATERIAL_BRICK,
                  bottom, top - 2, 0.15f);
    StructureFill(world, left, top, right, bottom, MATERIAL_EMPTY);
    for (x = left + 7; x < right - 5; x += RngRange(rng, 7, 12)) {
        if (RngRange(rng, 0, 99) < 40) {
            StructureFill(world, x, top, x + 1, bottom, MATERIAL_BRICK);
        }
    }
    if (RngRange(rng, 0, 99) < 60) {
        StructureBrazier(world, left + width / 2, bottom + 1, MATERIAL_BRICK);
    } else {
        StructureFill(world, left + width / 2, top, left + width / 2 + 1, top + 1,
                      MATERIAL_CRYSTAL);
    }
    StructureSweep(world, left - 3, top - 3, right + 3, bottom + 3);
    /* Part of the room has come in. */
    if (RngRange(rng, 0, 99) < 35) {
        int fall = left + RngRange(rng, 2, width - 8);

        StructureRubble(world, fall + 3, bottom + 1, RngRange(rng, 3, 6),
                        RngRange(rng, 3, height - 2));
    }
}

/* A corridor, level then vertical, from one room to the next. Laid in two
   passes: the lined passage before the rooms are built, and its bore again
   after, which is what opens the doorways through the rooms' walls without
   running a line of brick across the inside of a hall. */
static void DungeonCorridor(World *world, int fromX, int fromY, int toX, int toY,
                            bool lined)
{
    int step = fromX < toX ? 1 : -1;
    int x;
    int y;

    for (x = fromX; x != toX + step; x += step) {
        if (lined) {
            StructureFill(world, x, fromY - 6, x, fromY - 5, MATERIAL_BRICK);
            StructureFill(world, x, fromY + 1, x, fromY + 2, MATERIAL_BRICK);
        }
        StructureFill(world, x, fromY - 4, x, fromY, MATERIAL_EMPTY);
    }
    step = fromY < toY ? 1 : -1;
    for (y = fromY; y != toY + step; y += step) {
        if (lined) {
            StructureFill(world, toX - 4, y, toX - 3, y, MATERIAL_BRICK);
            StructureFill(world, toX + 3, y, toX + 4, y, MATERIAL_BRICK);
        }
        StructureFill(world, toX - 2, y, toX + 2, y, MATERIAL_EMPTY);
        /* Landings every so often, so it can be climbed down on foot. */
        if (!lined && y % 10 == 0) {
            StructureFill(world, toX - 2, y, toX - 1, y, MATERIAL_WOOD);
        }
    }
}

#define DUNGEON_MAX_ROOMS 6

typedef struct DungeonRoomPlan {
    int left;
    int top;
    int width;
    int height;
} DungeonRoomPlan;

static void GenerateDungeons(World *world)
{
    int count = (world->width + DUNGEON_SPACING - 1) / DUNGEON_SPACING;
    int feature;

    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_DUNGEONS);
        int x = StructureModulo(feature * DUNGEON_SPACING + RngRange(&rng, 0, DUNGEON_SPACING - 1),
                                world->width);
        int surface = WorldGenSurfaceY(world, x);
        int top = surface + 70;
        int bottom = (int)WorldGroundY(world, 0.78f);
        int rooms = RngRange(&rng, 3, DUNGEON_MAX_ROOMS);
        DungeonRoomPlan plan[DUNGEON_MAX_ROOMS];
        bool entrance;
        int room;
        int pass;
        int y;

        if (RngRange(&rng, 0, 99) >= 70 || WorldGenNearSpawn(world, x) ||
            WorldBiomeAt(world, x) == WORLD_BIOME_OCEAN || top + 40 >= bottom) {
            continue;
        }
        y = RngRange(&rng, top, bottom - 30);
        for (room = 0; room < rooms; ++room) {
            plan[room].width = RngRange(&rng, 18, 36);
            plan[room].height = RngRange(&rng, 11, 17);
            plan[room].left = x - plan[room].width / 2;
            plan[room].top = y;
            x += (RngRange(&rng, 0, 1) != 0 ? 1 : -1) * RngRange(&rng, 45, 75);
            y += RngRange(&rng, -14, 26);
            if (y > bottom - 30) y = bottom - 30;
        }
        entrance = RngRange(&rng, 0, 99) < 55;

        /* Corridors lined, rooms over them, corridors bored again. */
        for (pass = 0; pass < 2; ++pass) {
            for (room = 1; room < rooms; ++room) {
                const DungeonRoomPlan *from = &plan[room - 1];
                const DungeonRoomPlan *to = &plan[room];

                DungeonCorridor(world, from->left + from->width / 2, from->top + from->height,
                                to->left + to->width / 2, to->top + to->height, pass == 0);
            }
            if (entrance) {
                const DungeonRoomPlan *last = &plan[rooms - 1];
                int stairX = last->left + last->width / 2;

                DungeonCorridor(world, stairX, last->top + last->height, stairX,
                                WorldGenSurfaceY(world, stairX) - 2, pass == 0);
            }
            if (pass == 0) {
                for (room = 0; room < rooms; ++room) {
                    DungeonRoom(world, &rng, plan[room].left, plan[room].top,
                                plan[room].width, plan[room].height);
                }
            }
        }
    }
}

/* A mine: a shaft timbered down its walls, landings on alternate sides, and
   galleries driven off it, each framed with posts and a lintel. */
static void GenerateShafts(World *world)
{
    int count = (world->width + SHAFT_SPACING - 1) / SHAFT_SPACING;
    int feature;

    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_SHAFTS);
        int x = StructureModulo(feature * SHAFT_SPACING + RngRange(&rng, 0, SHAFT_SPACING - 1),
                                world->width);
        int top = WorldGenSurfaceY(world, x) - 1;
        int depth = RngRange(&rng, 140, 300);
        int y;

        if (RngRange(&rng, 0, 99) >= 65 || WorldGenNearSpawn(world, x) ||
            WorldBiomeAt(world, x) == WORLD_BIOME_OCEAN ||
            top + depth > (int)WorldGroundY(world, 0.82f)) {
            continue;
        }
        for (y = top; y < top + depth; ++y) {
            StructureFill(world, x - 3, y, x + 3, y, MATERIAL_EMPTY);
            StructureSet(world, x - 3, y, MATERIAL_WOOD);
            StructureSet(world, x + 3, y, MATERIAL_WOOD);
            if ((y - top) % 20 == 19) {
                bool left = ((y - top) / 20) % 2 == 0;

                StructureFill(world, left ? x - 2 : x, y, left ? x : x + 2, y,
                              MATERIAL_WOOD);
            }
            /* A gallery every forty rows, either way. */
            if ((y - top) % 40 == 39 && y > top + 30) {
                int direction = RngRange(&rng, 0, 1) != 0 ? 1 : -1;
                int length = RngRange(&rng, 30, 90);
                int step;

                /* Seen from the side a gallery's timbering is its ceiling
                   beams and its floor planks: a post across the passage would
                   be a wall. */
                for (step = 4; step < length; ++step) {
                    int column = x + direction * step;

                    StructureFill(world, column, y - 4, column, y, MATERIAL_EMPTY);
                    StructureSet(world, column, y + 1, MATERIAL_WOOD);
                    if (step % 8 < 3) {
                        StructureSet(world, column, y - 5, MATERIAL_WOOD);
                    }
                }
            }
        }
    }
}

/* A crypt, sealed in the deep basalt with nothing leading to it: a vault
   under a row of pillars, lit by crystal, for whoever digs that far. */
static void GenerateCrypts(World *world)
{
    int count = (world->width + CRYPT_SPACING - 1) / CRYPT_SPACING;
    int feature;

    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_CRYPTS);
        int x = StructureModulo(feature * CRYPT_SPACING + RngRange(&rng, 0, CRYPT_SPACING - 1),
                                world->width);
        int top = (int)WorldGroundY(world, 0.84f);
        int bottom = (int)WorldGroundY(world, 0.94f);
        int width = RngRange(&rng, 30, 56);
        int height = RngRange(&rng, 12, 18);
        int y;
        int column;

        if (RngRange(&rng, 0, 99) >= 60 || top >= bottom) continue;
        y = RngRange(&rng, top, bottom);
        StructureFill(world, x - width / 2 - 3, y - 3, x + width / 2 + 3, y + height + 3,
                      MATERIAL_BRICK);
        StructureFill(world, x - width / 2, y, x + width / 2, y + height, MATERIAL_EMPTY);
        for (column = x - width / 2 + 5; column < x + width / 2 - 3; column += 7) {
            StructureFill(world, column, y, column + 1, y + height, MATERIAL_BRICK);
            StructureFill(world, column, y, column + 1, y, MATERIAL_CRYSTAL);
        }
        StructureBrazier(world, x, y + height + 1, MATERIAL_BRICK);
    }
}

void WorldGenerateUnderground(World *world)
{
    if (world->height < 400 || world->width < 512) return;
    GenerateDungeons(world);
    GenerateShafts(world);
    GenerateCrypts(world);
}

/* ---- islands in the sky ---------------------------------------------------- */

/* An island: a flat top of soil under grass, gently domed, and an underside
   of rock that hangs down in a rough point, as if it were torn out of the
   ground below and never fell. Wider than the detach search window, so it
   is not a thing that comes loose; the few small rocks drifting beside it
   are, and fall when they are hit. */
static void PlaceIsland(World *world, Rng *rng, int centerX, int baseY, int halfWidth,
                        int depth)
{
    int x;

    for (x = -halfWidth; x <= halfWidth; ++x) {
        float t = (float)x / (float)halfWidth;
        float body = powf(fmaxf(0.0f, 1.0f - t * t), 0.7f);
        float rough = WorldGenUnit(world->seed, (centerX + x) / 3, baseY,
                                   STRUCTURE_ISLAND_SHAPE);
        int top = baseY - (int)(4.0f * body + 2.0f * rough * body);
        int bottom = baseY + (int)((float)depth * body * (0.7f + 0.3f * rough));
        int soil = 4 + (int)(4.0f * body);
        int y;

        if (body <= 0.02f) continue;
        for (y = top; y <= bottom; ++y) {
            StructureSet(world, centerX + x, y,
                         y < top + soil ? MATERIAL_DIRT : MATERIAL_ROCK);
        }
    }
    /* Trees, away from the edges, before the grass: a tree needs clear
       ground either side of its trunk. */
    for (x = -halfWidth + 20; x < halfWidth - 20; x += RngRange(rng, 14, 32)) {
        int top = WorldGenSolidY(world, centerX + x);

        if (top > 0 && top < baseY + 4) {
            WorldGenPlaceTree(world, centerX + x, top, rng);
        }
    }
    /* Grass on every piece of the top still open to the sky. */
    for (x = -halfWidth; x <= halfWidth; ++x) {
        int top = WorldGenSolidY(world, centerX + x);

        if (top > 0 && top < baseY + 12 &&
            WorldMaterialAt(world, centerX + x, top) == MATERIAL_DIRT &&
            WorldMaterialAt(world, centerX + x, top - 1) == MATERIAL_EMPTY) {
            StructureSet(world, centerX + x, top - 1, MATERIAL_GRASS);
        }
    }
    /* A shrine on some: four pillars and a lintel, a crystal at its heart. */
    if (RngRange(rng, 0, 99) < 45) {
        int shrineX = centerX + RngRange(rng, -halfWidth / 3, halfWidth / 3);
        int ground = WorldGenSolidY(world, shrineX);
        int pillar;

        if (ground > 0) {
            if (WorldMaterialAt(world, shrineX, ground) == MATERIAL_GRASS) ++ground;
            for (pillar = -2; pillar <= 1; ++pillar) {
                int column = shrineX + pillar * 5;

                StructureFill(world, column, ground - 12, column + 1, ground - 1,
                              MATERIAL_BRICK);
            }
            StructureWall(world, shrineX - 11, ground - 14, shrineX + 7, ground - 13,
                          MATERIAL_BRICK, ground, ground - 14, 0.6f);
            StructureFill(world, shrineX - 3, ground - 3, shrineX - 2, ground - 1,
                          MATERIAL_CRYSTAL);
            StructureSweep(world, shrineX - 12, ground - 15, shrineX + 8, ground);
        }
    }
    /* Rocks drifting beside it: small enough to fall when they are hit. */
    for (x = 0; x < 4; ++x) {
        int side = RngRange(rng, 0, 1) != 0 ? 1 : -1;
        int rockX = centerX + side * (halfWidth + RngRange(rng, 8, 40));
        int rockY = baseY + RngRange(rng, -30, depth);
        int radius = RngRange(rng, 2, 6);
        int oy;

        for (oy = -radius; oy <= radius; ++oy) {
            int ox;

            for (ox = -radius - 1; ox <= radius + 1; ++ox) {
                if (ox * ox + oy * oy * 2 <= radius * radius) {
                    StructureSet(world, rockX + ox, rockY + oy, MATERIAL_ROCK);
                }
            }
        }
    }
}

void WorldGenerateIslands(World *world)
{
    int count;
    int feature;

    if (world->height < 1600 || world->width < 1024) return;
    count = (world->width + ISLAND_SPACING - 1) / ISLAND_SPACING;
    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_ISLANDS);
        int centerX = StructureModulo(feature * ISLAND_SPACING + ISLAND_SPACING / 2 +
                                          RngRange(&rng, -400, 400),
                                      world->width);
        int halfWidth = RngRange(&rng, 100, 170);
        int depth = RngRange(&rng, 40, 90);
        int highest = world->height;
        int x;
        int baseY;

        if (RngRange(&rng, 0, 99) >= 70 || WorldGenNearSpawn(world, centerX)) continue;
        /* Clear of whatever is below it, peaks included, by a good margin. */
        for (x = centerX - halfWidth - 60; x <= centerX + halfWidth + 60; x += 8) {
            int top = WorldGenSolidY(world, x);

            if (top >= 0 && top < highest) highest = top;
        }
        baseY = highest - depth - RngRange(&rng, 160, 360);
        if (baseY < (int)WorldGroundY(world, 0.0f) + 40) {
            baseY = (int)WorldGroundY(world, 0.0f) + 40;
        }
        if (baseY + depth > highest - 80) continue;
        PlaceIsland(world, &rng, centerX, baseY, halfWidth, depth);
    }
}
