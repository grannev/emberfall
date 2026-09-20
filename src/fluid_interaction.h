#ifndef FLUID_INTERACTION_H
#define FLUID_INTERACTION_H

/* What the player does to a liquid.
 *
 * Not what a liquid does to the player: nothing slows the character, in
 * water or in lava, and nothing here may start to. Flying through the world
 * rather than only around it is the point of the game, and it is not
 * punished by any material — the drag that once took a share of every frame
 * under water read as swimming through glue and was removed for good. What
 * the character does to the water is the whole of this module, and it is
 * big, because a body going that fast that close to a surface leaves its
 * mark on it:
 *
 *   going in      fast enough, the surface breaks in a crown thrown clear of
 *                 the water and a ring spreading from it, and presentation is
 *                 told so it can splash.
 *   coming out    the same, the other way.
 *   under water   the character shoves the water aside as they go: a wake
 *                 of cells thrown out of the way every frame at speed. At
 *                 drill speed they burn through it instead, and the water in
 *                 the corridor flashes to steam (WorldDrillCircle).
 *   flying low    a fast pass just above the surface lifts a wall of water
 *                 under and behind it, wider and higher the faster it goes,
 *                 and a supersonic pass lifts it from further up — the wake
 *                 of a jet over a river.
 *
 * The coupling lives here and only here: `Player` keeps no notion of water
 * and `World` no notion of a character. Everything is bounded by the size
 * of the character and the speed ceilings.
 */

#include <stdbool.h>

#include "game_events.h"
#include "player.h"
#include "world.h"

typedef struct FluidInteractionConfig {
    /* Speed across the surface at which going in or coming out is a splash
       rather than a step. */
    float splashSpeed;
    /* Speed under water at which the character throws a wake. */
    float wakeSpeed;
    /* How far above a surface a pass counts as low, and how fast it must be
       to disturb the water; the supersonic pass reaches further. */
    float flyoverHeight;
    float flyoverSpeed;
    float sonicFlyoverHeight;
    /* Seconds between two flyover disturbances, so a low pass leaves a
       continuous wall rather than lifting the same cells every frame. */
    float flyoverInterval;
} FluidInteractionConfig;

typedef struct FluidInteractionStats {
    int entries;
    int exits;
    int flyovers;
    int wakes;
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
    float wakeCooldown;
    FluidInteractionStats stats;
} FluidInteractionState;

FluidInteractionConfig FluidInteractionDefaultConfig(void);
void FluidInteractionInit(FluidInteractionState *state);

/* Runs once per frame before PlayerUpdate: reads where the character is,
   pushes the liquid they disturb and reports the splashes. Never writes the
   player, and writes the world only through the liquid push entry points. */
void FluidInteractionUpdatePlayer(FluidInteractionState *state,
                                  const Player *player, World *world,
                                  GameEventBuffer *events, float deltaTime);

#endif
