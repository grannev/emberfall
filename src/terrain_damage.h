#ifndef TERRAIN_DAMAGE_H
#define TERRAIN_DAMAGE_H

/* Cutting into a terrain body, and letting it fall apart when the cut goes all
 * the way through.
 *
 * A detached slab stops being an indestructible sprite here. The same powers
 * that carve the static world carve a body's raster, in the body's own frame,
 * and when a cut severs it the pieces become bodies of their own.
 *
 * Two things this is deliberately not:
 *
 *   Not a cellular simulation inside a moving body. A body's cells do not fall,
 *   flow, burn or settle. They are a rigid shape that can lose material; the
 *   moment they would need to move relative to each other, that is fracture,
 *   and fracture makes new bodies rather than a second sand simulation with
 *   its own frame of reference.
 *
 *   Not a stress model. Connectivity is the whole of it: a piece either still
 *   touches the rest of the body or it does not. Nothing bends, sags, or
 *   accumulates damage.
 *
 * Everything here works on `DynamicTerrainSystem` alone and never sees a
 * `World`, so the static world cannot be changed through this path.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dynamic_terrain.h"

typedef struct TerrainDamageConfig {
    /* Pieces smaller than this are dropped rather than turned into bodies. A
       cut across a slab leaves a scatter of one- and two-cell chips along its
       edge, and each of those would otherwise take a body slot, a raster and a
       texture for something the player reads as dust. */
    int minimumFractureCells;
    /* Seconds between successive laser bites into a body. The beam is held, so
       without a rate it would carve on every frame and evaporate a slab in
       well under a second. */
    float beamCutInterval;
    /* Radius of one laser bite, in cells. */
    float beamCutRadius;
} TerrainDamageConfig;

typedef struct TerrainDamageStats {
    int carveCalls;
    int cellsCarved;
    int bodiesEmptied;
    int fractureChecks;
    int fractureSplits;
    int fragmentsCreated;
    /* Pieces that stayed part of the original body because they were too small
       to be worth one, and because a budget had no room for another. */
    int fragmentsTooSmall;
    int fragmentsRefusedByBudget;
} TerrainDamageStats;

typedef struct TerrainDamageSystem {
    TerrainDamageConfig config;
    TerrainDamageStats stats;
    /* Time until the beam may bite again. Rate state, so it lives beside the
       rate that defines it. */
    float beamCooldown;
    /* Scratch for one connectivity pass over one body's raster. Owned here
       rather than on a stack because it is 24 KiB, and rather than as a global
       because this module has one owner like every other subsystem. */
    uint8_t component[TERRAIN_BODY_RASTER_CAPACITY];
    uint16_t queue[TERRAIN_BODY_RASTER_CAPACITY];
} TerrainDamageSystem;

/* The largest number of separate pieces one fracture will track. Beyond it the
   remaining cells stay with the body they were part of, which is always a safe
   answer: nothing is lost and nothing moves. */
#define TERRAIN_FRACTURE_MAX_COMPONENTS 32

TerrainDamageConfig TerrainDamageDefaultConfig(void);
void TerrainDamageInit(TerrainDamageSystem *system);
void TerrainDamageResetStats(TerrainDamageSystem *system);

/* Clears every occupied cell of `handle` inside a world-space circle. Returns
   the number of cells removed.

   The body's mass, centre of mass, inertia, bounds and surface list are
   recomputed, and its transform is corrected so that every cell that survived
   stays exactly where it was in the world — a carve moves the centre of mass,
   and without the correction the body would visibly jump. A body left with no
   cells at all is freed. */
int TerrainDamageCarveCircle(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius);

/* Splits `handle` if its raster has stopped being one connected piece. Returns
   the number of new bodies created; zero means it was still in one piece, or
   that nothing could be split off safely.

   The largest piece keeps the original slot, so a handle held across a fracture
   still names the body a caller would recognise as "the one it was holding".
   Every piece keeps its materials and temperatures, lands exactly where it
   already was, and inherits the parent's motion including the part of it that
   comes from spin about a centre of mass the piece no longer shares. */
int TerrainDamageFracture(TerrainDamageSystem *system,
                          DynamicTerrainSystem *terrain,
                          TerrainBodyHandle handle);

/* Carve, then split if the carve severed anything. The pairing callers almost
   always want; returns cells removed. */
int TerrainDamageApplyCircle(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius);

/* Warms the cells a cut left behind, as a fraction of each material's own phase
   threshold. A body carries its cells' temperatures, so a tunnel bored through
   one should glow at its edges exactly as a tunnel through the static world
   does; without this a slab is the one thing in the game that can be drilled
   without getting hot. */
void TerrainDamageHeatAround(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius, float strength);

/* Advances the beam's cut rate and reports whether it may bite now. */
bool TerrainDamageBeamReady(TerrainDamageSystem *system, float deltaTime);

#endif
