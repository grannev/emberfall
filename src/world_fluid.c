/* Pressure and momentum in a liquid. See world_fluid.h for the model; this
 * file records the decisions inside it.
 */
#include "world_fluid.h"

#include <math.h>
#include <stddef.h>

/* The head of a liquid cell. A cell with rock over it stores its own; a
   cell with liquid over it is one cell deeper than the cell above, and its
   head is read by walking up its column to the first cell that has something
   other than liquid over it — a free surface, which is zero, or a roof, which
   stores what its neighbours push it with. Nothing under liquid is ever
   written: storing the chain meant every grain that slid onto a column of
   the ocean rewrote a hundred cells under it and woke four chunk rows that
   had nothing to do, and one blast in the sea cost eight milliseconds a tick
   for two hundred ticks. The walk is bounded, and a column deeper than the
   bound presses no harder than the bound. */
static int32_t WorldFluidHeadAt(const World *world, int x, int y)
{
    int depth = 0;

    while (depth < WORLD_LIQUID_CHAIN_REACH &&
           MaterialIsLiquid(WorldMaterialAt(world, x, y - depth - 1))) {
        ++depth;
    }
    if (MaterialIsSolid(WorldMaterialAt(world, x, y - depth - 1))) {
        return (int32_t)WorldLiquidHead(WorldCellConst(world, x, y - depth)) +
               (int32_t)WORLD_LIQUID_HEAD_PER_CELL * depth;
    }
    return (int32_t)WORLD_LIQUID_HEAD_PER_CELL * depth;
}

/* Stores a head, and wakes the neighbourhood when it changed: a changing
   head is a pressure wave on its way through the pool, and the cells it will
   reach next have to be awake to carry it. */
static void WorldFluidStoreHead(World *world, int x, int y, int32_t head)
{
    Cell *cell = WorldCell(world, x, y);

    if (head < 0) head = 0;
    if (head > (int32_t)WORLD_LIQUID_HEAD_MAX) head = (int32_t)WORLD_LIQUID_HEAD_MAX;
    if ((uint32_t)head != WorldLiquidHead(cell)) {
        WorldLiquidSetHead(cell, (uint32_t)head);
        ++world->fluid.headChanges;
        WorldWakeCellAndNeighbors(world, x, y);
    }
}

/* The most head the cells beside and below (x, y) offer it, each less the
   loss its route costs. */
static int32_t WorldFluidOffered(const World *world, int x, int y)
{
    int32_t offered = 0;
    int32_t candidate;

    if (MaterialIsLiquid(WorldMaterialAt(world, x - 1, y))) {
        candidate = WorldFluidHeadAt(world, x - 1, y) - (int32_t)WORLD_LIQUID_HEAD_LOSS;
        if (candidate > offered) offered = candidate;
    }
    if (MaterialIsLiquid(WorldMaterialAt(world, x + 1, y))) {
        candidate = WorldFluidHeadAt(world, x + 1, y) - (int32_t)WORLD_LIQUID_HEAD_LOSS;
        if (candidate > offered) offered = candidate;
    }
    if (MaterialIsLiquid(WorldMaterialAt(world, x, y + 1))) {
        candidate = WorldFluidHeadAt(world, x, y + 1) -
                    (int32_t)(WORLD_LIQUID_HEAD_PER_CELL + WORLD_LIQUID_HEAD_LOSS);
        if (candidate > offered) offered = candidate;
    }
    return offered;
}

uint32_t WorldFluidUpdateHead(World *world, int x, int y)
{
    /* Only a cell with rock over it keeps a head of its own: the roof of a
       channel, the ceiling of a sealed cave. That is where pressure travels
       sideways, and it is a row of cells rather than a lake of them. */
    WorldFluidStoreHead(world, x, y, WorldFluidOffered(world, x, y));
    return WorldLiquidHead(WorldCellConst(world, x, y));
}

/* How much a column's neighbour at (x, y) pushes a column whose surface is
   `surfaceY`, over what the column's own depth there explains. A neighbour
   under its own free surface pushes by how much higher that surface stands —
   read at the surface, once, rather than at every depth — and a neighbour
   with rock over it pushes with the head it stores. Under liquid without a
   free surface the chain goes up to a roof, and the roof's head is what it
   says. */
static int32_t WorldFluidPushFrom(const World *world, int x, int y, int surfaceY)
{
    CellMaterial material = WorldMaterialAt(world, x, y);
    int32_t head;

    if (!MaterialIsLiquid(material)) {
        return 0;
    }
    head = WorldFluidHeadAt(world, x, y);
    return head - (int32_t)WORLD_LIQUID_HEAD_LOSS -
           (int32_t)WORLD_LIQUID_HEAD_PER_CELL * (y - surfaceY);
}

