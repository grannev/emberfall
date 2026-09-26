/* What was built here before the player came, and what floats above it.
 *
 * The landscape world_biomes.c makes is a place; these are what make it a
 * place something happened in. Two peoples left their mark. The precursors,
 * long gone, cut pale relic stone into great gateways, stepped terraces and
 * obelisks crowned with lamps that still burn, and sealed vaults and
 * reliquaries deep in the rock. The explorers who came after them, and did
 * not stay, left metal: ships broken open where they came down, outposts on
 * stilts with their antennas still lit, mine workings driven down through the
 * rock with grates and lamps along the shafts. Above everything, in space,
 * islands of earth hang where nothing pulls them down.
 *
 * Everything is built on the scale of the landscape, not of the character:
 * he is sixteen cells tall, a doorway is thirty-four rows, a corridor
 * forty-four, a hall a hundred, a gateway two hundred — places he walks
 * into, not boxes that just fit him. A step is never more than a jump.
 *
 * Everything is placed on a grid of its own and decided by the seed and the
 * grid cell alone, like every other feature, so adding a ruin never moves a
 * lake. A ruin is ruined deterministically too: which blocks are gone is a
 * hash of where they are, so the same seed crumbles the same gate the same
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

#define RUIN_SPACING 1100
#define DUNGEON_SPACING 2600
#define SHAFT_SPACING 3000
#define CRYPT_SPACING 3000
#define ISLAND_SPACING 1500

/* What a character needs to pass: a door this tall, a corridor this tall. */
#define STRUCTURE_DOOR_ROWS 34
#define STRUCTURE_CORRIDOR_ROWS 44

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

/* A filled disc: an orb, a lamp, a boulder of scrap. */
static void StructureDisc(World *world, int centerX, int centerY, int radius,
                          CellMaterial material)
{
    int y;

    for (y = -radius; y <= radius; ++y) {
        int x;

        for (x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius + radius) {
                StructureSet(world, centerX + x, centerY + y, material);
            }
        }
    }
}

/* A block of stone that time has taken: more of them toward the top of a
   wall and at its edges, none at all in a foundation. `height` is 0 at the
   foot of the wall and 1 at its top. */
static bool StructureDecayed(const World *world, int x, int y, float height,
                             float wear)
{
    float roll = WorldGenUnit(world->seed, x, y, STRUCTURE_DECAY);
    float chunk = WorldGenUnit(world->seed, x / 8, y / 6, STRUCTURE_DECAY + 1u);

    /* A few cells knocked out anywhere, more toward the top; whole pieces
       gone only from the upper part of a wall, which is the part that
       falls. The lower walls of a ruin still stand. */
    return roll < wear * (0.015f + 0.1f * height * height) ||
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
        for (y = floorY; y < floorY + 8; ++y) {
            StructureSet(world, x, y, foundation);
        }
    }
}

