/* Welding settled bodies back into the world. See terrain_weld.h. */
#include "terrain_weld.h"

#include "materials.h"

#include <math.h>
#include <string.h>

TerrainWeldConfig TerrainWeldDefaultConfig(void)
{
    TerrainWeldConfig config;

    /* Six seconds of stillness. A slab that fell out of a blast is usually
       asleep inside one, so this is five seconds of the player being able to
       shoot it, push it or pick it up again before it becomes ground. */
    config.weldDelay = 6.0f;
    config.maxWeldsPerTick = 2;
    /* Wider than the player is, so a weld cannot close on them even if they
       are moving into it as it happens. */
    config.playerClearance = 24.0f;
    return config;
}

void TerrainWeldInit(TerrainWeldSystem *system)
{
    if (system == NULL) {
        return;
    }
    memset(system, 0, sizeof(*system));
    system->config = TerrainWeldDefaultConfig();
}

void TerrainWeldResetStats(TerrainWeldSystem *system)
{
    if (system == NULL) {
        return;
    }
    memset(&system->stats, 0, sizeof(system->stats));
}

/* Does the body's footprint reach the box kept clear around the player?

   Tested against the bounding radius rather than the raster: it is the cheap
   conservative answer, and being conservative here means waiting a few more
   seconds, which costs nothing. */
static bool TerrainWeldTouchesPlayer(const TerrainBody *body, Vector2 playerAt,
                                     float clearance)
{
    float dx = body->position.x - playerAt.x;
    float dy = body->position.y - playerAt.y;
    float reach = body->boundingRadius + clearance;

    return dx * dx + dy * dy <= reach * reach;
}

/* Writes one body's occupied cells into the world and frees it.
 *
 * The mapping runs backwards, from the world into the body, and that is the
 * whole of why it is correct. Walking the body's cells and rounding each one
 * into the world — the obvious direction — is not onto: a rotated square of
 * cells is not a square of cells, two source cells land on one destination and
 * a third destination is named by none, so the welded rubble came out full of
 * single-cell holes. Asking every world cell in the footprint which body cell
 * covers it gives every destination exactly one answer, and the result is
 * solid.
 */
/* How far up through water a body's cell may push the water it displaces.
   A body welded into a flooded cave with no air over it would have to
   destroy the water it lies in, and it is left a body instead. */
#define TERRAIN_WELD_LIQUID_REACH 512

/* True when the liquid at (x, y) has an empty cell above it within reach. */
static bool TerrainWeldLiquidCanRise(const World *world, int x, int y)
{
    int probe;

    for (probe = y - 1; probe >= 0 && probe >= y - TERRAIN_WELD_LIQUID_REACH; --probe) {
        CellMaterial material = WorldGetCell(world, x, probe);

        if (material == MATERIAL_EMPTY) {
            return true;
        }
        if (!MaterialIsLiquid(material)) {
            return false;
        }
    }
    return false;
}

