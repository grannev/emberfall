#ifndef TERRAIN_PHYSICS_H
#define TERRAIN_PHYSICS_H

/* One fixed step of terrain-body physics: integration, then collision against
 * the static cellular world.
 *
 * The dependency runs one way only. This module reads the world through a
 * `const World *` — the compiler, not a comment, is what guarantees collision
 * cannot delete a cell — and the world knows nothing about bodies. No
 * TerrainBody logic belongs anywhere inside the cellular material simulation.
 *
 * This is not a physics engine and must not grow into one. It is a specialised
 * solver for one shape of problem: a rotating raster against a static grid of
 * unit cells, with every cost bounded by a compile-time constant.
 */

#include <stdbool.h>

#include "dynamic_terrain.h"
#include "world.h"

/* ---- hard bounds --------------------------------------------------------
 *
 * Worst case per tick is the product of these, and every one of them is a
 * compile-time constant so the product can be read off:
 *
 *   MAX_TERRAIN_BODIES (32)
 *     x TERRAIN_MAX_SUBSTEPS (16)
 *       x [surface cells tested, <= MAX_TERRAIN_BODY_CELLS]
 *   plus MAX_TERRAIN_CONTACTS_PER_BODY (16) x TERRAIN_SOLVER_ITERATIONS (4)
 */

/* A substep never advances a body's fastest point by more than this, so it
   cannot step over a wall one cell thick. */
#define TERRAIN_COLLISION_SUBSTEP_DISTANCE 0.5f
/* Ceiling on substeps, and therefore on cost. Beyond the motion this covers a
   body may tunnel; the envelope is stated rather than hidden, and
   TerrainPhysicsConfigIsSafe checks that the shipped defaults stay inside it. */
#define TERRAIN_MAX_SUBSTEPS 16
/* Contacts kept per body per substep. A body resting on a long floor generates
   one per surface cell in touch; the deepest few are what the response needs,
   and keeping all of them would buy nothing. */
#define MAX_TERRAIN_CONTACTS_PER_BODY 16
/* The largest bounding radius any body can have. A raster is at most
   TERRAIN_BODY_RASTER_CAPACITY cells with neither side over
   TERRAIN_BODY_MAX_SPAN, so the widest it can be is 128x64 and the farthest a
   corner can sit from the centre is sqrt(64^2 + 32^2). Rounded up, this is what
   the speed ceilings are chosen against, which is why no body can tunnel rather
   than merely no body anyone has tried. */
#define TERRAIN_BODY_MAX_BOUNDING_RADIUS 72.0f

/* Velocity solver passes over the contact set. Enough for a body to settle on
   a floor without the unbounded "repeat until no overlap" loop that would make
   a bad frame arbitrarily expensive. */
#define TERRAIN_SOLVER_ITERATIONS 4

/* Advances every awake body by `deltaTime`, colliding against `world`.
 *
 * `world` may be NULL, which runs pure kinematics — that is what the
 * kinematics tests use, and what a caller with no world would want. It is never
 * written to.
 *
 * This is the only body update entry point; it is called from the fixed-step
 * loop in GameUpdate, never with a frame delta. */
void TerrainPhysicsUpdate(DynamicTerrainSystem *system, const World *world,
                          float deltaTime);

/* True when a config's speed ceilings stay inside what the substep budget can
   cover for a body of `boundingRadius`, i.e. when tunnelling through a
   one-cell wall is impossible rather than merely unlikely. Used by tests to
   hold the shipped defaults to their own promise. */
bool TerrainPhysicsConfigIsSafe(const DynamicTerrainConfig *config,
                                float boundingRadius, float deltaTime);

#endif