bool WorldFluidSurfaceStep(World *world, int x, int y)
{
    CellMaterial material = WorldMaterialAt(world, x, y);
    int32_t bottomHead;
    int32_t excess;
    int depth = 0;
    int pushedDepth = 0;

    /* The surface is the reference: no head, whatever the cell remembered. */
    WorldFluidStoreHead(world, x, y, 0);
    /* What the two neighbouring columns stand at, read once: a neighbour
       whose surface is higher pushes every cell of this column by the
       difference, and that is the same number at every depth. */
    excess = WorldFluidPushFrom(world, x - 1, y, y);
    {
        int32_t right = WorldFluidPushFrom(world, x + 1, y, y);

        if (right > excess) excess = right;
    }
    /* Down the column for pushes that enter its side from under a roof — a
       pipe into it — which only a cell with rock over it can deliver. Those
       are found by their roof, two material reads a cell, and their head is
       read only when one is found. */
    while (depth < WORLD_LIQUID_COLUMN_REACH &&
           WorldMaterialAt(world, x, y + depth + 1) == material) {
        int side;

        ++depth;
        for (side = -1; side <= 1; side += 2) {
            int probeX = x + side;

            if (MaterialIsLiquid(WorldMaterialAt(world, probeX, y + depth)) &&
                MaterialIsSolid(WorldMaterialAt(world, probeX, y + depth - 1))) {
                int32_t offered = WorldFluidPushFrom(world, probeX, y + depth, y);

                if (offered > excess) {
                    excess = offered;
                    pushedDepth = depth;
                }
            }
        }
    }
    depth = pushedDepth;
    bottomHead = excess + (int32_t)WORLD_LIQUID_HEAD_PER_CELL * depth;
    /* Pushed by more than its own depth explains, and room above to rise
       into: the bottom of the column is lifted to the top, and its place is
       taken by the liquid that is pushing — found by following the head
       uphill from the bottom, neighbour to neighbour, to where it stops
       rising, which is the foot of the surface that stands highest. Two swaps
       and a walk, so the hole opens under that surface and it drops, while
       nothing between the two has to move: every cell of a liquid is the same
       liquid. The hole used to open under the column that had just risen,
       and the column fell straight back into it. */
    if (bottomHead - (int32_t)WORLD_LIQUID_HEAD_PER_CELL * depth <
            (int32_t)WORLD_LIQUID_LIFT_THRESHOLD ||
        WorldMaterialAt(world, x, y - 1) != MATERIAL_EMPTY) {
        return false;
    }
    {
        static const int offsets[4][2] = {{-1, 0}, {1, 0}, {0, 1}, {0, -1}};
        int bottomY = y + depth;
        int sourceX = x;
        int sourceY = bottomY;
        int32_t sourceHead = bottomHead;
        int steps;

        for (steps = 0; steps < WORLD_LIQUID_SOURCE_REACH; ++steps) {
            int bestX = sourceX;
            int bestY = sourceY;
            int32_t best = sourceHead;
            int i;

            for (i = 0; i < 4; ++i) {
                int probeX = sourceX + offsets[i][0];
                int probeY = sourceY + offsets[i][1];
                int32_t candidate;

                if (WorldMaterialAt(world, probeX, probeY) != material) {
                    continue;
                }
                /* Seen from the candidate's own row: a cell above holds
                   less head for the same push by exactly one cell's worth,
                   so that is added back before comparing, or the walk would
                   never climb a shaft. */
                candidate = WorldFluidHeadAt(world, probeX, probeY) +
                            (int32_t)WORLD_LIQUID_HEAD_PER_CELL * (sourceY - probeY);
                if (candidate > best) {
                    best = candidate;
                    bestX = probeX;
                    bestY = probeY;
                }
            }
            if (bestX == sourceX && bestY == sourceY) {
                break;
            }
            sourceX = bestX;
            sourceY = bestY;
            sourceHead = best;
        }
        if (sourceX == x && sourceY == bottomY) {
            return false;
        }
        WorldMoveCell(world, x, bottomY, x, y - 1);
        WorldMoveCell(world, sourceX, sourceY, x, bottomY);
    }
    /* A fresh surface cell: no wander yet, and no head. */
    WorldCell(world, x, y - 1)->lifetime = 0u;
    ++world->fluid.lifts;
    return true;
}

