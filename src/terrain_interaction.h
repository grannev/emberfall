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
    /* How far in front of the player the held body is pulled towards. */
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
    /* Where the held body's grab point currently is, for the same reason. */
    Vector2 holdWorldPoint;
} TerrainInteractionSystem;

TerrainInteractionConfig TerrainInteractionDefaultConfig(void);
void TerrainInteractionInit(TerrainInteractionSystem *system);
void TerrainInteractionResetStats(TerrainInteractionSystem *system);

/* One frame of player-body interaction: resolve overlaps, exchange momentum,
   and run the hold. Call after the player has settled against the static world,
   so the correction applied here is the last word on where they are.

   `grabHeld` is the interaction button's state; the transition into and out of
   it is what starts and ends a hold. Presentation reads the hold through the
   system's own state rather than through an event, because a highlight has to
   be drawn for as long as the hold lasts and not once when it starts. */
void TerrainInteractionUpdate(TerrainInteractionSystem *system, Player *player,
                              DynamicTerrainSystem *terrain, Vector2 aimWorld,
                              bool grabHeld, float deltaTime);

/* True when the player is holding a body that still exists. */
bool TerrainInteractionIsHolding(const TerrainInteractionSystem *system,
                                 const DynamicTerrainSystem *terrain);

#endif