/* Clears every built cell in the box that touches nothing: decay takes
   blocks at random, and a block whose neighbours all went is a speck
   hanging in the air — which a player notices, and which the first blast
   nearby would tear off as a body of one cell. */
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
                 material != MATERIAL_BASALT && material != MATERIAL_CRYSTAL &&
                 material != MATERIAL_METAL && material != MATERIAL_LUMEN &&
                 material != MATERIAL_RELIC && material != MATERIAL_GIRDER &&
                 material != MATERIAL_PILLAR && material != MATERIAL_PLANK)) {
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

/* How much of a box is solid ground, sampled on a coarse grid: a chamber
   built into a cavern that is already there would be a box of stone hanging
   in the air. */
static float StructureSolidShare(const World *world, int firstX, int firstY, int lastX,
                                 int lastY)
{
    int solid = 0;
    int samples = 0;
    int y;

    for (y = firstY; y <= lastY; y += 4) {
        int x;

        for (x = firstX; x <= lastX; x += 4) {
            ++samples;
            if (MaterialIsSolid(WorldMaterialAt(world, x, y))) ++solid;
        }
    }
    return samples > 0 ? (float)solid / (float)samples : 0.0f;
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

/* ---- what fills a place --------------------------------------------------

   A building with nothing in it is a box. These are the things people left in
   theirs, drawn from a few cells each. Almost all of them are backdrop —
   planks, girders, carved stone that stands behind the character — so a
   furnished room is still a room the character can walk across. */

/* A line of `thickness` cells from (fromX, fromY) to (toX, toY). */
static void StructureLine(World *world, int fromX, int fromY, int toX, int toY,
                          int thickness, CellMaterial material)
{
    int dx = toX - fromX;
    int dy = toY - fromY;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx)
                                                          : (dy < 0 ? -dy : dy);
    int step;

    if (steps == 0) steps = 1;
    for (step = 0; step <= steps; ++step) {
        int x = fromX + dx * step / steps;
        int y = fromY + dy * step / steps;

        StructureFill(world, x, y, x + thickness - 1, y + thickness - 1, material);
    }
}

/* A crate standing on `floorY`: a frame of girder round a body of planks,
   a brace across it. */
static void StructureCrate(World *world, int x, int floorY, int size)
{
    StructureFill(world, x, floorY - size, x + size - 1, floorY - 1, MATERIAL_PLANK);
    StructureFill(world, x, floorY - size, x + size - 1, floorY - size, MATERIAL_GIRDER);
    StructureFill(world, x, floorY - 1, x + size - 1, floorY - 1, MATERIAL_GIRDER);
    StructureLine(world, x + 1, floorY - 2, x + size - 2, floorY - size + 1, 1,
                  MATERIAL_GIRDER);
}

/* A drum of plate with two bands round it. */
static void StructureBarrel(World *world, int x, int floorY, int width, int height)
{
    StructureFill(world, x, floorY - height, x + width - 1, floorY - 1, MATERIAL_GIRDER);
    StructureFill(world, x, floorY - height / 3, x + width - 1, floorY - height / 3,
                  MATERIAL_METAL);
    StructureFill(world, x, floorY - height * 2 / 3, x + width - 1,
                  floorY - height * 2 / 3, MATERIAL_METAL);
}

/* A bunk: two boards on posts. */
static void StructureBunk(World *world, int x, int floorY, int length)
{
    StructureFill(world, x, floorY - 26, x + 1, floorY - 1, MATERIAL_GIRDER);
    StructureFill(world, x + length - 2, floorY - 26, x + length - 1, floorY - 1,
                  MATERIAL_GIRDER);
    StructureFill(world, x, floorY - 8, x + length - 1, floorY - 6, MATERIAL_PLANK);
    StructureFill(world, x, floorY - 22, x + length - 1, floorY - 20, MATERIAL_PLANK);
}

/* A table and a chair either side. */
static void StructureTable(World *world, int x, int floorY, int length)
{
    StructureFill(world, x, floorY - 12, x + length - 1, floorY - 11, MATERIAL_PLANK);
    StructureFill(world, x + 2, floorY - 10, x + 3, floorY - 1, MATERIAL_PLANK);
    StructureFill(world, x + length - 4, floorY - 10, x + length - 3, floorY - 1,
                  MATERIAL_PLANK);
    StructureFill(world, x - 7, floorY - 7, x - 3, floorY - 6, MATERIAL_PLANK);
    StructureFill(world, x - 7, floorY - 14, x - 7, floorY - 1, MATERIAL_PLANK);
    StructureFill(world, x + length + 2, floorY - 7, x + length + 6, floorY - 6,
                  MATERIAL_PLANK);
    StructureFill(world, x + length + 6, floorY - 14, x + length + 6, floorY - 1,
                  MATERIAL_PLANK);
}

/* Shelves on a wall, with things on them. */
static void StructureShelves(World *world, Rng *rng, int x, int topY, int length, int rows)
{
    int row;

    for (row = 0; row < rows; ++row) {
        int y = topY + row * 12;
        int item;

        StructureFill(world, x, y, x + length - 1, y + 1, MATERIAL_PLANK);
        for (item = x + 1; item < x + length - 3; item += RngRange(rng, 4, 8)) {
            int tall = RngRange(rng, 2, 6);

            StructureFill(world, item, y - tall, item + RngRange(rng, 1, 3), y - 1,
                          RngRange(rng, 0, 3) == 0 ? MATERIAL_LUMEN : MATERIAL_GIRDER);
        }
    }
}

/* A locker: a tall cabinet of girder with a lit tag. */
static void StructureLocker(World *world, int x, int floorY)
{
    StructureFill(world, x, floorY - 28, x + 9, floorY - 1, MATERIAL_GIRDER);
    StructureFill(world, x + 4, floorY - 27, x + 5, floorY - 2, MATERIAL_METAL);
    StructureFill(world, x + 2, floorY - 24, x + 2, floorY - 23, MATERIAL_LUMEN);
}

/* A lamp hanging from a ceiling on a cord. */
static void StructureHangingLamp(World *world, int x, int ceilingY, int drop)
{
    StructureFill(world, x, ceilingY, x, ceilingY + drop, MATERIAL_GIRDER);
    StructureFill(world, x - 2, ceilingY + drop + 1, x + 2, ceilingY + drop + 1,
                  MATERIAL_GIRDER);
    StructureFill(world, x - 1, ceilingY + drop + 2, x + 1, ceilingY + drop + 3,
                  MATERIAL_LUMEN);
}

/* A pipe along a ceiling with a valve now and then. */
static void StructurePipe(World *world, int fromX, int toX, int y)
{
    int x;

    StructureFill(world, fromX, y, toX, y + 1, MATERIAL_GIRDER);
    for (x = fromX + 9; x < toX - 4; x += 23) {
        StructureFill(world, x, y - 1, x + 2, y + 2, MATERIAL_METAL);
    }
}

/* A guardian: a tall hooded figure on a plinth, a staff in its hands with a
   light at the top. The precursors stood them at every door. */
static void StructureStatue(World *world, int x, int floorY, int height, CellMaterial stone)
{
    int base = floorY - 6;
    int row;

    StructureFill(world, x - 9, base, x + 9, floorY - 1, stone);
    for (row = 0; row < height; ++row) {
        int y = base - 1 - row;
        float along = (float)row / (float)height;
        /* A robe widening to the shoulders, a hood above them. */
        int half = along < 0.72f ? (int)(4.0f + 3.0f * along / 0.72f)
                                 : (int)(5.0f * (1.0f - (along - 0.72f) / 0.28f) + 1.0f);

        StructureFill(world, x - half, y, x + half, y, stone);
    }
    /* The staff and its light. */
    StructureFill(world, x + 8, base - height - 6, x + 9, base - 1, stone);
    StructureDisc(world, x + 8, base - height - 9, 3, MATERIAL_LUMEN);
}

/* An urn: a belly, a neck, a lip. */
static void StructureUrn(World *world, int x, int floorY, int height, CellMaterial stone)
{
    int row;

    for (row = 0; row < height; ++row) {
        float along = (float)row / (float)height;
        float belly = sinf(along * 3.14159f * 0.9f + 0.2f);
        int half = along > 0.8f ? 2 : (int)(1.5f + 4.0f * belly);

        StructureFill(world, x - half, floorY - 1 - row, x + half, floorY - 1 - row, stone);
    }
    StructureFill(world, x - 3, floorY - height - 1, x + 3, floorY - height - 1, stone);
}

/* A sarcophagus: a long box on a step, its lid lined with a lit glyph. */
static void StructureSarcophagus(World *world, int x, int floorY, int length)
{
    StructureFill(world, x - 2, floorY - 3, x + length + 1, floorY - 1, MATERIAL_PILLAR);
    StructureFill(world, x, floorY - 14, x + length - 1, floorY - 4, MATERIAL_PILLAR);
    StructureFill(world, x - 1, floorY - 17, x + length, floorY - 15, MATERIAL_PILLAR);
    StructureFill(world, x + 3, floorY - 16, x + length - 4, floorY - 16, MATERIAL_LUMEN);
}

/* A mine cart on its rail, heaped with ore. */
static void StructureCart(World *world, int x, int railY)
{
    StructureFill(world, x, railY - 12, x + 15, railY - 4, MATERIAL_METAL);
    StructureFill(world, x + 1, railY - 11, x + 14, railY - 5, MATERIAL_GIRDER);
    StructureDisc(world, x + 3, railY - 2, 2, MATERIAL_GIRDER);
    StructureDisc(world, x + 12, railY - 2, 2, MATERIAL_GIRDER);
    StructureDisc(world, x + 5, railY - 13, 2, MATERIAL_CRYSTAL);
    StructureDisc(world, x + 10, railY - 14, 3, MATERIAL_ROCK);
}

/* ---- the precursors' stone -------------------------------------------------- */

/* The column a builder's stone makes: relic stone stands behind the
   character as a pillar; basalt and brick are left as they are. */
static CellMaterial StructurePillarOf(CellMaterial stone)
{
    return stone == MATERIAL_RELIC ? MATERIAL_PILLAR : stone;
}

/* An obelisk lamp: a plinth, a shaft that narrows toward the top, and an orb
   of light resting on its point. The one thing on a precursor site that
   still works. */
static void StructureObeliskLamp(World *world, int centerX, int floorY, int height,
                                 int orbRadius, CellMaterial stone, CellMaterial orb)
{
    int top = floorY - height;
    int y;

    StructureFill(world, centerX - 12, floorY - 8, centerX + 12, floorY - 1, stone);
    StructureFill(world, centerX - 9, floorY - 13, centerX + 9, floorY - 9, stone);
    for (y = floorY - 14; y > top; --y) {
        float along = (float)(floorY - 14 - y) / (float)(height - 14);
        int half = (int)(6.0f - 3.0f * along);

        StructureFill(world, centerX - half, y, centerX + half, y, StructurePillarOf(stone));
    }
    /* A collar, then the orb sitting in it. */
    StructureFill(world, centerX - 6, top, centerX + 6, top + 3, stone);
    StructureDisc(world, centerX, top - orbRadius, orbRadius, orb);
}

/* A gateway: two great pillars carrying a stepped lintel, a lit key-stone at
   the crown of the opening and a strip of glyphs running up each pillar,
   stood on a stepped plinth. Some have lost a pillar, and the lintel it
   carried lies in the sand beside them. */
static void RuinGateway(World *world, Rng *rng, int centerX, CellMaterial stone,
                        bool drowned)
{
    int opening = RngRange(rng, 70, 110);
    int pillar = RngRange(rng, 20, 28);
    int height = RngRange(rng, 130, 200);
    int half = opening / 2 + pillar;
    int relief;
    bool wet;
    int floorY = StructureGround(world, centerX - half - 20, centerX + half + 20,
                                 &relief, &wet);
    bool broken;
    int topY;
    int lintelBottom;
    int side;
    int x;
    int y;

    if (floorY < 0 || relief > 90 || (wet && !drowned)) return;
    broken = RngRange(rng, 0, 99) < 35;
    topY = floorY - height;
    lintelBottom = topY + 30;
    StructureLevel(world, centerX - half - 20, centerX + half + 20, floorY,
                   floorY - relief - 2, stone);
    /* The plinth, in steps a walker climbs without noticing. */
    StructureFill(world, centerX - half - 18, floorY - 4, centerX + half + 18, floorY - 1,
                  stone);
    StructureFill(world, centerX - half - 12, floorY - 8, centerX + half + 12, floorY - 5,
                  stone);
    StructureFill(world, centerX - half - 6, floorY - 12, centerX + half + 6, floorY - 9,
                  stone);
    floorY -= 12;

    for (side = -1; side <= 1; side += 2) {
        int inner = centerX + side * (opening / 2);
        int outer = centerX + side * half;
        int first = inner < outer ? inner : outer;
        int last = inner < outer ? outer : inner;
        int columnTop = lintelBottom;
        int glyph = (first + last) / 2;

        if (broken && side > 0) {
            columnTop = floorY - (int)((float)(floorY - lintelBottom) *
                                       (0.35f + 0.25f * WorldGenUnit(world->seed, centerX,
                                                                     floorY, STRUCTURE_DECAY + 5u)));
        }
        /* The shafts are columns the character walks between and through;
           the plinth, the capitals and the lintel they carry are stone. */
        StructureWall(world, first, columnTop, last, floorY - 1, StructurePillarOf(stone),
                      floorY, topY, broken && side > 0 ? 0.9f : 0.25f);
        /* A capital and a base a little wider than the shaft. */
        StructureFill(world, first - 5, floorY - 7, last + 5, floorY - 1, stone);
        if (columnTop == lintelBottom) {
            StructureFill(world, first - 5, lintelBottom, last + 5, lintelBottom + 5,
                          stone);
        }
        /* The glyphs: a lit line up the middle of the shaft, broken into
           marks. */
        for (y = floorY - 16; y > columnTop + 10; --y) {
            if (((floorY - y) / 6) % 3 != 2 &&
                WorldMaterialAt(world, glyph, y) == StructurePillarOf(stone)) {
                StructureFill(world, glyph - 1, y, glyph + 1, y, MATERIAL_LUMEN);
            }
        }
    }

    /* The lintel in three courses, each wider than the one above it. Over
       a fallen pillar only the half that still has something under it. */
    for (y = lintelBottom - 1; y >= topY; --y) {
        int course = (lintelBottom - 1 - y) / 10;
        int reach = half + 10 - course * 10;
        int last = broken ? centerX + opening / 4 : centerX + reach;

        for (x = centerX - reach; x <= last; ++x) {
            if (!StructureDecayed(world, x, y, 0.2f, 0.3f)) {
                StructureSet(world, x, y, stone);
            }
        }
    }
    /* The key-stone: an orb of light hung from the crown of the arch. */
    StructureDisc(world, centerX, lintelBottom + 13, 11, MATERIAL_LUMEN);
    /* Guardians either side of the way through, on the plinth. */
    StructureStatue(world, centerX - half - 2 - 12, floorY, 46, StructurePillarOf(stone));
    StructureStatue(world, centerX + half + 2 + 12, floorY, 46, StructurePillarOf(stone));
    StructureFill(world, centerX - 2, lintelBottom, centerX + 2, lintelBottom + 2, stone);

    if (broken) {
        /* The lintel's other half, lying where it fell. */
        int fallen = centerX + half + RngRange(rng, 40, 70);
        int ground = WorldGenSolidY(world, fallen);

        if (ground > 0) {
            StructureWall(world, fallen - 34, ground - 16, fallen + 34, ground - 1, stone,
                          ground, ground - 16, 0.5f);
        }
        StructureRubble(world, centerX + half + 8, floorY + 12, RngRange(rng, 18, 30),
                        RngRange(rng, 10, 18));
    }
    StructureSweep(world, centerX - half - 22, topY - 2, centerX + half + 110, floorY + 14);
}

/* A stepped platform: terraces of relic stone rising to an altar, a lit
   frieze along the lip of every terrace, and a lamp on each end of the top.
   In the dunes the sand has drifted up its flanks. */
static void RuinTerraces(World *world, Rng *rng, int centerX, CellMaterial stone,
                         bool dunes)
{
    int tiers = RngRange(rng, 3, 5);
    int tierHeight = RngRange(rng, 24, 30);
    int base = RngRange(rng, 150, 210);
    int inset = RngRange(rng, 30, 42);
    int relief;
    bool wet;
    int floorY = StructureGround(world, centerX - base, centerX + base, &relief, &wet);
    int summit;
    int topHalf;
    int tier;
    int x;

    if (floorY < 0 || relief > 120 || wet) return;
    for (tier = 0; tier < tiers; ++tier) {
        int halfWidth = base - tier * inset;
        int bottom = floorY - tier * tierHeight;
        int top = bottom - tierHeight + 1;

        StructureWall(world, centerX - halfWidth, top, centerX + halfWidth, bottom, stone,
                      floorY, floorY - tiers * tierHeight, 0.3f);
        /* The frieze: a lit line under the lip, in dashes. */
        for (x = centerX - halfWidth + 4; x <= centerX + halfWidth - 4; ++x) {
            if (((x - centerX + 800) / 8) % 3 != 0) {
                int row;

                for (row = top + 4; row <= top + 5; ++row) {
                    if (WorldMaterialAt(world, x, row) == stone) {
                        StructureSet(world, x, row, MATERIAL_LUMEN);
                    }
                }
            }
        }
    }
    summit = floorY - tiers * tierHeight;
    topHalf = base - (tiers - 1) * inset;
    /* The altar and the orb above it. */
    StructureFill(world, centerX - 18, summit - 10, centerX + 18, summit, stone);
    StructureFill(world, centerX - 11, summit - 16, centerX + 11, summit - 11, stone);
    StructureDisc(world, centerX, summit - 30, 11, MATERIAL_CRYSTAL);
    StructureStatue(world, centerX - 34, summit + 1, 40, StructurePillarOf(stone));
    StructureStatue(world, centerX + 34, summit + 1, 40, StructurePillarOf(stone));
    StructureUrn(world, centerX - 52, summit + 1, 14, StructurePillarOf(stone));
    StructureUrn(world, centerX + 52, summit + 1, 14, StructurePillarOf(stone));
    StructureObeliskLamp(world, centerX - topHalf + 16, summit + 1,
                         RngRange(rng, 50, 70), 7, stone, MATERIAL_LUMEN);
    StructureObeliskLamp(world, centerX + topHalf - 16, summit + 1,
                         RngRange(rng, 50, 70), 7, stone, MATERIAL_LUMEN);
    if (dunes) {
        /* Drifts: wedges of sand up the flanks. */
        for (x = 0; x <= base + 40; ++x) {
            int drift = (int)((float)(tiers * tierHeight) * 0.55f *
                              (1.0f - (float)x / (float)(base + 40)));
            int side;

            for (side = -1; side <= 1; side += 2) {
                int column = centerX + side * (base - x + 40);
                int y;

                for (y = floorY - 1; y >= floorY - drift; --y) {
                    if (WorldMaterialAt(world, column, y) != MATERIAL_EMPTY) continue;
                    StructureSet(world, column, y, MATERIAL_SAND);
                }
            }
        }
    }
    StructureSweep(world, centerX - base - 42, summit - 80, centerX + base + 42, floorY);
}

/* A row of obelisk lamps along the ground, as if marking a road nobody
   travels any more. */
static void RuinObelisks(World *world, Rng *rng, int centerX, CellMaterial stone,
                         CellMaterial orb)
{
    int count = RngRange(rng, 2, 4);
    int spacing = RngRange(rng, 80, 130);
    int index;

    for (index = 0; index < count; ++index) {
        int x = centerX + (index - count / 2) * spacing;
        int relief;
        bool wet;
        int floorY = StructureGround(world, x - 13, x + 13, &relief, &wet);
        int height = RngRange(rng, 80, 150);

        if (floorY < 0 || relief > 20 || wet) continue;
        StructureLevel(world, x - 13, x + 13, floorY, floorY - relief - 1, stone);
        if (RngRange(rng, 0, 99) < 25) {
            /* Broken off, its orb gone out and rolled away. */
            StructureWall(world, x - 6, floorY - height / 3, x + 6, floorY - 1, stone,
                          floorY, floorY - height / 3, 0.6f);
            StructureFill(world, x - 12, floorY - 8, x + 12, floorY - 1, stone);
            continue;
        }
        StructureObeliskLamp(world, x, floorY, height, RngRange(rng, 7, 11), stone, orb);
    }
}

/* ---- the explorers' metal --------------------------------------------------- */

/* A ship that came down and did not leave: a long hull of plate lying
   tilted into the ground nose first, broken open behind the bridge, a row of
   lit ports along its side, a reactor still glowing in the tail and scrap
   thrown round the crater it dug. Its hold runs along the keel, a deck a
   walker can cross, entered through the break. On the sea floor the same
   wreck, drowned. */
static void WreckShip(World *world, Rng *rng, int centerX, bool drowned)
{
    int length = RngRange(rng, 300, 440);
    int height = RngRange(rng, 90, 120);
    float angle = (float)RngRange(rng, 6, 16) * (RngRange(rng, 0, 1) != 0 ? 1.0f : -1.0f) *
                  (PI / 180.0f);
    float cosine = cosf(angle);
    float sine = sinf(angle);
    int relief;
    bool wet;
    int groundY = StructureGround(world, centerX - length / 2, centerX + length / 2,
                                  &relief, &wet);
    float breakAt = 0.36f + 0.1f * WorldGenUnit(world->seed, centerX, 3, STRUCTURE_DECAY + 6u);
    int breakHalf = RngRange(rng, 12, 24);
    /* The nose leads the way the ship is tilted down. */
    float noseSign = angle > 0.0f ? 1.0f : -1.0f;
    int reachX = length / 2 + height;
    int reachY = height + (int)(fabsf(sine) * (float)length * 0.5f) + 8;
    int centerY;
    int x;
    int y;

    if (groundY < 0 || relief > 260 || (wet && !drowned)) return;
    /* Resting on the ground, its nose dug in by the tilt. */
    centerY = groundY - height / 3;

    for (y = centerY - reachY; y <= centerY + reachY; ++y) {
        for (x = centerX - reachX; x <= centerX + reachX; ++x) {
            float dx = (float)(x - centerX) + 0.5f;
            float dy = (float)(y - centerY) + 0.5f;
            /* Along the hull toward the nose, and across it downward. */
            float u = (dx * cosine + dy * sine) * noseSign;
            float v = -dx * sine + dy * cosine;
            float t = u / (float)length + 0.5f;
            float upper;
            float lower;
            float hull;
            bool inside;
            bool interior;

            if (t < 0.0f || t > 1.0f) continue;
            hull = (float)height * 0.5f;
            if (t > 0.72f) {
                hull *= sqrtf((1.0f - t) / 0.28f);
            } else if (t < 0.06f) {
                hull *= 0.8f + 0.2f * t / 0.06f;
            }
            upper = hull;
            lower = hull * 0.85f;
            /* The bridge, a step up on the back behind the nose. */
            if (t > 0.52f && t < 0.7f) {
                upper += (float)height * 0.22f;
            }
            /* Fins at the tail, above and below. */
            if (t < 0.14f) {
                float fin = (0.14f - t) / 0.14f;

                upper += (float)height * 0.45f * fin;
                lower += (float)height * 0.25f * fin;
            }
            inside = v >= -upper && v <= lower;
            if (!inside) continue;
            /* The hold: one deck along the keel, tall enough to walk, from
               behind the fins to under the bridge. The rest of the hull is
               plate — a ship that is all hollow reads as a drawing of one. */
            interior = v <= lower - 3.0f &&
                       v >= lower - 3.0f - (float)STRUCTURE_CORRIDOR_ROWS - 2.0f &&
                       t > 0.12f && t < 0.8f;
            /* Broken open: a jagged gap right through the hull. */
            if (fabsf(u - (breakAt - 0.5f) * (float)length) <
                (float)breakHalf + 4.0f * WorldGenUnit(world->seed, x / 3, y / 2,
                                                       STRUCTURE_DECAY + 7u)) {
                StructureSet(world, x, y, MATERIAL_EMPTY);
                continue;
            }
            if (interior) {
                /* Frames across the hold every so often, a door's height
                   clear under each. */
                int frame = (int)floorf(u / 30.0f);
                float within = u - (float)frame * 30.0f;
                bool rib = within < 3.0f && v < lower - 3.0f - (float)STRUCTURE_DOOR_ROWS;

                StructureSet(world, x, y, rib ? MATERIAL_GIRDER : MATERIAL_EMPTY);
                WorldGenSetBackWall(world, x, y, x, y, MATERIAL_METAL);
                continue;
            }
            /* Lit ports in a row along the upper side, and a line of lamps
               along the hold's ceiling. */
            if ((v > -upper + 5.0f && v < -upper + 8.0f && t > 0.16f && t < 0.9f &&
                 ((int)floorf(u) % 12 + 12) % 12 < 4) ||
                (v >= lower - 5.0f - (float)STRUCTURE_CORRIDOR_ROWS - 2.0f &&
                 v < lower - 4.0f - (float)STRUCTURE_CORRIDOR_ROWS - 2.0f && t > 0.14f &&
                 t < 0.78f && ((int)floorf(u) % 16 + 16) % 16 < 3)) {
                StructureSet(world, x, y, MATERIAL_LUMEN);
            } else {
                StructureSet(world, x, y, MATERIAL_METAL);
            }
        }
    }
    /* The cargo, still in the hold where it was stowed: crates and drums
       along the deck, found by looking down from the middle of the hold. */
    {
        int cargo;

        for (cargo = 0; cargo < 7; ++cargo) {
            float t = 0.18f + 0.08f * (float)cargo;
            float u = (t - 0.5f) * (float)length * noseSign;
            float v = (float)height * 0.85f * 0.5f - 12.0f;
            int cargoX = centerX + (int)(u * cosine - v * sine);
            int cargoY = centerY + (int)(u * sine + v * cosine);
            int floorY = cargoY;

            if (fabsf(t - breakAt) < 0.06f) continue;
            while (floorY < cargoY + 30 && WorldMaterialAt(world, cargoX, floorY) == MATERIAL_EMPTY) {
                ++floorY;
            }
            if (floorY >= cargoY + 30) continue;
            if ((cargo & 1) == 0) {
                StructureCrate(world, cargoX, floorY, RngRange(rng, 9, 14));
            } else {
                StructureBarrel(world, cargoX, floorY, 8, RngRange(rng, 10, 15));
            }
        }
    }
    /* The reactor, still glowing in the tail. */
    {
        float u = (-0.5f + 0.08f) * (float)length * noseSign;
        int reactorX = centerX + (int)(u * cosine);
        int reactorY = centerY + (int)(u * sine);

        StructureDisc(world, reactorX, reactorY, 12, MATERIAL_CRYSTAL);
    }
    /* The crater's rim and the scrap thrown round it. */
    if (!drowned) {
        int nose = centerX + (int)(noseSign * (float)length * 0.5f * cosine);
        int piece;

        StructureRubble(world, nose + (int)(noseSign * 30.0f), groundY + 4,
                        RngRange(rng, 24, 40), RngRange(rng, 14, 24));
        for (piece = 0; piece < 8; ++piece) {
            int scrapX = centerX + RngRange(rng, -length, length);
            int ground = WorldGenSolidY(world, scrapX);

            if (ground > 0 && fabsf((float)(ground - groundY)) < 80.0f) {
                StructureDisc(world, scrapX, ground - 2, RngRange(rng, 4, 9),
                              MATERIAL_METAL);
            }
        }
    }
    StructureSweep(world, centerX - reachX, centerY - reachY, centerX + reachX,
                   centerY + reachY);
}

/* An outpost: a cabin of plate on four legs, lamps along its ceiling, a
   console inside, a stair down from the door, an antenna mast with a light
   on top and a dish beside it. Abandoned, its roof going first. */
static void WreckOutpost(World *world, Rng *rng, int centerX)
{
    int width = RngRange(rng, 120, 170);
    int cabin = 64;
    int legs = RngRange(rng, 24, 40);
    int firstX = centerX - width / 2;
    int lastX = centerX + width / 2;
    int relief;
    bool wet;
    int floorY = StructureGround(world, firstX - 40, lastX + 40, &relief, &wet);
    int bottom;
    int top;
    int doorLeft;
    int leg;
    int x;
    int y;

    if (floorY < 0 || relief > 90 || wet) return;
    bottom = floorY - legs - relief;
    top = bottom - cabin;
    doorLeft = RngRange(rng, 0, 1) != 0;
    StructureLevel(world, firstX - 40, lastX + 40, floorY, top - 90, MATERIAL_ROCK);

    /* The legs, down to whatever holds them, braced across and crossed:
       girders, which the character walks past rather than into. */
    for (leg = 0; leg < 4; ++leg) {
        int legX = firstX + 6 + leg * (width - 16) / 3;

        for (y = bottom + 1; y < floorY + 60; ++y) {
            if (y > floorY && MaterialIsSolid(WorldMaterialAt(world, legX, y)) &&
                !MaterialIsBackdrop(WorldMaterialAt(world, legX, y))) {
                break;
            }
            StructureFill(world, legX, y, legX + 4, y, MATERIAL_GIRDER);
        }
        if (leg < 3) {
            int nextX = firstX + 6 + (leg + 1) * (width - 16) / 3;

            StructureLine(world, legX + 4, bottom + 2, nextX, floorY - 2, 2, MATERIAL_GIRDER);
            StructureLine(world, legX + 4, floorY - 2, nextX, bottom + 2, 2, MATERIAL_GIRDER);
        }
    }
    StructureFill(world, firstX + 6, bottom + legs / 2, lastX - 6, bottom + legs / 2 + 2,
                  MATERIAL_GIRDER);
    /* Floor and walls, a gabled roof over them, the roof worn. */
    StructureFill(world, firstX, bottom - 5, lastX, bottom, MATERIAL_METAL);
    StructureWall(world, firstX, top, firstX + 5, bottom - 6, MATERIAL_METAL, bottom, top,
                  0.25f);
    StructureWall(world, lastX - 5, top, lastX, bottom - 6, MATERIAL_METAL, bottom, top,
                  0.25f);
    StructureWall(world, firstX, top, lastX, top + 5, MATERIAL_METAL, bottom, top, 0.4f);
    for (x = firstX - 6; x <= lastX + 6; ++x) {
        int fromCentre = x - centerX < 0 ? centerX - x : x - centerX;
        int peak = (width / 2 + 6 - fromCentre) / 4;

        if (peak > 0) {
            StructureWall(world, x, top - peak, x, top - peak + 3, MATERIAL_METAL, top,
                          top - 30, 0.5f);
        }
    }
    /* A band of plate along the walls, a shade darker, at the height of a
       window sill. */
    StructureFill(world, firstX, bottom - 30, firstX + 5, bottom - 28, MATERIAL_GIRDER);
    StructureFill(world, lastX - 5, bottom - 30, lastX, bottom - 28, MATERIAL_GIRDER);
    /* Lamps hung from the ceiling, a pipe along it. */
    for (x = firstX + 16; x < lastX - 14; x += 26) {
        StructureHangingLamp(world, x, top + 6, RngRange(rng, 4, 9));
    }
    StructurePipe(world, firstX + 6, lastX - 6, top + 7);
    WorldGenSetBackWall(world, firstX + 6, top + 6, lastX - 6, bottom - 6, MATERIAL_METAL);
    /* What the crew left: bunks, a table, lockers, shelves, crates. */
    {
        int left = firstX + 8;
        int right = lastX - 8;
        int bunkX = doorLeft ? right - 40 : left + 30;

        StructureBunk(world, bunkX, bottom - 6, 34);
        StructureTable(world, centerX - 12, bottom - 6, 24);
        StructureLocker(world, doorLeft ? right - 56 : left + 70, bottom - 6);
        StructureLocker(world, doorLeft ? right - 68 : left + 82, bottom - 6);
        StructureShelves(world, rng, doorLeft ? left + 14 : right - 38, top + 22, 26, 2);
        StructureCrate(world, doorLeft ? left + 10 : right - 22, bottom - 6, 12);
        StructureCrate(world, doorLeft ? left + 12 : right - 20, bottom - 18, 9);
    }
    /* The door and the stair down from it. */
    {
        int wallFirst = doorLeft ? firstX : lastX - 5;
        int direction = doorLeft ? -1 : 1;
        int stepX = doorLeft ? firstX - 1 : lastX + 1;
        int stepY = bottom;

        StructureFill(world, wallFirst, bottom - 5 - STRUCTURE_DOOR_ROWS, wallFirst + 5,
                      bottom - 6, MATERIAL_EMPTY);
        while (stepY < floorY) {
            int run;

            for (run = 0; run < 8; ++run) {
                StructureFill(world, stepX + direction * run, stepY - 2,
                              stepX + direction * run, stepY, MATERIAL_METAL);
            }
            /* A railing post on every step, a rail along the tops. */
            StructureFill(world, stepX, stepY - 16, stepX, stepY - 3, MATERIAL_GIRDER);
            StructureLine(world, stepX, stepY - 16, stepX + direction * 8, stepY - 12, 1,
                          MATERIAL_GIRDER);
            stepX += direction * 8;
            stepY += 4;
        }
        /* Drums by the foot of the stair. */
        StructureBarrel(world, stepX + direction * 6, floorY, 8, 14);
        StructureBarrel(world, stepX + direction * 16, floorY, 8, 12);
    }
    /* A console against the far wall: a cabinet with a lit screen. */
    {
        int consoleX = doorLeft ? lastX - 30 : firstX + 8;

        StructureFill(world, consoleX, bottom - 22, consoleX + 21, bottom - 6,
                      MATERIAL_GIRDER);
        StructureFill(world, consoleX + 2, bottom - 20, consoleX + 19, bottom - 15,
                      MATERIAL_LUMEN);
    }
    /* The mast and its light, the dish on the other end of the roof. */
    {
        int mastX = doorLeft ? lastX - 16 : firstX + 12;
        int dishX = doorLeft ? firstX + 26 : lastX - 26;
        int mastTop = top - RngRange(rng, 60, 100);

        StructureFill(world, mastX, mastTop, mastX + 3, top - 1, MATERIAL_GIRDER);
        for (y = top - 16; y > mastTop + 4; y -= 16) {
            StructureFill(world, mastX - 7, y, mastX + 10, y + 1, MATERIAL_GIRDER);
            StructureLine(world, mastX - 7, y, mastX, y - 14, 1, MATERIAL_GIRDER);
            StructureLine(world, mastX + 10, y, mastX + 3, y - 14, 1, MATERIAL_GIRDER);
        }
        StructureDisc(world, mastX + 1, mastTop - 4, 4, MATERIAL_LUMEN);
        for (x = -18; x <= 18; ++x) {
            int dip = (x * x) / 26;

            StructureFill(world, dishX + x, top - 22 + dip, dishX + x, top - 19 + dip,
                          MATERIAL_GIRDER);
        }
        StructureFill(world, dishX - 1, top - 10, dishX + 2, top - 1, MATERIAL_GIRDER);
        /* A water tank on its own stand at the other end. */
        {
            int tankX = doorLeft ? firstX - 30 : lastX + 14;
            int tankBottom = floorY - 24;

            StructureFill(world, tankX, tankBottom, tankX + 1, floorY - 1, MATERIAL_GIRDER);
            StructureFill(world, tankX + 14, tankBottom, tankX + 15, floorY - 1,
                          MATERIAL_GIRDER);
            StructureLine(world, tankX, floorY - 2, tankX + 15, tankBottom, 1, MATERIAL_GIRDER);
            StructureFill(world, tankX - 2, tankBottom - 22, tankX + 17, tankBottom - 1,
                          MATERIAL_METAL);
            StructureFill(world, tankX - 2, tankBottom - 12, tankX + 17, tankBottom - 11,
                          MATERIAL_GIRDER);
        }
    }
    StructureSweep(world, firstX - 110, top - 110, lastX + 110, floorY + 60);
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
                                          RngRange(&rng, -260, 260),
                                      world->width);
        int roll = RngRange(&rng, 0, 99);

        if (WorldGenNearSpawn(world, centerX) || roll >= 72) continue;
        switch (WorldBiomeAt(world, centerX)) {
        case WORLD_BIOME_TEMPERATE:
            if (roll < 22) {
                RuinGateway(world, &rng, centerX, MATERIAL_RELIC, false);
            } else if (roll < 40) {
                WreckOutpost(world, &rng, centerX);
            } else if (roll < 56) {
                WreckShip(world, &rng, centerX, false);
            } else {
                RuinObelisks(world, &rng, centerX, MATERIAL_RELIC, MATERIAL_LUMEN);
            }
            break;
        case WORLD_BIOME_DUNES:
            if (roll < 30) {
                RuinTerraces(world, &rng, centerX, MATERIAL_RELIC, true);
            } else if (roll < 50) {
                WreckShip(world, &rng, centerX, false);
            } else {
                RuinObelisks(world, &rng, centerX, MATERIAL_RELIC, MATERIAL_CRYSTAL);
            }
            break;
        case WORLD_BIOME_FROST:
            if (roll < 34) {
                WreckOutpost(world, &rng, centerX);
            } else {
                RuinGateway(world, &rng, centerX, MATERIAL_RELIC, false);
            }
            break;
        case WORLD_BIOME_VOLCANIC:
            if (roll < 30) {
                RuinTerraces(world, &rng, centerX, MATERIAL_BASALT, false);
            } else if (roll < 50) {
                WreckShip(world, &rng, centerX, false);
            } else {
                RuinObelisks(world, &rng, centerX, MATERIAL_BASALT, MATERIAL_CRYSTAL);
            }
            break;
        case WORLD_BIOME_OCEAN:
            /* On the sea floor, before the sea is poured over it. */
            if (roll < 40) {
                WreckShip(world, &rng, centerX, true);
            } else {
                RuinGateway(world, &rng, centerX, MATERIAL_RELIC, true);
            }
            break;
        case WORLD_BIOME_COUNT:
            break;
        }
    }
}