bool WorldFluidMayFlowToward(const World *world, int x, int y, int direction,
                             int distance)
{
    int otherX = x + (distance + 1) * direction;
    const Cell *other;

    if (!WorldInBounds(world, otherX, y)) {
        return true;
    }
    other = WorldCellConst(world, otherX, y);
    if (!MaterialIsLiquid((CellMaterial)other->material)) {
        return true;
    }
    return WorldFluidHeadAt(world, otherX, y) < WorldFluidHeadAt(world, x, y);
}

/* --- impulses ------------------------------------------------------------ */

static bool WorldFluidPassable(CellMaterial material)
{
    return material == MATERIAL_EMPTY ||
           (!MaterialIsSolid(material) && !MaterialIsLiquid(material));
}

/* Turns an impulse that has run into something solid. A horizontal push
   turns up before down — that is the splash a blow against a wall throws —
   and a vertical one turns to the side its own sign points, so a column of
   impulses does not all pick the same side. Returns false when there is
   nowhere to turn. */
static bool WorldFluidDeflect(const World *world, WorldFluidImpulse *impulse)
{
    int candidates[2][2];
    int i;

    if (impulse->directionX != 0) {
        candidates[0][0] = 0; candidates[0][1] = -1;
        candidates[1][0] = 0; candidates[1][1] = 1;
    } else {
        int side = ((impulse->x + impulse->y) & 1) != 0 ? 1 : -1;

        candidates[0][0] = side; candidates[0][1] = 0;
        candidates[1][0] = -side; candidates[1][1] = 0;
    }
    for (i = 0; i < 2; ++i) {
        CellMaterial material = WorldMaterialAt(world, impulse->x + candidates[i][0],
                                                impulse->y + candidates[i][1]);

        if (WorldFluidPassable(material) || MaterialIsLiquid(material)) {
            impulse->directionX = (int8_t)candidates[i][0];
            impulse->directionY = (int8_t)candidates[i][1];
            impulse->strength = (uint8_t)(impulse->strength / 2u);
            return impulse->strength > 0u;
        }
    }
    return false;
}

/* One step of one impulse. Returns whether it is still alive. */
static bool WorldFluidAdvance(World *world, WorldFluidImpulse *impulse)
{
    int targetX;
    int targetY;
    CellMaterial target;

    if (!WorldInBounds(world, impulse->x, impulse->y) || impulse->strength == 0u) {
        return false;
    }
    if (!MaterialIsLiquid(WorldMaterialAt(world, impulse->x, impulse->y))) {
        /* The liquid this was pushing is no longer here — it reacted, froze
           or was carried off by something else — and an impulse without a
           cell is nothing. */
        return false;
    }
    targetX = impulse->x + impulse->directionX;
    targetY = impulse->y + impulse->directionY;
    if (!WorldInBounds(world, targetX, targetY)) {
        return false;
    }
    target = WorldMaterialAt(world, targetX, targetY);
    if (WorldFluidPassable(target)) {
        WorldMoveCell(world, impulse->x, impulse->y, targetX, targetY);
        impulse->x = targetX;
        impulse->y = targetY;
        ++world->fluid.impulseMoves;
    } else if (MaterialIsLiquid(target)) {
        /* Handed on: the cell ahead is pushed next, which is how a push
           travels through a pool as a pressure wave rather than stopping at
           the first cell that could not move. */
        impulse->x = targetX;
        impulse->y = targetY;
        WorldWakeCellAndNeighbors(world, targetX, targetY);
    } else if (!WorldFluidDeflect(world, impulse)) {
        return false;
    }
    --impulse->strength;
    return impulse->strength > 0u;
}

void WorldFluidStepImpulses(World *world)
{
    int index = 0;

    world->fluid.impulseMoves = 0;
    while (index < world->fluid.impulsesActive) {
        WorldFluidImpulse *impulse = &world->fluidImpulses[index];

        if (WorldFluidAdvance(world, impulse)) {
            ++index;
            continue;
        }
        /* Swap-remove: the entry moved into this slot has not had its step
           yet and is looked at next. Deterministic, since the order is a
           function of the state and nothing else. */
        --world->fluid.impulsesActive;
        *impulse = world->fluidImpulses[world->fluid.impulsesActive];
    }
}

