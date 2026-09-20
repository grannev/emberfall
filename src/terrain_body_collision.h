#ifndef TERRAIN_BODY_COLLISION_H
#define TERRAIN_BODY_COLLISION_H

/* Terrain bodies against each other.
 *
 * Until this existed every body collided with the static world alone, and a
 * pile of rubble was a pile of pieces occupying the same cells. This is the
 * missing half of the narrow phase, and it is built the way the world half is:
 * a rotating raster tested against a raster, one cell read per sample, no
 * polygon clipping and no general engine.
 *
 * Shape of the work, all of it bounded by compile-time constants:
 *
 *   broad phase   every pair of live bodies in slot order, rejected on the
 *                 world-space bounding boxes — at most MAX_TERRAIN_BODIES
 *                 choose two comparisons, which is nothing;
 *   narrow phase  the surface cells of the body with the fewer of them,
 *                 transformed into the other body's frame and looked up in
 *                 its raster, at most TERRAIN_PAIR_MAX_SAMPLES per pair.
 *
 * What it finds goes into the shared contact workspace (terrain_contact.h),
 * where it is solved in the same iterations as the contacts with the world.
 *
 * A sleeping body is a wall to whatever touches it — infinite mass, no
 * correction — unless something moving faster than TERRAIN_PAIR_WAKE_SPEED
 * touches it, and then it is a body again. That is what lets a pile settle: its
 * base sleeps first, and the pieces on top come to rest against something that
 * no longer moves. Waking a body also wakes what rests on it, so a base kicked
 * out from under a pile does not leave the pile hanging in the air.
 *
 * Pair order is slot order, always. The response is sequential and therefore
 * order-dependent, and a deterministic order is what keeps a replay a replay.
 *
 * Called from TerrainPhysicsUpdate once per substep; nothing else needs it.
 */

#include <stdbool.h>

#include "terrain_contact.h"

/* Surface cells one narrow phase may test. A filigree body can have thousands
   of surface cells, and testing every one of them for every pair it overlaps
   every substep is where a bad frame would go; past this many the surface is
   sampled with a stride instead. A missed corner is a deeper contact on the
   next substep, never a lost one. */
#define TERRAIN_PAIR_MAX_SAMPLES 2048
/* Edge speed of a moving body that wakes a sleeping body it touches. Below it
   the sleeper is a wall: a pile's base must not be woken by the weight
   creeping on top of it, or a pile could never sleep. Above it the sleeper is
   struck, or left behind, and moves. */
#define TERRAIN_PAIR_WAKE_SPEED 4.0f

/* Fills the workspace's manifolds with every contact between live bodies,
   waking the sleepers that a moving body touches. Runs after the world narrow
   phase and before TerrainContactSolve. */
void TerrainPairCollect(TerrainContactWorkspace *workspace,
                        DynamicTerrainSystem *system);

/* Wakes every sleeping body whose bounding box touches a body that woke since
   the last call, so the pieces resting on a piece that starts moving move with
   it rather than hanging where it was. Bounded by bodies squared. */
void TerrainPairWakeNeighbours(TerrainContactWorkspace *workspace,
                               DynamicTerrainSystem *system);

#endif
