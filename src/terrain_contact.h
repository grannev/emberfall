#ifndef TERRAIN_CONTACT_H
#define TERRAIN_CONTACT_H

/* Contacts between terrain bodies and whatever stops them, and the one solver
 * that resolves them together.
 *
 * A body touches two kinds of thing: the static world, which never moves, and
 * other bodies, which do. The two narrow phases live in different files —
 * terrain_physics.c asks the world, terrain_body_collision.c asks the other
 * rasters — but they must be *solved* as one problem. A pile is the reason. A
 * piece resting on a piece resting on the ground has its weight carried down
 * the pile and the ground's answer carried back up, and solving the ground
 * once and then the pair once leaves a residual that never shrinks: every tick
 * the pile creeps, nothing ever gets quiet enough to sleep, and the settled
 * scene the whole design is built around costs a full solve for ever. Iterating
 * the world contacts and the pair contacts in the same loop lets the answer
 * converge, and the residual after the last iteration is small enough for the
 * sleep thresholds to close over it.
 *
 * Everything here is bounded: a fixed contact set per body against the world,
 * a fixed number of pair manifolds with a fixed number of contacts each, and a
 * fixed number of iterations. Overflow keeps the deepest contacts and counts
 * what it dropped.
 */

#include <stdbool.h>

#include <raylib.h>

#include "terrain_limits.h"

/* This header is included by dynamic_terrain.h, which defines the system; the
   solver takes it through a forward declaration. */
typedef struct DynamicTerrainSystem DynamicTerrainSystem;

/* Contacts kept per body against the world, and per pair of bodies, in one
   substep. A slab resting on a long floor touches along its whole edge, and
   what the response needs from an edge is its two ends and its deepest point:
   two ends because any pressure along the edge has a resultant between them,
   the deepest because that is where a tilted body is actually being pushed.
   Contacts are grouped by the face they leave through — a raster has four —
   and each group keeps those three, so a body wedged in a corner keeps the
   floor's and the wall's contacts apart.

   The deepest few alone, which is what an earlier version kept, are the wrong
   ones: a slab a hair off level has its deepest contacts all at one end, a
   set made of them is a support at one end only, and the slab pivots on it
   and rocks for ever instead of sleeping. */
#define TERRAIN_CONTACT_FACES 4
#define TERRAIN_CONTACTS_PER_FACE 3
#define TERRAIN_CONTACT_CAPACITY (TERRAIN_CONTACT_FACES * TERRAIN_CONTACTS_PER_FACE)
#define MAX_TERRAIN_CONTACTS_PER_BODY TERRAIN_CONTACT_CAPACITY
#define TERRAIN_PAIR_MAX_CONTACTS TERRAIN_CONTACT_CAPACITY
/* Pairs in contact tracked in one substep. Every body in a pile touches a
   few others, so a pile of two hundred is a few hundred pairs; a scene where
   more than this many touch is a scene where quality, not safety, is being
   lost, and the overflow is counted. */
#define TERRAIN_PAIR_MAX_MANIFOLDS 768
/* Solver passes over every contact. Each pass carries a pile's weight one
   level further and the ground's answer one level back, so the residual after
   the last pass shrinks geometrically with the count. Four settled a body on
   a floor; a pile several bodies high needs the world and the pairs to meet in
   the middle, which is what the extra passes buy. Cost is contacts times
   passes and every factor is a constant. */
#define TERRAIN_SOLVER_ITERATIONS 8

typedef struct TerrainContact {
    Vector2 point;
    /* The direction the body has to move to separate. For a world contact,
       out of the solid cell; for a pair contact, out of the body that was
       entered, toward the one whose surface entered it. */
    Vector2 normal;
    float penetration;
    /* How fast the contact was closing when it was found, before the solver
       touched anything. Restitution has to be measured against this rather
       than against the current velocity: the solver drains the approach as it
       goes, so by the last contact of the last iteration there would be
       nothing left for a bounce to act on, and restitution would do almost
       nothing however high it was set. */
    float approachSpeed;
    /* Totals delivered through this contact over the solver's passes this
       substep. Kept so a pass can take back what an earlier one over-delivered
       while the total never becomes a pull, and read afterwards as how hard
       the contact hit. */
    float normalImpulse;
    float tangentImpulse;
    /* Which surface cell of the sampling body produced the contact: its
       identity from one substep to the next, so the totals above can be
       carried over as the next substep's starting guess. */
    int sample;
} TerrainContact;

/* What a contact leaves behind for the next substep. A resting pile is the
   same problem every substep, and the solution to the last one is very nearly
   the solution to this one: starting the passes from it, rather than from
   zero, is what makes a stack of bodies converge in the passes available
   instead of creeping under a residual the passes never quite remove. Matched
   by body, slot and sample; a contact that moved to a different cell starts
   from nothing. */
typedef struct TerrainWarmContact {
    int sample;
    float normalImpulse;
    float tangentImpulse;
} TerrainWarmContact;

