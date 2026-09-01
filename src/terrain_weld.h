#ifndef TERRAIN_WELD_H
#define TERRAIN_WELD_H

/* Giving a body back to the world it came out of.
 *
 * This is the exact inverse of terrain_extraction.h, and it exists for the same
 * reason that module does: a body is an expensive way to represent terrain that
 * has stopped moving. A slab that came loose, fell, and has been lying against
 * the ground for several seconds is scenery. It still holds a body slot, a
 * raster, a texture, a place in the collision loop and a share of the dynamic
 * cell budget, and it will hold all of them for the rest of the session,
 * because nothing here ever evicts a body for being old.
 *
 * Welding turns it back into cells. The slot is freed, the budget is returned,
 * and the rubble stays exactly where the player left it — as ground, which is
 * what it looks like anyway once it has settled.
 *
 * Three rules shape the implementation:
 *
 *   Only a body that has slept long enough. Sleep already means "below both
 *   sleep thresholds for a continuous spell"; this waits for a much longer one
 *   on top of that, so a slab that is still tumbling, or resting for a moment
 *   between bounces, is never welded mid-motion.
 *
 *   Never overwrite. A cell is written only where the world is empty. Two
 *   things could otherwise happen, and both are worse than a gap: material the
 *   player was standing in could be replaced, and a body resting a fraction of
 *   a cell inside the ground would delete the ground it rests on.
 *
 *   Never move the player. A body is refused, and left as a body, while the
 *   player is inside its footprint — welding around them would bury them in
 *   solid rock in a single frame.
 *
 * The blit runs backwards, from the world into the body. Walking the body's
 * cells and rounding each into the world is the obvious direction and it is
 * wrong: a rotated square of cells is not a square of cells, so two source
 * cells land on one destination while a third destination is named by none, and
 * the welded rubble comes out riddled with single-cell holes. Asking each world
 * cell which body cell covers it gives every destination exactly one answer.
 */

#include <stdbool.h>

#include "dynamic_terrain.h"
#include "world.h"

typedef struct TerrainWeldConfig {
    /* Seconds a body must have been asleep before it is given back. Long enough
       that a player watching rubble settle does not see it stiffen under them,
       short enough that a session spent demolishing things does not run out of
       body slots. */
    float weldDelay;
    /* Bodies welded per call. Welding is a write over a body's whole raster and
       dirties every chunk it touches, so a tick that would weld a dozen spreads
       them out instead. */
    int maxWeldsPerTick;
    /* Cells around the player kept clear of any weld. A body whose footprint
       reaches into this box waits; it does not lose its chance. */
    float playerClearance;
} TerrainWeldConfig;

typedef struct TerrainWeldStats {
    int bodiesWelded;
    int cellsWelded;
    /* Cells dropped because the world was not empty where they landed. A large
       count here means bodies are being welded while overlapping the ground,
       which is a physics question, not a welding one. */
    int cellsRefused;
    int bodiesDeferredByPlayer;
    int bodiesDeferredByBudget;
} TerrainWeldStats;

typedef struct TerrainWeldSystem {
    TerrainWeldConfig config;
    TerrainWeldStats stats;
    /* Seconds each slot has been asleep, indexed by slot rather than held on
       the body: how long a body has rested is a policy this module owns, and
       putting it here keeps dynamic_terrain.h free of a lifetime rule that has
       nothing to do with rigid bodies. Reset whenever the slot's generation
       changes, so a new body in a reused slot starts from zero. */
    float rested[MAX_TERRAIN_BODIES];
    uint16_t restedGeneration[MAX_TERRAIN_BODIES];
} TerrainWeldSystem;

TerrainWeldConfig TerrainWeldDefaultConfig(void);
void TerrainWeldInit(TerrainWeldSystem *system);
void TerrainWeldResetStats(TerrainWeldSystem *system);

/* Ages every sleeping body and welds the ones that have rested long enough.
   Returns the number welded.

   `playerAt` is where the player is, in cells; pass a point far outside the
   world to disable the clearance check. Must be called with the world settled,
   outside WorldUpdate, for the same reason detachment must: it reads and writes
   cells. */
int TerrainWeldProcess(TerrainWeldSystem *system, World *world,
                       DynamicTerrainSystem *terrain, Vector2 playerAt,
                       float deltaTime);

#endif