/* ---- underground ------------------------------------------------------------- */

/* A vault chamber: relic walls four thick round a hall tall enough to jump
   in, lamps set in its ceiling, pillars down its length and an altar with
   a crystal on it. */
static void VaultRoom(World *world, Rng *rng, int left, int top, int width, int height)
{
    int right = left + width;
    int bottom = top + height;
    int x;

    StructureWall(world, left - 8, top - 8, right + 8, bottom + 8, MATERIAL_RELIC,
                  bottom, top - 8, 0.12f);
    StructureFill(world, left, top, right, bottom, MATERIAL_EMPTY);
    WorldGenSetBackWall(world, left, top, right, bottom, MATERIAL_RELIC);
    for (x = left + 6; x < right - 6; ++x) {
        if (((x - left) / 10) % 3 == 1) {
            StructureFill(world, x, top, x, top + 1, MATERIAL_LUMEN);
        }
    }
    for (x = left + 44; x < right - 40; x += RngRange(rng, 40, 60)) {
        int roll = RngRange(rng, 0, 99);

        if (roll < 40) {
            /* A column to the roof, walked past. */
            StructureFill(world, x, top, x + 8, bottom, MATERIAL_PILLAR);
            StructureFill(world, x - 3, top + 2, x + 11, top + 6, MATERIAL_PILLAR);
            StructureFill(world, x - 3, bottom - 4, x + 11, bottom, MATERIAL_PILLAR);
        } else if (roll < 65) {
            StructureStatue(world, x + 4, bottom + 1, RngRange(rng, 34, height - 26),
                            MATERIAL_PILLAR);
        } else if (roll < 85) {
            StructureUrn(world, x, bottom + 1, RngRange(rng, 10, 18), MATERIAL_PILLAR);
            StructureUrn(world, x + 12, bottom + 1, RngRange(rng, 8, 14), MATERIAL_PILLAR);
        } else {
            StructureSarcophagus(world, x - 6, bottom + 1, 28);
        }
    }
    /* Panels of glyphs set into the far wall, lit. */
    for (x = left + 20; x < right - 30; x += RngRange(rng, 50, 80)) {
        int row;

        for (row = 0; row < 3; ++row) {
            int mark;

            for (mark = 0; mark < 5; ++mark) {
                if (WorldGenUnit(world->seed, x + mark, top + row, STRUCTURE_DECAY + 9u) < 0.7f) {
                    StructureFill(world, x + mark * 4, top + 14 + row * 6,
                                  x + mark * 4 + 2, top + 15 + row * 6, MATERIAL_LUMEN);
                }
            }
        }
    }
    {
        int altar = left + width / 2;

        StructureFill(world, altar - 20, bottom - 6, altar + 20, bottom, MATERIAL_RELIC);
        StructureFill(world, altar - 12, bottom - 12, altar + 12, bottom - 7,
                      MATERIAL_RELIC);
        StructureFill(world, altar - 3, bottom - 40, altar + 3, bottom - 13,
                      MATERIAL_CRYSTAL);
    }
    StructureSweep(world, left - 9, top - 9, right + 9, bottom + 9);
    /* Part of the ceiling has come in. */
    if (RngRange(rng, 0, 99) < 30) {
        int fall = left + RngRange(rng, 12, width - 30);

        StructureRubble(world, fall + 10, bottom + 1, RngRange(rng, 10, 18),
                        RngRange(rng, 8, height / 3));
    }
}

