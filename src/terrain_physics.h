#ifndef TERRAIN_PHYSICS_H
#define TERRAIN_PHYSICS_H

/* One fixed step of terrain-body physics: integration, collision against the
 * static cellular world, then collision between the bodies themselves
 * (terrain_body_collision.h), all inside one shared run of substeps.
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
 *   TERRAIN_MAX_SUBSTEPS (24), shared by every awake body, times
 *     MAX_TERRAIN_BODIES (64)
 *       x [surface cells tested against the world, <= MAX_TERRAIN_BODY_CELLS]
 *     plus the pair phase: MAX_TERRAIN_BODIES choose two box tests and
 *       TERRAIN_PAIR_MAX_MANIFOLDS x TERRAIN_PAIR_MAX_SAMPLES raster reads
 *     plus the solve: TERRAIN_SOLVER_ITERATIONS x
 *       (MAX_TERRAIN_BODIES x MAX_TERRAIN_CONTACTS_PER_BODY
 *        + TERRAIN_PAIR_MAX_MANIFOLDS x TERRAIN_PAIR_MAX_CONTACTS) impulses.
 *
 * The substep count is the one the fastest awake body needs, taken by all of
 * them, because two bodies can only be compared at the same moment. The
 * contact caps and the iteration count live in terrain_contact.h.
 */

/* A substep never advances a body's fastest point by more than this, so it
   cannot step over a wall one cell thick. Two bodies closing on each other
   advance by at most twice this relative to each other, which is one cell:
   still not enough to step a surface cell centre clean across a body one cell
   thick, so the same budget covers bodies against bodies. */
#define TERRAIN_COLLISION_SUBSTEP_DISTANCE 0.5f
/* Ceiling on substeps, and therefore on cost. Beyond the motion this covers a
   body may tunnel; the envelope is stated rather than hidden, and
   TerrainPhysicsConfigIsSafe checks that the shipped defaults stay inside it.

   Raised from sixteen with the body size: a bigger body has a longer bounding
   radius, its edge travels further for the same spin, and the budget has to
   cover that. Raising it rather than slowing every body's spin is the cheaper
   trade — substeps are taken from the motion a body actually has, so an
   ordinary one still takes one or two, and only the largest, fastest, fastest
   spinning body ever reaches the ceiling. */
#define TERRAIN_MAX_SUBSTEPS 32
/* The largest bounding radius any body can have. A raster is at most
   TERRAIN_BODY_RASTER_CAPACITY cells with neither side over
   TERRAIN_BODY_MAX_SPAN, so the widest it can be is 384x384 and the farthest a
   corner can sit from the centre is sqrt(192^2 + 192^2). Rounded up, this is
   what the speed ceilings are chosen against, which is why no body can tunnel
   rather than merely no body anyone has tried. */
#define TERRAIN_BODY_MAX_BOUNDING_RADIUS 272.0f

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