bool WorldPushLiquid(World *world, int x, int y, int directionX, int directionY,
                     int strength)
{
    WorldFluidImpulse *impulse;

    if (world == NULL || world->cells == NULL) {
        return false;
    }
    if (directionX < -1 || directionX > 1 || directionY < -1 || directionY > 1 ||
        (directionX == 0 && directionY == 0) || strength <= 0) {
        return false;
    }
    if (!WorldInBounds(world, x, y) ||
        !MaterialIsLiquid(WorldMaterialAt(world, x, y))) {
        return false;
    }
    if (world->fluid.impulsesActive >= MAX_WORLD_FLUID_IMPULSES) {
        ++world->fluid.impulsesRefused;
        return false;
    }
    impulse = &world->fluidImpulses[world->fluid.impulsesActive++];
    impulse->x = x;
    impulse->y = y;
    impulse->directionX = (int8_t)directionX;
    impulse->directionY = (int8_t)directionY;
    impulse->strength = (uint8_t)(strength > 255 ? 255 : strength);
    WorldWakeCellAndNeighbors(world, x, y);
    return true;
}

int WorldPushLiquidRadial(World *world, Vector2 centre, float radius,
                          int strength)
{
    int pushed = 0;
    int centreX;
    int centreY;
    int extent;
    int y;

    if (world == NULL || world->cells == NULL || !(radius > 0.0f) || strength <= 0) {
        return 0;
    }
    if (radius > 96.0f) {
        radius = 96.0f;
    }
    centreX = (int)floorf(centre.x);
    centreY = (int)floorf(centre.y);
    extent = (int)ceilf(radius);
    for (y = centreY - extent; y <= centreY + extent; ++y) {
        int x;

        for (x = centreX - extent; x <= centreX + extent; ++x) {
            float dx = (float)x + 0.5f - centre.x;
            float dy = (float)y + 0.5f - centre.y;
            float distance = sqrtf(dx * dx + dy * dy);
            int directionX;
            int directionY;
            int steps;

            if (distance > radius || !WorldInBounds(world, x, y) ||
                !MaterialIsLiquid(WorldMaterialAt(world, x, y))) {
                continue;
            }
            /* Eight directions: the nearer axis, plus the other when the
               push is within thirty degrees of the diagonal. */
            directionX = fabsf(dx) * 2.0f > fabsf(dy) ? (dx < 0.0f ? -1 : 1) : 0;
            directionY = fabsf(dy) * 2.0f > fabsf(dx) ? (dy < 0.0f ? -1 : 1) : 0;
            if (directionX == 0 && directionY == 0) {
                directionY = -1;
            }
            steps = 1 + (int)((float)strength * (1.0f - distance / radius));
            if (WorldPushLiquid(world, x, y, directionX, directionY, steps)) {
                ++pushed;
            }
        }
    }
    return pushed;
}

int WorldSplashLiquid(World *world, Vector2 centre, float radius, int strength)
{
    int pushed = 0;
    int centreX;
    int centreY;
    int extent;
    int y;

    if (world == NULL || world->cells == NULL || !(radius > 0.0f) || strength <= 0) {
        return 0;
    }
    if (radius > 96.0f) {
        radius = 96.0f;
    }
    centreX = (int)floorf(centre.x);
    centreY = (int)floorf(centre.y);
    extent = (int)ceilf(radius);
    for (y = centreY - extent; y <= centreY + extent; ++y) {
        int x;

        for (x = centreX - extent; x <= centreX + extent; ++x) {
            float dx = (float)x + 0.5f - centre.x;
            float dy = (float)y + 0.5f - centre.y;
            float distance = sqrtf(dx * dx + dy * dy);
            float falloff;
            int directionX;
            int directionY;
            int steps;

            if (distance > radius || !WorldInBounds(world, x, y) ||
                !MaterialIsLiquid(WorldMaterialAt(world, x, y))) {
                continue;
            }
            falloff = 1.0f - distance / radius;
            if (fabsf(dx) >= radius * 0.3f) {
                /* The ring around what fell in, the whole depth of it: up,
                   and outward the further from the centre, so the crown
                   leans away from the impact. Column after column rises,
                   which is what makes it a crown and not a sheet. */
                directionX = fabsf(dx) > radius * 0.65f ? (dx < 0.0f ? -1 : 1) : 0;
                directionY = -1;
                steps = 1 + (int)((float)strength * (0.5f + 0.5f * falloff));
            } else {
                /* Straight under it: shoved out of the way and down, which
                   is where the mass that went in has to go. */
                directionX = dx < 0.0f ? -1 : 1;
                directionY = dy > 0.0f ? 1 : 0;
                steps = 1 + (int)((float)strength * 0.5f * falloff);
            }
            if (WorldPushLiquid(world, x, y, directionX, directionY, steps)) {
                ++pushed;
            }
        }
    }
    return pushed;
}