/* A passage, level then vertical, from one chamber to the next. Laid in two
   passes: the lined passage before the chambers are built, and its bore
   again after, which is what opens the doorways through the chambers' walls
   without running a line of stone across the inside of a hall. The vertical
   run has ledges on alternate sides, a jump apart, so it can be climbed on
   foot. */
static void VaultPassage(World *world, int fromX, int fromY, int toX, int toY,
                         bool lined)
{
    int step = fromX < toX ? 1 : -1;
    int x;
    int y;

    for (x = fromX; x != toX + step; x += step) {
        if (lined) {
            StructureFill(world, x, fromY - STRUCTURE_CORRIDOR_ROWS - 5, x,
                          fromY - STRUCTURE_CORRIDOR_ROWS, MATERIAL_RELIC);
            StructureFill(world, x, fromY + 1, x, fromY + 5, MATERIAL_RELIC);
        } else {
            WorldGenSetBackWall(world, x, fromY - STRUCTURE_CORRIDOR_ROWS + 1, x, fromY,
                                MATERIAL_RELIC);
        }
        StructureFill(world, x, fromY - STRUCTURE_CORRIDOR_ROWS + 1, x, fromY,
                      MATERIAL_EMPTY);
    }
    step = fromY < toY ? 1 : -1;
    for (y = fromY; y != toY + step; y += step) {
        if (lined) {
            StructureFill(world, toX - 24, y, toX - 20, y, MATERIAL_RELIC);
            StructureFill(world, toX + 20, y, toX + 24, y, MATERIAL_RELIC);
        } else {
            WorldGenSetBackWall(world, toX - 19, y, toX + 19, y, MATERIAL_RELIC);
        }
        StructureFill(world, toX - 19, y, toX + 19, y, MATERIAL_EMPTY);
        if (!lined && y % 28 == 0) {
            bool left = (y / 28) % 2 == 0;

            StructureFill(world, left ? toX - 19 : toX + 4, y, left ? toX - 4 : toX + 19,
                          y + 2, MATERIAL_RELIC);
        }
    }
}

