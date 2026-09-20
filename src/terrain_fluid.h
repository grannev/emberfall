#ifndef TERRAIN_FLUID_H
#define TERRAIN_FLUID_H

/* What a liquid does to a terrain body and a body does to a liquid.
 *
 * The one place the two know about each other: `DynamicTerrainSystem` never
 * receives a World, so this module reads the world for it and hands the body
 * what it found as velocity, the way terrain_physics.c hands it contacts.
 *
 *   buoyancy   a body displaces the liquid its submerged cells stand in, and
 *              is pushed up by that liquid's weight. What decides whether it
 *              floats is its own density against the liquid's — an ice floe
 *              floats, a slab of rock sinks, a log of wood bobs — and nothing
 *              else: no rule about size, no coefficient that cancels mass.
 *   drag       a body in liquid sheds speed and spin in proportion to how
 *              much of it is under.
 *   splash     a body that breaks the surface fast shoves the liquid out of
 *              its way and tells presentation; with hysteresis on "in", so a
 *              floe bobbing at the surface does not splash every tick.
 *
 * Only awake bodies are looked at, and each is sampled at a bounded number of
 * its surface cells, so the cost is bodies times a constant. A body asleep on
 * a lake bed stays there; a body asleep at the surface stays afloat.
 */

#include <stdbool.h>

#include "dynamic_terrain.h"
#include "game_events.h"
#include "world.h"

typedef struct TerrainFluidConfig {
    /* Speed and spin shed per second when fully submerged, as exp(-k dt). */
    float linearDrag;
    float angularDrag;
    /* Lava is thicker. */
    float lavaDragScale;
    /* Speed at which breaking the surface is a splash. */
    float splashSpeed;
} TerrainFluidConfig;

typedef struct TerrainFluidStats {
    /* Refreshed by every update. */
    int bodiesInLiquid;
    /* Since init. */
    int entries;
    int exits;
    int cellsPushed;
} TerrainFluidStats;

typedef struct TerrainFluidSystem {
    TerrainFluidConfig config;
    TerrainFluidStats stats;
} TerrainFluidSystem;

/* Surface cells sampled per body per step. A floe of twelve thousand cells
   has thousands of surface cells; thirty-two of them, evenly along the list,
   say how much of it is under to within a few percent. */
#define TERRAIN_FLUID_SAMPLES 32

TerrainFluidConfig TerrainFluidDefaultConfig(void);
void TerrainFluidInit(TerrainFluidSystem *system);

/* Runs on the fixed step before TerrainPhysicsUpdate: applies buoyancy and
   drag to every awake body, pushes the liquid a body breaks into or out of,
   and reports the splashes. Writes the world only through WorldPushLiquid. */
void TerrainFluidUpdate(TerrainFluidSystem *system, DynamicTerrainSystem *terrain,
                        World *world, GameEventBuffer *events, float deltaTime);

#endif