typedef struct TerrainWarmManifold {
    int sampler;
    int raster;
    TerrainWarmContact contacts[TERRAIN_CONTACT_CAPACITY];
} TerrainWarmManifold;

/* One body's contacts against the static world this substep. Slots are
   grouped by face and may be empty; `count` is how many are not. */
typedef struct TerrainContactSet {
    TerrainContact contacts[TERRAIN_CONTACT_CAPACITY];
    int count;
    /* Deepest overlap seen this substep, including contacts that did not make
       it into the set. Positional correction uses it so that dropping shallow
       contacts can never make a body sink further. */
    float deepest;
} TerrainContactSet;

/* The contacts between one pair of bodies this substep. `sampler` is the body
   whose surface cells were tested; `raster` is the one they were tested
   against. Every normal points toward the sampler. */
typedef struct TerrainPairManifold {
    int sampler;
    int raster;
    TerrainContact contacts[TERRAIN_CONTACT_CAPACITY];
    int count;
    float deepest;
} TerrainPairManifold;

typedef struct TerrainContactStats {
    /* Refreshed by every physics update. */
    int pairsBroadPhase;
    int pairsNarrowPhase;
    int pairContacts;
    int manifoldsDropped;
    int bodiesWokenByContact;
    int bodiesWokenByNeighbour;
    /* Largest single normal impulse delivered between two bodies this update,
       in mass-cells per second: what a fracture rule reads. */
    float largestImpulse;
} TerrainContactStats;

typedef struct TerrainContactWorkspace {
    /* Indexed by body slot; only awake bodies have a non-empty set. */
    TerrainContactSet world[MAX_TERRAIN_BODIES];
    TerrainPairManifold manifolds[TERRAIN_PAIR_MAX_MANIFOLDS];
    int manifoldCount;
    /* The previous substep's impulses, and how long that substep was, so a
       warm start can be scaled to a substep of a different length. A slot
       whose count is zero has nothing remembered, and is neither read nor
       written, so the cost of a substep follows the bodies in contact rather
       than the size of the store. */
    TerrainWarmContact warmWorld[MAX_TERRAIN_BODIES][TERRAIN_CONTACT_CAPACITY];
    int warmWorldCount[MAX_TERRAIN_BODIES];
    TerrainWarmManifold warmManifolds[TERRAIN_PAIR_MAX_MANIFOLDS];
    int warmManifoldCount;
    float warmStepTime;
    float stepTime;
    /* World boxes of the live bodies, computed once per substep for the pair
       phase: the box needs the rotated corners, and two thousand pairs must
       not each recompute them. `boundsValid` is false for a dead or empty
       body. */
    Vector2 boundsMinimum[MAX_TERRAIN_BODIES];
    Vector2 boundsMaximum[MAX_TERRAIN_BODIES];
    bool boundsValid[MAX_TERRAIN_BODIES];
    TerrainContactStats stats;
} TerrainContactWorkspace;

void TerrainContactWorkspaceInit(TerrainContactWorkspace *workspace);
/* Zeroes the per-update counters. Called at the start of a physics update. */
void TerrainContactStatsReset(TerrainContactWorkspace *workspace);
/* Empties every set and manifold, keeping what the last substep's solve
   delivered as the warm start for this one. Called at the start of a substep
   of `stepTime` seconds, before the narrow phases fill it. */
void TerrainContactBegin(TerrainContactWorkspace *workspace, float stepTime);
/* Forgets the warm start, for every body or for one slot. Called when a body
   is freed or a slot reused, so an impulse remembered for one body is not
   delivered to whatever takes its slot. */
void TerrainContactForget(TerrainContactWorkspace *workspace);
void TerrainContactForgetBody(TerrainContactWorkspace *workspace, int slot);

/* Marks every slot of a contact array empty. */
void TerrainContactClear(TerrainContact *contacts);
/* True for a slot no contact was put in. */
static inline bool TerrainContactIsEmpty(const TerrainContact *contact)
{
    return contact->penetration < 0.0f;
}

/* Offers a contact to a fixed array. `face` is which of the four faces the
   sample leaves through, 0 to 3; within that face's three slots the contact
   is kept if it is the deepest so far, the farthest along the face one way,
   or the farthest the other way. Tracks the deepest overlap seen whether or
   not the contact was kept. Shared by both narrow phases so that the cap
   means the same thing in each. */
void TerrainContactAdd(TerrainContact *contacts, int *count, float *deepest,
                       int face, int sample, Vector2 point, Vector2 normal,
                       float penetration, float approachSpeed);

/* Resolves every world contact and pair manifold in the workspace against the
   bodies' velocities, then corrects positions. A sleeping body is a wall:
   infinite mass, no correction. Records each body's largest impact. */
void TerrainContactSolve(TerrainContactWorkspace *workspace,
                         DynamicTerrainSystem *system);

#endif