#define VAULT_MAX_ROOMS 6

typedef struct VaultRoomPlan {
    int left;
    int top;
    int width;
    int height;
} VaultRoomPlan;

/* A precursor vault: chambers strung along passages, deep under the soil,
   one of them sometimes reached by a shaft from the surface. */
static void GenerateVaults(World *world)
{
    int count = (world->width + DUNGEON_SPACING - 1) / DUNGEON_SPACING;
    int feature;

    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_DUNGEONS);
        int x = StructureModulo(feature * DUNGEON_SPACING + RngRange(&rng, 0, DUNGEON_SPACING - 1),
                                world->width);
        int surface = WorldGenSurfaceY(world, x);
        int top = surface + 160;
        int bottom = (int)WorldGroundY(world, 0.78f);
        int rooms = RngRange(&rng, 3, VAULT_MAX_ROOMS);
        VaultRoomPlan plan[VAULT_MAX_ROOMS];
        bool entrance;
        int room;
        int pass;
        int y;

        if (RngRange(&rng, 0, 99) >= 70 || WorldGenNearSpawn(world, x) ||
            WorldBiomeAt(world, x) == WORLD_BIOME_OCEAN || top + 140 >= bottom) {
            continue;
        }
        y = RngRange(&rng, top, bottom - 120);
        for (room = 0; room < rooms; ++room) {
            plan[room].width = RngRange(&rng, 140, 230);
            plan[room].height = RngRange(&rng, 70, 110);
            plan[room].left = x - plan[room].width / 2;
            plan[room].top = y;
            x += (RngRange(&rng, 0, 1) != 0 ? 1 : -1) * RngRange(&rng, 240, 340);
            y += RngRange(&rng, -50, 90);
            /* Never climbing back toward the surface: a chamber that walked
               up room by room broke out of the ground as a box. */
            if (y < top) y = top;
            if (y > bottom - 120) y = bottom - 120;
        }
        entrance = RngRange(&rng, 0, 99) < 55;

        /* Passages lined, chambers over them, passages bored again. */
        for (pass = 0; pass < 2; ++pass) {
            for (room = 1; room < rooms; ++room) {
                const VaultRoomPlan *from = &plan[room - 1];
                const VaultRoomPlan *to = &plan[room];

                VaultPassage(world, from->left + from->width / 2, from->top + from->height,
                             to->left + to->width / 2, to->top + to->height, pass == 0);
            }
            if (entrance && WorldGenSurfaceY(world, plan[rooms - 1].left +
                                                     plan[rooms - 1].width / 2) <
                                (int)WorldSeaLevelY(world) - 12) {
                const VaultRoomPlan *last = &plan[rooms - 1];
                int stairX = last->left + last->width / 2;

                VaultPassage(world, stairX, last->top + last->height, stairX,
                             WorldGenSurfaceY(world, stairX) - 2, pass == 0);
            }
            if (pass == 0) {
                for (room = 0; room < rooms; ++room) {
                    /* Not into a cavern: a chamber there is a box of stone
                       hanging in the air. The passages still reach it. */
                    if (StructureSolidShare(world, plan[room].left - 4, plan[room].top - 4,
                                            plan[room].left + plan[room].width + 4,
                                            plan[room].top + plan[room].height + 4) <
                        0.6f) {
                        continue;
                    }
                    VaultRoom(world, &rng, plan[room].left, plan[room].top,
                              plan[room].width, plan[room].height);
                }
            }
        }
    }
}

