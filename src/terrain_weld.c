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
    /* Half a minute of hardly moving, a minute out of sight. */
    config.quietDelay = 30.0f;
    config.quietSpeed = 4.0f;
    config.quietSpin = 0.3f;
    config.awayDelay = 60.0f;
    /* Past the edge of the view (426 by 240) with a margin. */
    config.awayX = 320.0f;
    config.awayY = 200.0f;
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

static bool TerrainWeldBody(TerrainWeldSystem *system, World *world,
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
    /* Rows only: the world wraps, and a body lying across the seam is
       written into both sides of it. */
    if (firstY < 0) firstY = 0;
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
            /* A plant under a rock is crushed by it: bodies fall through
               flora, so a slab at rest on a meadow lies among grass blades,
               and refusing it for them kept it a body for ever. */
            if (there == MATERIAL_EMPTY || MaterialIsFlora(there)) {
                continue;
            }
            if (!MaterialIsLiquid(there) ||
                !TerrainWeldLiquidCanRise(world, worldX, worldY)) {
                ++system->stats.cellsRefused;
                ++system->stats.bodiesRefused;
                system->rested[slot] = system->config.weldDelay * 0.5f;
                system->quiet[slot] *= 0.5f;
                system->away[slot] *= 0.5f;
                return false;
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
            if (MaterialIsFlora(there)) {
                WorldSetCell(world, worldX, worldY, MATERIAL_EMPTY);
                continue;
            }
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
            /* Its tone comes back with it: a slab welded where it landed
               looks like the slab, not like fresh ground of its material. */
            WorldSetShade(world, worldX, worldY,
                          DynamicTerrainShadeAt(terrain, handle,
                                                (int)floorf(local.x),
                                                (int)floorf(local.y)));
            ++system->stats.cellsWelded;
        }
    }

    DynamicTerrainFreeBody(terrain, handle);
    system->rested[slot] = 0.0f;
    system->quiet[slot] = 0.0f;
    system->away[slot] = 0.0f;
    system->restedGeneration[slot] = 0u;
    ++system->stats.bodiesWelded;
    return true;
}

/* Whether a static solid cell holds the body up: one right under any of its
   lowest cells. Liquid does not: a body floating on a lake is not lying on
   anything. */
static bool TerrainWeldIsSupported(const DynamicTerrainSystem *terrain, int slot,
                                   const TerrainBody *body, const World *world)
{
    size_t surfaceBase = TerrainSlotSurfaceBase(slot);
    int index;

    for (index = 0; index < body->surfaceCount; ++index) {
        Vector2 at = TerrainBodyLocalToWorld(
            body, (float)terrain->surfaceX[surfaceBase + (size_t)index] + 0.5f,
            (float)terrain->surfaceY[surfaceBase + (size_t)index] + 1.5f);
        int gap;

        /* Two rows: a body at rest sits within a contact's slop of what
           holds it, not always flush on it. */
        for (gap = 0; gap < 2; ++gap) {
            CellMaterial below =
                WorldGetCell(world, (int)floorf(at.x), (int)floorf(at.y) + gap);

            if (WorldMaterialIsSolid(below) && !MaterialIsFlora(below) &&
                !MaterialIsDynamic(below)) {
                return true;
            }
        }
    }
    return false;
}

/* How far the body can drop straight down before a cell of it would meet
   the ground — through water and air and plants, which a lowered body
   passes as a falling one would. Asked per column of the body, from its
   lowest cell there, and bounded. */
#define TERRAIN_WELD_MAX_DROP 1200

