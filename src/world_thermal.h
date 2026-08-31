#ifndef WORLD_THERMAL_H
#define WORLD_THERMAL_H

/* Heat transport, phase changes and the water/lava reaction. Kept apart from
   the motion rules in world_simulation.c because temperature is what decides
   *what* a cell is, while the simulation decides where it goes; the laser and
   the cryo beam need the first without the second. */

#include <stdbool.h>

#include "world.h"

/* cap <= 0 means the source imposes no ceiling of its own. */
void WorldHeatNeighbors(World *world, int x, int y, float heat, float cap);
/* Applies whichever phase transition the cell's current temperature calls for.
   Returns true when the material changed. */
bool WorldTryThermalTransition(World *world, int x, int y);
bool WorldUpdateTemperatureState(World *world, int x, int y);
void WorldBurnDirt(World *world, int x, int y);
bool WorldTryMaterialReaction(World *world, int x, int y);

#endif