/* A mine: a headframe over the shaft, the shaft lined with plate, grates
   on alternate sides a jump apart with a lamp over each, galleries driven
   off it with beams along their ceilings, and at the bottom the machine
   that dug it, dead. */
static void GenerateMines(World *world)
{
    int count = (world->width + SHAFT_SPACING - 1) / SHAFT_SPACING;
    int feature;

    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_SHAFTS);
        int x = StructureModulo(feature * SHAFT_SPACING + RngRange(&rng, 0, SHAFT_SPACING - 1),
                                world->width);
        int relief;
        bool wet;
        int top = StructureGround(world, x - 40, x + 40, &relief, &wet) - 1;
        int depth = RngRange(&rng, 400, 700);
        int y;

        if (RngRange(&rng, 0, 99) >= 65 || WorldGenNearSpawn(world, x) ||
            WorldBiomeAt(world, x) == WORLD_BIOME_OCEAN || top < 0 || wet ||
            relief > 80 || top + depth > (int)WorldGroundY(world, 0.82f) ||
            /* A shaft opened under the sea is the sea's way into the whole
               underground. */
            top > (int)WorldSeaLevelY(world) - 12) {
            continue;
        }
        /* The ground the headframe stands on, levelled. */
        StructureLevel(world, x - 40, x + 40, top + 1, top - relief - 2, MATERIAL_ROCK);
        /* The headframe: two legs of girder, braced and crossed, a beam,
           a wheel — all of it standing behind the character. */
        StructureFill(world, x - 32, top - 80, x - 28, top, MATERIAL_GIRDER);
        StructureFill(world, x + 28, top - 80, x + 32, top, MATERIAL_GIRDER);
        StructureFill(world, x - 36, top - 85, x + 36, top - 80, MATERIAL_GIRDER);
        for (y = top - 70; y < top - 4; y += 22) {
            StructureFill(world, x - 28, y, x + 28, y + 2, MATERIAL_GIRDER);
            StructureLine(world, x - 28, y + 2, x + 28, y + 22, 1, MATERIAL_GIRDER);
            StructureLine(world, x + 28, y + 2, x - 28, y + 22, 1, MATERIAL_GIRDER);
        }
        for (y = -15; y <= 15; ++y) {
            int across;

            for (across = -15; across <= 15; ++across) {
                int ring = across * across + y * y;

                if ((ring <= 225 && ring >= 160) ||
                    (ring < 160 && (across == 0 || y == 0) && ring > 4)) {
                    StructureSet(world, x + across, top - 101 + y, MATERIAL_GIRDER);
                }
            }
        }
        StructureFill(world, x - 2, top - 88, x + 2, top - 86, MATERIAL_GIRDER);
        /* The cable down from the wheel, and a winch house beside. */
        StructureFill(world, x + 14, top - 101, x + 14, top + 30, MATERIAL_GIRDER);
        StructureFill(world, x + 44, top - 34, x + 76, top, MATERIAL_METAL);
        StructureFill(world, x + 48, top - 30, x + 72, top - 4, MATERIAL_EMPTY);
        WorldGenSetBackWall(world, x + 48, top - 30, x + 72, top - 4, MATERIAL_METAL);
        StructureFill(world, x + 50, top - 18, x + 60, top - 4, MATERIAL_GIRDER);
        StructureDisc(world, x + 55, top - 22, 3, MATERIAL_GIRDER);
        StructureHangingLamp(world, x + 66, top - 30, 5);
        StructureCrate(world, x - 60, top + 1, 12);
        StructureCrate(world, x - 50, top + 1, 10);
        StructureCrate(world, x - 57, top - 11, 9);

        for (y = top; y < top + depth; ++y) {
            int row = y - top;

            StructureFill(world, x - 23, y, x + 23, y, MATERIAL_EMPTY);
            WorldGenSetBackWall(world, x - 23, y, x + 23, y, MATERIAL_METAL);
            StructureFill(world, x - 27, y, x - 24, y, MATERIAL_METAL);
            StructureFill(world, x + 24, y, x + 27, y, MATERIAL_METAL);
            if (row % 30 == 29) {
                bool left = (row / 30) % 2 == 0;

                StructureFill(world, left ? x - 23 : x + 2, y, left ? x - 2 : x + 23, y + 2,
                              MATERIAL_METAL);
                StructureFill(world, left ? x - 23 : x + 20, y - 20, left ? x - 20 : x + 23,
                              y - 16, MATERIAL_LUMEN);
            }
            /* A gallery every hundred and twenty rows, either way. */
            if (row % 120 == 119 && row > 60) {
                int direction = RngRange(&rng, 0, 1) != 0 ? 1 : -1;
                int length = RngRange(&rng, 140, 320);
                int step;

                for (step = 24; step < length; ++step) {
                    int column = x + direction * step;

                    StructureFill(world, column, y - STRUCTURE_CORRIDOR_ROWS + 1, column, y,
                                  MATERIAL_EMPTY);
                    WorldGenSetBackWall(world, column, y - STRUCTURE_CORRIDOR_ROWS + 1,
                                        column, y, MATERIAL_METAL);
                    StructureFill(world, column, y + 1, column, y + 4, MATERIAL_METAL);
                    /* The rail along the floor. */
                    StructureSet(world, column, y, MATERIAL_GIRDER);
                    /* A frame every thirty cells: a beam under the roof on
                       two posts, which the character walks through. */
                    if (step % 30 < 6) {
                        StructureFill(world, column, y - STRUCTURE_CORRIDOR_ROWS - 3, column,
                                      y - STRUCTURE_CORRIDOR_ROWS, MATERIAL_GIRDER);
                    }
                    if (step % 30 == 0 || step % 30 == 5) {
                        StructureFill(world, column, y - STRUCTURE_CORRIDOR_ROWS + 1, column,
                                      y - 1, MATERIAL_GIRDER);
                    }
                    if (step % 90 == 47) {
                        StructureCart(world, column, y);
                    } else if (step % 110 == 71) {
                        StructureCrate(world, column, y, 10);
                    }
                    if (step % 60 == 15) {
                        StructureFill(world, column, y - STRUCTURE_CORRIDOR_ROWS + 1,
                                      column + 3, y - STRUCTURE_CORRIDOR_ROWS + 4,
                                      MATERIAL_LUMEN);
                    }
                }
            }
        }
        /* The machine at the bottom. */
        y = top + depth - 1;
        StructureFill(world, x - 20, y - 28, x + 20, y, MATERIAL_GIRDER);
        StructureFill(world, x - 14, y - 24, x + 14, y - 18, MATERIAL_LUMEN);
        StructureFill(world, x - 5, y + 1, x + 5, y + 20, MATERIAL_METAL);
    }
}