static int TerrainWeldDropDistance(const DynamicTerrainSystem *terrain, int slot,
                                   const TerrainBody *body, const World *world)
{
    size_t surfaceBase = TerrainSlotSurfaceBase(slot);
    int drop = TERRAIN_WELD_MAX_DROP;
    int index;

    for (index = 0; index < body->surfaceCount; ++index) {
        Vector2 at = TerrainBodyLocalToWorld(
            body, (float)terrain->surfaceX[surfaceBase + (size_t)index] + 0.5f,
            (float)terrain->surfaceY[surfaceBase + (size_t)index] + 0.5f);
        int x = (int)floorf(at.x);
        int y = (int)floorf(at.y);
        int fall;

        /* A cell with more of the body under it meets the ground later
           than that one does, so it never sets the minimum; the world is
           read, not the body, and the body is not in the world. */
        for (fall = 0; fall < drop; ++fall) {
            CellMaterial below = WorldGetCell(world, x, y + fall + 1);

            if (WorldMaterialIsSolid(below) && !MaterialIsFlora(below) &&
                !MaterialIsDynamic(below)) {
                break;
            }
        }
        if (fall < drop) drop = fall;
    }
    return drop;
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
            system->quiet[slot] = 0.0f;
            system->away[slot] = 0.0f;
            system->restedGeneration[slot] = 0u;
            continue;
        }
        /* A reused slot holds a different body, and it has rested for no time
           at all whatever the previous tenant had accumulated. */
        if (system->restedGeneration[slot] != body->generation) {
            system->restedGeneration[slot] = body->generation;
            system->rested[slot] = 0.0f;
            system->quiet[slot] = 0.0f;
            system->away[slot] = 0.0f;
        }
        /* Far from the character: out of sight. Counted whatever the body
           is doing, because what never ends out there is exactly a body
           that keeps a small motion for ever and never sleeps. */
        if (fabsf(body->position.x - playerAt.x) > system->config.awayX ||
            fabsf(body->position.y - playerAt.y) > system->config.awayY) {
            system->away[slot] += deltaTime;
        } else {
            system->away[slot] = 0.0f;
        }
        if (body->awake) {
            float speed = sqrtf(body->velocity.x * body->velocity.x +
                                body->velocity.y * body->velocity.y);

            system->rested[slot] = 0.0f;
            /* Awake but hardly moving — a jitter under a pile, a rock that
               rocks — counts as lying still, only more slowly. */
            if (speed < system->config.quietSpeed &&
                fabsf(body->angularVelocity) < system->config.quietSpin) {
                system->quiet[slot] += deltaTime;
            } else {
                system->quiet[slot] = 0.0f;
            }
        } else {
            system->rested[slot] += deltaTime;
            system->quiet[slot] += deltaTime;
        }

        if (system->away[slot] >= system->config.awayDelay) {
            /* Out of sight for long: it goes down to whatever solid ground
               is under it — to the sea bed from the surface of the sea —
               and becomes part of it there. */
            int drop;

            if (welded >= system->config.maxWeldsPerTick) {
                ++system->stats.bodiesDeferredByBudget;
                continue;
            }
            drop = TerrainWeldDropDistance(terrain, slot, body, world);
            if (drop >= TERRAIN_WELD_MAX_DROP) {
                system->away[slot] = 0.0f;
                continue;
            }
            body->position.y += (float)drop;
            body->velocity = (Vector2){0.0f, 0.0f};
            body->angularVelocity = 0.0f;
            if (TerrainWeldBody(system, world, terrain, slot)) {
                ++system->stats.bodiesSettledAway;
                ++welded;
            }
            continue;
        }
        if (system->quiet[slot] >= system->config.quietDelay &&
            system->rested[slot] < system->config.weldDelay) {
            /* Lying still for long without ever falling asleep: welded
               where it lies, when it lies on something. */
            if (!TerrainWeldIsSupported(terrain, slot, body, world)) {
                continue;
            }
            system->rested[slot] = system->config.weldDelay;
        }
        if (system->rested[slot] < system->config.weldDelay) {
            continue;
        }
        /* Asleep on the water is not lying on the ground: welded where it
           floats, it was a slab of static wood standing on the sea. It stays
           a body until it is out of sight, and then goes to the bottom. */
        if ((body->inLiquid || body->submerged > 0.0f) &&
            !TerrainWeldIsSupported(terrain, slot, body, world)) {
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
        if (TerrainWeldBody(system, world, terrain, slot)) {
            ++welded;
        }
    }
    return welded;
}
