#ifndef TERRAIN_IMPULSE_H
#define TERRAIN_IMPULSE_H

/* Ability blasts acting on terrain bodies.
 *
 * The physics is not here. `DynamicTerrainApplyImpulse` already turns an
 * impulse at a point into linear and angular velocity using the body's own mass
 * and inertia; this module decides only *which* bodies a blast reaches, how hard
 * it hits them, and where it lands. There is no second rigid-body model.
 *
 * Why a queue rather than a direct call from the ability: a blast has to be able
 * to throw the piece it just set free. An explosion destroys terrain, the
 * connectivity check runs, the fragment becomes a body — and only then is there
 * anything to push. Abilities run before the fixed step, so they describe the
 * blast and the fixed step applies it after detachment has had its turn. That
 * ordering is the whole reason this file exists, and it is also what makes a
 * blast apply exactly once however many fixed steps a frame happens to run.
 *
 * Large bodies are deliberately not special-cased. A cliff that came loose is a
 * physical object like any other; what makes it hard to move is its mass and
 * inertia, never a size check that refuses to push it.
 */

#include <stdbool.h>

#include "dynamic_terrain.h"
#include "world.h"

/* Blasts awaiting the next fixed step. Abilities have cooldowns, so more than a
   couple in one frame means several powers fired at once; the extra are counted
   and dropped rather than growing a buffer nothing needs. */
#define MAX_TERRAIN_BLASTS 8

typedef enum TerrainBlastShape {
    /* Outward from a point, the whole way round: an explosion. */
    TERRAIN_BLAST_RADIAL = 0,
    /* Outward from a point but only inside a cone, and stopped by terrain in
       the way: the force blast. Both restrictions exist to match what the same
       power already does to loose cells — a blow that reaches round a corner
       reads as the power passing straight through the world. */
    TERRAIN_BLAST_CONE
} TerrainBlastShape;

typedef struct TerrainBlast {
    TerrainBlastShape shape;
    Vector2 origin;
    /* Cone axis. Ignored by a radial blast. */
    Vector2 direction;
    /* Reach in cells. Strength falls linearly to nothing here. */
    float radius;
    /* Cosine of the cone's half angle. Ignored by a radial blast. */
    float spreadCosine;
    /* Impulse delivered at the origin, before falloff, in mass-cells per
       second. Divided by a body's mass, so the same blast moves a small
       fragment far and a hillside barely at all — which is the point. */
    float momentum;
} TerrainBlast;

typedef struct TerrainImpulseStats {
    int blastsQueued;
    int blastsDropped;
    int blastsApplied;
    int bodiesAffectedByExplosion;
    int bodiesAffectedByForce;
    int bodyImpulseApplications;
    /* Bodies a cone blast reached geometrically but could not touch because
       solid terrain stood in the way. */
    int bodiesOccluded;
    /* Bodies left asleep because the impulse could not have moved them beyond
       the speed the sleep rule already calls "at rest". */
    int bodiesLeftSleeping;
} TerrainImpulseStats;

typedef struct TerrainImpulseSystem {
    TerrainBlast blasts[MAX_TERRAIN_BLASTS];
    int blastCount;
    TerrainImpulseStats stats;
} TerrainImpulseSystem;

void TerrainImpulseInit(TerrainImpulseSystem *system);
void TerrainImpulseResetStats(TerrainImpulseSystem *system);

/* Records a blast for the next fixed step. Returns false when the description
   is unusable or the queue is full; either way nothing is applied and the
   caller has nothing to undo. */
bool TerrainImpulseQueueBlast(TerrainImpulseSystem *system, TerrainBlast blast);

/* Applies every queued blast and empties the queue. Returns the number of
   impulses delivered.

   Must run after detachment and before the bodies are integrated, so that a
   fragment freed by the same blast is thrown by it. `world` is read only, and
   only to decide whether terrain blocks a cone blast; passing NULL skips that
   test. Cost is O(queued blasts x live bodies) and nothing is allocated. */
int TerrainImpulseApply(TerrainImpulseSystem *system,
                        DynamicTerrainSystem *terrain, const World *world);

#endif
