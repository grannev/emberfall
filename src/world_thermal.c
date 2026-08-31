/* Heat transport, phase changes and the water/lava reaction.
 *
 * Temperature decides what a cell *is*; world_simulation.c decides where it
 * goes. They are separate because the laser, the cryo beam and the drill all
 * need to change a cell's state without running a movement step, and because
 * the containment rules — a lava pocket must not melt its own lining, one fire
 * must not consume a connected dirt field — live entirely on this side.
 */
#include "world_thermal.h"

#include "world_internal.h"

/* cap <= 0 means the source imposes no ceiling of its own. */
void WorldHeatNeighbors(World *world, int x, int y, float heat, float cap)
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
            WorldMaterialAt(world, targetX, targetY) != MATERIAL_EMPTY) {
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

bool WorldTryThermalTransition(World *world, int x, int y)
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
    WorldCell(world, x, y)->updatedTick = WorldTickStamp(world);
    return true;
}

bool WorldUpdateTemperatureState(World *world, int x, int y)
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

void WorldBurnDirt(World *world, int x, int y)
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
            WorldMaterialAt(world, targetX, targetY) == MATERIAL_DIRT) {
            WorldSetCellRaw(world, targetX, targetY, MATERIAL_FIRE);
            WorldCell(world, targetX, targetY)->updatedTick = WorldTickStamp(world);
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

bool WorldTryMaterialReaction(World *world, int x, int y)
{
    static const int offsets[8][2] = {
        {0, 1}, {1, 0}, {0, -1}, {-1, 0},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1}
    };
    CellMaterial material = WorldMaterialAt(world, x, y);
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
            WorldMaterialAt(world, targetX, targetY) != targetMaterial) {
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
        WorldCell(world, lavaX, lavaY)->updatedTick = WorldTickStamp(world);
        WorldCell(world, waterX, waterY)->updatedTick = WorldTickStamp(world);
        WorldRecordReaction(world, waterX, waterY, lavaX, lavaY);
        return true;
    }

    return false;
}
