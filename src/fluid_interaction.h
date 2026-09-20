#ifndef FLUID_INTERACTION_H
#define FLUID_INTERACTION_H

/* What a liquid does to the player and the player does to a liquid.
 *
 * This is the one place the two know about each other. `Player` keeps no
 * notion of water beyond a drag coefficient it integrates; `World` keeps no
 * notion of a character; the coupling lives here, where it can be read in one
 * sitting, the way terrain_interaction.c couples the player to bodies.
 *
 * Four things happen, and all of them come from where the character is and
 * how fast they are going, nothing else:
 *
 *   under water   the character is slowed by a drag that grows with the
 *                 square of their speed — a dive is stopped hard, a crawl is
 *                 hardly touched — and never by a flat multiplier, which took
 *                 the same fraction from both and read as swimming through
 *                 glue. Thrust keeps working: a diving character can steer.
 *   going in      fast enough, the surface breaks: the water under the entry
 *                 is shoved outward and up, the speed across the surface is
 *                 cut, and presentation is told so it can splash.
 *   coming out    the same, the other way.
 *   flying low    a fast pass just above the surface lifts the water under
 *                 it — the wake of something going that fast that close.
 *
 * All of it is bounded by the size of the character: a handful of samples
 * through the collider, a handful of pushed cells, one event a frame at most.
 */

#include <stdbool.h>

#include "game_events.h"
#include "player.h"
#include "world.h"

typedef struct FluidInteractionConfig {
    /* Quadratic drag per cell of speed when fully submerged. With the boost's
       thrust of 720 cells/s^2 this settles a boosting character at about
       160 cells/s under water, well under the 380 of open air but far from
       stuck; in lava it is doubled by `lavaDragScale`. */
    float dragCoefficient;
    float lavaDragScale;
    /* Speed across the surface at which going in or coming out is a splash
       rather than a step. */
    float splashSpeed;
    /* Fraction of the speed lost on breaking the surface at a splash: the
       blow of hitting water flat. */
    float entryLoss;
    /* How far above a surface a pass counts as low, and how fast it must be
       to disturb the water. */
    float flyoverHeight;
    float flyoverSpeed;
    /* Seconds between two flyover disturbances, so a low pass leaves a line
       of them rather than lifting every cell it passes over. */
    float flyoverInterval;
} FluidInteractionConfig;

typedef struct FluidInteractionStats {
    int entries;
    int exits;
    int flyovers;
    int cellsPushed;
} FluidInteractionStats;

typedef struct FluidInteractionState {
    FluidInteractionConfig config;
    /* How much of the collider is in liquid this frame, 0 to 1, and which
       liquid, for presentation and for tests. */
    float submerged;
    CellMaterial liquid;
    /* Whether the character counted as in the liquid last frame, with
       hysteresis: in above a quarter, out below a twentieth, so a character
       bobbing at the surface does not enter and leave every frame. */
    bool inside;
    float flyoverCooldown;
    FluidInteractionStats stats;
} FluidInteractionState;

FluidInteractionConfig FluidInteractionDefaultConfig(void);
void FluidInteractionInit(FluidInteractionState *state);

/* Runs once per frame before PlayerUpdate: reads where the character is,
   sets the drag they will integrate, pushes the liquid they disturb and
   reports the splashes. Writes the world only through WorldPushLiquid. */
void FluidInteractionUpdatePlayer(FluidInteractionState *state, Player *player,
                                  World *world, GameEventBuffer *events,
                                  float deltaTime);

#endif
