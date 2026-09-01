/* Welding settled bodies back into the world. See terrain_weld.h. */
#include "terrain_weld.h"

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

/* Writes one body's occupied cells into the world and frees it. */
static void TerrainWeldBody(TerrainWeldSystem *system, World *world,
                            DynamicTerrainSystem *terrain, int slot)
{
    TerrainBody *body = &terrain->bodies[slot];
    TerrainBodyHandle handle;
    int localY;

    handle.index = (uint16_t)slot;
    handle.generation = body->generation;

    for (localY = body->minimumY; localY <= body->maximumY; ++localY) {
        int localX;

        for (localX = body->minimumX; localX <= body->maximumX; ++localX) {
            CellMaterial material =
                DynamicTerrainCellAt(terrain, handle, localX, localY);
            Vector2 at;
            int worldX;
            int worldY;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            /* Cell centres, which is what the transform is defined against. */
            at = TerrainBodyLocalToWorld(body, (float)localX + 0.5f,
                                         (float)localY + 0.5f);
            worldX = (int)floorf(at.x);
            worldY = (int)floorf(at.y);
            if (worldX < 0 || worldY < 0 || worldX >= world->width ||
                worldY >= world->height) {
                ++system->stats.cellsRefused;
                continue;
            }
            if (WorldGetCell(world, worldX, worldY) != MATERIAL_EMPTY) {
                ++system->stats.cellsRefused;
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
