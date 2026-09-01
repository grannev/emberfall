#ifndef TERRAIN_INTERACTION_H
#define TERRAIN_INTERACTION_H

/* What the player can do to a piece of terrain that has come loose: stand on
 * it, walk into it, shove it, take hold of it, drag it around and throw it.
 *
 * This is the one place the player and terrain bodies know about each other.
 * Neither module gains a field for the other: `Player` still describes a
 * character, `DynamicTerrainSystem` still describes rigid bodies, and the
 * coupling lives here where it can be read in one sitting.
 *
 * Two rules run through all of it:
 *
 *   Mass is the only thing that makes something hard to move. A small chip
 *   skitters away, a hillside barely notices, and the difference comes from
 *   dividing by mass rather than from any check on how big a body is allowed
 *   to be.
 *
 *   The player is what gets corrected out of an overlap, never the body.
 *   Pushing a slab out of the way to make room would teleport terrain the
 *   player is standing on; moving the player is always safe and is what the
 *   character controller already expects.
 */

#include <stdbool.h>

#include "dynamic_terrain.h"
#include "player.h"
#include "terrain_damage.h"

typedef struct TerrainInteractionConfig {
    /* The player has no mass of their own — nothing else in the game needed
       one. It lives here because it means exactly one thing: how much of a
       shove a body gets back when the player runs into it. */
    float playerMass;
    /* Almost nothing. Rock is not springy, and a player who bounces off a slab
       reads as standing on a trampoline. */
    float contactRestitution;
    /* Ceiling on one contact's exchange, so a single frame of a boosted player
       at full speed cannot launch a body across the map. */
    float maxContactImpulse;

    /* How far from the player a body may be taken hold of, measured to the
       nearest point of its world box. */
    float grabDistance;
    /* How far the aim may land from a body and still count as pointing at it.
       Separate from the reach, and deliberately generous: a slab is a big
       target, and demanding the cursor land on it exactly turns picking things
       up into a precision task nobody asked for. Still bounded, so a body the
       player is clearly not looking at is not taken. */
    float grabAimTolerance;
    /* How far from the player the cursor may drag a held body. The hold pulls
       the body to where the cursor is — telekinesis, not a carried plank — so
       this is a leash rather than a fixed distance: point further away than
       this and the body goes as far as the leash allows, along the same line. */
    float holdDistance;
    /* Spring pulling the grab point towards that target, and the damping that
       stops it oscillating. Both are forces in absolute units, so a heavier
       body follows more sluggishly — which is the point. */
    float pullStrength;
    float pullDamping;
    float maxPullForce;
    /* Impulse added along the aim when the hold is let go. Not scaled by mass,
       so a chip can be thrown and a boulder can only be dropped. */
    float throwImpulse;
} TerrainInteractionConfig;

typedef struct TerrainInteractionStats {
    int contacts;
    int pushImpulses;
    /* Frames whose movement was long enough to need breaking up, and the most
       pieces any one of them needed. */
    int sweptFrames;
    int maximumSubsteps;
    /* Holds that followed their cell onto the fragment it broke away with,
       rather than ending because the cell left the body. */
    int transferredHolds;
    /* Cells the player's drill took out of a body, and the bodies it cut
       clean through. */
    int bodyCellsDrilled;
    int bodiesDrilled;
    int grabs;
    int releases;
    int throws;
    /* Grabs that ended because the body stopped existing under the hold —
       carved away, or fractured into something else. */
    int lostHolds;
} TerrainInteractionStats;

typedef struct TerrainInteractionSystem {
    TerrainInteractionConfig config;
    TerrainInteractionStats stats;

    /* The body being held, and where on it. The point is stored in the body's
       own raster coordinates, so it stays on the same corner of the same rock
       however the body turns. */
    TerrainBodyHandle held;
    Vector2 holdLocalPoint;
    bool holding;
    /* Set to the body the player could take hold of right now, whether or not
       they are holding anything. Read-only for presentation; the simulation
       never draws. */
    TerrainBodyHandle hovered;
    /* Where the held body's grab point currently is, for the same reason — and
       the value a hold transferred across a fracture is matched against. */
    Vector2 holdWorldPoint;
    /* Where the player was when this ran last. The difference is the path they
       took, and the path is what has to be tested: checking only where they
       ended up lets a boosting player cross a thin slab between two frames and
       never touch it. */
    Vector2 previousPosition;
    bool hasPreviousPosition;
} TerrainInteractionSystem;

/* Most substeps one frame of player movement is broken into. The step is a
   fraction of the player's own radius, so consecutive probes overlap and
   nothing can slip between them; the cap is what keeps a teleport — a respawn,
   a regenerated world — from turning into an unbounded walk. */
#define TERRAIN_INTERACTION_MAX_SUBSTEPS 32

TerrainInteractionConfig TerrainInteractionDefaultConfig(void);
void TerrainInteractionInit(TerrainInteractionSystem *system);
void TerrainInteractionResetStats(TerrainInteractionSystem *system);

/* One frame of player-body interaction: resolve overlaps, exchange momentum,
   and run the hold. Call after the player has settled against the static world,
   so the correction applied here is the last word on where they are.

   A boosting player cuts through a body rather than stopping on it: `damage`
   may be NULL, and then a body is simply solid. The drill is the same drill
   that cuts the static world, so a detached slab is not the one thing in the
   game that can stop it.

   `grabHeld` is the interaction button's state; the transition into and out of
   it is what starts and ends a hold. Presentation reads the hold through the
   system's own state rather than through an event, because a highlight has to
   be drawn for as long as the hold lasts and not once when it starts. */
void TerrainInteractionUpdate(TerrainInteractionSystem *system, Player *player,
                              DynamicTerrainSystem *terrain,
                              TerrainDamageSystem *damage, Vector2 aimWorld,
                              bool grabHeld, float deltaTime);

/* True when the player is holding a body that still exists. */
bool TerrainInteractionIsHolding(const TerrainInteractionSystem *system,
                                 const DynamicTerrainSystem *terrain);

#endif