/* A reliquary, sealed in the deep basalt with nothing leading to it: a hall
   under a row of pillars with crystal capitals, a stepped dais in the middle
   with a crystal monolith on it, for whoever digs that far. */
static void GenerateReliquaries(World *world)
{
    int count = (world->width + CRYPT_SPACING - 1) / CRYPT_SPACING;
    int feature;

    for (feature = 0; feature < count; ++feature) {
        Rng rng = WorldGenFeatureRng(world->seed, feature, STRUCTURE_CRYPTS);
        int x = StructureModulo(feature * CRYPT_SPACING + RngRange(&rng, 0, CRYPT_SPACING - 1),
                                world->width);
        int top = (int)WorldGroundY(world, 0.84f);
        int bottom = (int)WorldGroundY(world, 0.93f);
        int width = RngRange(&rng, 160, 260);
        int height = RngRange(&rng, 70, 100);
        int y;
        int column;

        if (RngRange(&rng, 0, 99) >= 60 || top >= bottom) continue;
        y = RngRange(&rng, top, bottom);
        if (StructureSolidShare(world, x - width / 2 - 10, y - 10, x + width / 2 + 10,
                                y + height + 10) < 0.85f) {
            continue;
        }
        StructureFill(world, x - width / 2 - 10, y - 10, x + width / 2 + 10,
                      y + height + 10, MATERIAL_RELIC);
        StructureFill(world, x - width / 2, y, x + width / 2, y + height, MATERIAL_EMPTY);
        WorldGenSetBackWall(world, x - width / 2, y, x + width / 2, y + height,
                            MATERIAL_RELIC);
        for (column = x - width / 2 + 18; column < x + width / 2 - 14; column += 40) {
            if (column > x - 34 && column < x + 34) continue;
            StructureFill(world, column, y, column + 8, y + height, MATERIAL_PILLAR);
            StructureFill(world, column - 3, y, column + 11, y + 5, MATERIAL_CRYSTAL);
            /* Between the columns, the dead and what was left with them. */
            if (column + 40 < x + width / 2 - 14) {
                if (((column / 40) & 1) == 0) {
                    StructureSarcophagus(world, column + 14, y + height + 1, 20);
                } else {
                    StructureUrn(world, column + 18, y + height + 1, 16, MATERIAL_PILLAR);
                    StructureUrn(world, column + 28, y + height + 1, 11, MATERIAL_PILLAR);
                }
            }
        }
        for (column = x - width / 2 + 4; column < x + width / 2 - 4; ++column) {
            if (((column - x + 800) / 6) % 4 == 0) {
                StructureFill(world, column, y + height - 1, column, y + height,
                              MATERIAL_LUMEN);
            }
        }
        StructureFill(world, x - 30, y + height - 6, x + 30, y + height, MATERIAL_RELIC);
        StructureFill(world, x - 18, y + height - 12, x + 18, y + height - 7,
                      MATERIAL_RELIC);
        StructureFill(world, x - 5, y + height - 52, x + 5, y + height - 13,
                      MATERIAL_CRYSTAL);
    }
}