static void TerrainWeldBody(TerrainWeldSystem *system, World *world,
                            DynamicTerrainSystem *terrain, int slot)
{
    TerrainBody *body = &terrain->bodies[slot];
    TerrainBodyHandle handle;
    Vector2 corner[4];
    float minimumX;
    float minimumY;
    float maximumX;
    float maximumY;
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    int index;
    int worldY;

    handle.index = (uint16_t)slot;
    handle.generation = body->generation;

    /* World-space bounds of the body's local bounding box, from its four
       transformed corners. A circle of the bounding radius would also cover it
       and would be several times the area for a long thin slab. */
    corner[0] = TerrainBodyLocalToWorld(body, (float)body->minimumX,
                                        (float)body->minimumY);
    corner[1] = TerrainBodyLocalToWorld(body, (float)body->maximumX + 1.0f,
                                        (float)body->minimumY);
    corner[2] = TerrainBodyLocalToWorld(body, (float)body->minimumX,
                                        (float)body->maximumY + 1.0f);
    corner[3] = TerrainBodyLocalToWorld(body, (float)body->maximumX + 1.0f,
                                        (float)body->maximumY + 1.0f);
    minimumX = maximumX = corner[0].x;
    minimumY = maximumY = corner[0].y;
    for (index = 1; index < 4; ++index) {
        if (corner[index].x < minimumX) minimumX = corner[index].x;
        if (corner[index].x > maximumX) maximumX = corner[index].x;
        if (corner[index].y < minimumY) minimumY = corner[index].y;
        if (corner[index].y > maximumY) maximumY = corner[index].y;
    }
    firstX = (int)floorf(minimumX) - 1;
    firstY = (int)floorf(minimumY) - 1;
    lastX = (int)floorf(maximumX) + 1;
    lastY = (int)floorf(maximumY) + 1;
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > world->width - 1) lastX = world->width - 1;
    if (lastY > world->height - 1) lastY = world->height - 1;

    /* Two passes. The first asks whether every cell of the body can be
       given back: into an empty cell outright, or into a liquid cell whose
       water has somewhere to go — the first empty cell above it, where it
       is moved before the weld writes. A body on a lake bed used to have
       every cell refused, since none of them were empty, and was freed
       anyway: it vanished into the water, and the player saw it. Now a body
       with even one cell that cannot be placed stays a body and is asked
       again later. */
    for (worldY = firstY; worldY <= lastY; ++worldY) {
        int worldX;

        for (worldX = firstX; worldX <= lastX; ++worldX) {
            Vector2 local = TerrainBodyWorldToLocal(body, (float)worldX + 0.5f,
                                                    (float)worldY + 0.5f);
            CellMaterial material =
                DynamicTerrainCellAt(terrain, handle, (int)floorf(local.x),
                                     (int)floorf(local.y));
            CellMaterial there;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            there = WorldGetCell(world, worldX, worldY);
            if (there == MATERIAL_EMPTY) {
                continue;
            }
            if (!MaterialIsLiquid(there) ||
                !TerrainWeldLiquidCanRise(world, worldX, worldY)) {
                ++system->stats.cellsRefused;
                ++system->stats.bodiesRefused;
                system->rested[slot] = system->config.weldDelay * 0.5f;
                return;
            }
        }
    }
    /* The second lifts the water out of every cell the body will take,
       bottom row first: the water above a cell is still water when the cell
       is lifted, so it rises through the body's whole footprint to the
       surface instead of into the cell just emptied above it. */
    for (worldY = lastY; worldY >= firstY; --worldY) {
        int worldX;

        for (worldX = firstX; worldX <= lastX; ++worldX) {
            Vector2 local = TerrainBodyWorldToLocal(body, (float)worldX + 0.5f,
                                                    (float)worldY + 0.5f);
            CellMaterial material =
                DynamicTerrainCellAt(terrain, handle, (int)floorf(local.x),
                                     (int)floorf(local.y));
            CellMaterial there;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            there = WorldGetCell(world, worldX, worldY);
            if (there != MATERIAL_EMPTY &&
                !WorldLiftLiquidOut(world, worldX, worldY,
                                    TERRAIN_WELD_LIQUID_REACH)) {
                /* The first pass promised this cannot happen; counted so a
                   broken promise shows in the stats rather than in a lake. */
                ++system->stats.cellsRefused;
                continue;
            }
        }
    }
    /* Every cell is now empty, and only now is the body written: written a
       row at a time, the row above would have sealed the water under it in
       before it could rise. */
    for (worldY = firstY; worldY <= lastY; ++worldY) {
        int worldX;

        for (worldX = firstX; worldX <= lastX; ++worldX) {
            Vector2 local = TerrainBodyWorldToLocal(body, (float)worldX + 0.5f,
                                                    (float)worldY + 0.5f);
            CellMaterial material =
                DynamicTerrainCellAt(terrain, handle, (int)floorf(local.x),
                                     (int)floorf(local.y));

            if (material == MATERIAL_EMPTY ||
                WorldGetCell(world, worldX, worldY) != MATERIAL_EMPTY) {
                continue;
            }
            WorldSetCell(world, worldX, worldY, material);
            ++system->stats.cellsWelded;
        }
    }

    DynamicTerrainFreeBody(terrain, handle);
    system->rested[slot] = 0.0f;
    system->restedGeneration[slot] = 0u;
    ++system->stats.bodiesWelded;
}

int TerrainWeldProcess(TerrainWeldSystem *system, World *world,
                       DynamicTerrainSystem *terrain, Vector2 playerAt,
                       float deltaTime)
{
    int welded = 0;
    int slot;

    if (system == NULL || world == NULL || world->cells == NULL ||
        terrain == NULL || !(deltaTime > 0.0f)) {
        return 0;
    }

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &terrain->bodies[slot];

        if (!body->active) {
            system->rested[slot] = 0.0f;
            system->restedGeneration[slot] = 0u;
            continue;
        }
        /* A reused slot holds a different body, and it has rested for no time
           at all whatever the previous tenant had accumulated. */
        if (system->restedGeneration[slot] != body->generation) {
            system->restedGeneration[slot] = body->generation;
            system->rested[slot] = 0.0f;
        }
        if (body->awake) {
            system->rested[slot] = 0.0f;
            continue;
        }

        system->rested[slot] += deltaTime;
        if (system->rested[slot] < system->config.weldDelay) {
            continue;
        }
        if (welded >= system->config.maxWeldsPerTick) {
            ++system->stats.bodiesDeferredByBudget;
            continue;
        }
        if (TerrainWeldTouchesPlayer(body, playerAt,
                                     system->config.playerClearance)) {
            ++system->stats.bodiesDeferredByPlayer;
            continue;
        }
        TerrainWeldBody(system, world, terrain, slot);
        ++welded;
    }
    return welded;
}