void WorldGenerateUnderground(World *world)
{
    if (world->height < 400 || world->width < 512) return;
    GenerateVaults(world);
    GenerateMines(world);
    GenerateReliquaries(world);
}

/* ---- islands in the sky ---------------------------------------------------- */

/* The top of an island's column: the first solid cell at or below `fromY`,
   within `reach`. The ground search the rest of the generator uses starts
   at the ground band and would never see an island in space. */
static int IslandTopY(const World *world, int x, int fromY, int reach)
{
    int y;

    for (y = fromY; y < fromY + reach && y < world->height; ++y) {
        if (MaterialIsSolid(WorldMaterialAt(world, x, y))) {
            return y;
        }
    }
    return -1;
}

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
        int top = IslandTopY(world, centerX + x, baseY - 12, 24);

        if (top > 0 && top < baseY + 4) {
            WorldGenPlaceTree(world, centerX + x, top, rng);
        }
    }
    /* Grass on every piece of the top still open to the sky. */
    for (x = -halfWidth; x <= halfWidth; ++x) {
        int top = IslandTopY(world, centerX + x, baseY - 12, 24);

        if (top > 0 && top < baseY + 12 &&
            WorldMaterialAt(world, centerX + x, top) == MATERIAL_DIRT &&
            WorldMaterialAt(world, centerX + x, top - 1) == MATERIAL_EMPTY) {
            WorldGenGrowGrass(world, centerX + x, top, rng);
        }
    }
    /* On some, what the precursors left up here: an obelisk lamp on a
       plinth of relic stone, still lit. */
    if (RngRange(rng, 0, 99) < 45) {
        int shrineX = centerX + RngRange(rng, -halfWidth / 3, halfWidth / 3);
        int ground = IslandTopY(world, shrineX, baseY - 12, 24);

        if (ground > 0) {
            if (WorldMaterialAt(world, shrineX, ground) == MATERIAL_GRASS) ++ground;
            StructureFill(world, shrineX - 14, ground - 3, shrineX + 14, ground - 1,
                          MATERIAL_RELIC);
            StructureFill(world, shrineX - 14, ground - 3, shrineX + 14, ground + 3,
                          MATERIAL_RELIC);
            StructureObeliskLamp(world, shrineX, ground - 3, RngRange(rng, 30, 44), 4,
                                 MATERIAL_RELIC, MATERIAL_LUMEN);
            StructureSweep(world, shrineX - 16, ground - 60, shrineX + 16, ground + 4);
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
        int lowest;
        int baseY;

        if (RngRange(&rng, 0, 99) >= 70) continue;
        /* Only where nothing pulls: above the space line, the whole island
           — its trees on top and its hanging underside — clear of the top
           of the world and of the band where the pull begins. An island in
           the air under the clouds would be the one thing there that does
           not fall. */
        lowest = (int)WorldSpaceLineY(world) - depth - 24;
        if (lowest < 90) continue;
        baseY = RngRange(&rng, 90, lowest);
        PlaceIsland(world, &rng, centerX, baseY, halfWidth, depth);
    }
}
