#ifndef WORLD_FLUID_H
#define WORLD_FLUID_H

/* Private to the world module: what a liquid cell remembers, the pressure it
 * feels and the momentum it can be given. Public entry points are in world.h.
 *
 * A liquid is still one cell per unit of liquid. Mass is conserved because a
 * cell moves and is never created or destroyed by motion — WorldMoveCell is a
 * swap — and every rule below is a rule about where a whole cell goes. What
 * this module adds is two things a falling-sand liquid does not have on its
 * own:
 *
 *   pressure  every enclosed liquid cell carries a *head*: how far below the
 *             highest free surface it is connected to, propagated one cell a
 *             tick from its neighbours. A cell at the top of a column with head
 *             below it is being pushed up by liquid that stands higher
 *             somewhere else, and it rises. That is what makes water find the
 *             same level in both arms of a U, fill a cave to the level of the
 *             lake feeding it, and stop there.
 *
 *   momentum  a bounded queue of impulses, each of which pushes one cell one
 *             step a tick along a direction for a number of ticks, and passes
 *             itself on to the liquid it meets. A blast, a body or a diving
 *             player moves water for a while rather than teleporting it once.
 *
 * Both live in what the cell already has: the head in `Cell.lifetime`, which
 * a liquid never used as an age, and the impulses in a fixed array in World.
 * The cell did not grow, and a settled pool still costs nothing — a head that
 * does not change wakes nothing.
 */

#include <stdbool.h>
#include <stdint.h>

#include "world_internal.h"

/* ---- what a liquid keeps in `lifetime` --------------------------------
 *
 * Low bits: how many times a surface cell has slid along the top of a pool
 * without finding anywhere to fall. High bits: the head, in sixteenths of a
 * cell. A surface cell has no head and an enclosed cell does not wander, so
 * the two never need each other's bits at the same time, but keeping them
 * apart means a cell that goes from one state to the other carries no garbage
 * into the other's field. Five bits of wander is thirty-one slides of up to
 * sixteen cells, enough to cross the widest pool the generator makes; the
 * eleven bits left hold a hundred and twenty-seven cells of head, and a
 * deeper lake simply presses no harder than that. */
#define WORLD_LIQUID_WANDER_BITS 5
#define WORLD_LIQUID_WANDER_MASK ((1u << WORLD_LIQUID_WANDER_BITS) - 1u)
#define WORLD_LIQUID_HEAD_SHIFT WORLD_LIQUID_WANDER_BITS
#define WORLD_LIQUID_HEAD_MAX ((1u << (16 - WORLD_LIQUID_HEAD_SHIFT)) - 1u)
/* Of the wander bits, the lowest is the direction of the last slide and the
   rest count the slides. A grain that kept no direction chose one afresh
   every tick and walked a pool at random, sixteen cells a step, so crossing
   two hundred cells took more steps than any budget allowed; keeping the
   direction makes the walk a line, and a line crosses the widest pool the
   generator makes in a handful of steps. */
#define WORLD_LIQUID_WANDER_COUNT_SHIFT 1
#define WORLD_LIQUID_WANDER_COUNT_MAX ((1u << (WORLD_LIQUID_WANDER_BITS - 1)) - 1u)
/* How far a surface grain slides along a pool in one step, and how far a
   pressed cell runs. A grain reads every cell it slides past, so this is the
   cost of one wandering grain; there are few of them and each stops within
   its budget, where a pressed run is what a whole poured column does at
   once. */
#define WORLD_LIQUID_WANDER_REACH 32
/* Head units per cell of depth. */
#define WORLD_LIQUID_HEAD_PER_CELL 16u
/* Head lost per cell a value travels sideways, or upward through liquid that
   has no free surface over it. The head of a cell under a free surface is
   exact — its column is walked down from the surface every tick — but a cell
   in a channel or a sealed shaft only knows what its neighbours tell it, and
   without a loss two such cells could hold each other's stale head up for
   ever after the surface that gave it to them had drained. With it, a stale
   head decays at this rate per tick and a genuine one arrives a little
   smaller. An eighth of a cell per cell: a channel forty cells long costs five
   cells of level, which is what a long narrow pipe costs in the real thing
   too, and a stale head of a cell is gone in eight ticks. */
#define WORLD_LIQUID_HEAD_LOSS 1u
/* How much more head the bottom of a column must carry than its own depth
   accounts for before its surface is lifted: a connected surface standing
   this much higher. Two cells and a half. A step of one cell between
   neighbouring columns is what the sideways run levels, by sliding the higher
   grain across — a lift there raised the lower column by taking from the
   higher one's foot, which turned the step round rather than removing it, and
   the two columns traded the step back and forth for ever. The half is
   hysteresis, so two arms within half a cell of each other stop. */
#define WORLD_LIQUID_LIFT_THRESHOLD (2u * WORLD_LIQUID_HEAD_PER_CELL + 8u)
/* How far down a surface cell looks for a push into its column each tick,
   two material reads a cell. Bounds the cost of one surface cell; a push
   that enters a column deeper than this is not seen. Not rate-limited: a
   column checked every fourth tick could fall asleep between checks with a
   push waiting beside it, and did. */
#define WORLD_LIQUID_COLUMN_REACH 48
/* How far up a column the head of a cell under liquid is read. Bounds the
   cost of one read; a column deeper than this presses as if it were this
   deep, which for a lake deeper than sixty-four cells is a pressure nothing
   in the game can tell from the true one. */
#define WORLD_LIQUID_CHAIN_REACH 64
/* How far a lift follows the head uphill from the bottom of its column to
   find the liquid that is pushing. Bounds the cost of one lift; a longer
   route is taken from part way along, and the hole that leaves walks the
   rest of the way by the ordinary flow. */
#define WORLD_LIQUID_SOURCE_REACH 128

static inline uint16_t WorldLiquidHead(const Cell *cell)
{
    return (uint16_t)(cell->lifetime >> WORLD_LIQUID_HEAD_SHIFT);
}

static inline uint16_t WorldLiquidWander(const Cell *cell)
{
    return (uint16_t)((cell->lifetime & WORLD_LIQUID_WANDER_MASK) >>
                      WORLD_LIQUID_WANDER_COUNT_SHIFT);
}

static inline int WorldLiquidWanderDirection(const Cell *cell)
{
    return (cell->lifetime & 1u) != 0u ? 1 : -1;
}

static inline void WorldLiquidSetHead(Cell *cell, uint32_t head)
{
    if (head > WORLD_LIQUID_HEAD_MAX) {
        head = WORLD_LIQUID_HEAD_MAX;
    }
    cell->lifetime = (uint16_t)((cell->lifetime & WORLD_LIQUID_WANDER_MASK) |
                                (head << WORLD_LIQUID_HEAD_SHIFT));
}

static inline void WorldLiquidSetWander(Cell *cell, uint32_t wander,
                                        int direction)
{
    if (wander > WORLD_LIQUID_WANDER_COUNT_MAX) {
        wander = WORLD_LIQUID_WANDER_COUNT_MAX;
    }
    cell->lifetime = (uint16_t)((cell->lifetime & ~WORLD_LIQUID_WANDER_MASK) |
                                (wander << WORLD_LIQUID_WANDER_COUNT_SHIFT) |
                                (direction > 0 ? 1u : 0u));
}

/* A liquid cell with nothing above it that it could be pressed by: the free
   surface of a pool, or a single grain in the air. Gas counts as nothing —
   steam over a pond does not hold the pond down. */
static inline bool WorldLiquidIsSurface(const World *world, int x, int y)
{
    CellMaterial above = WorldMaterialAt(world, x, y - 1);

    return !MaterialIsLiquid(above) && !MaterialIsSolid(above);
}

/* For a cell with liquid or rock over it: recomputes its head from its
   neighbours and stores it, waking the neighbourhood only when it changed.
   Returns the new head. */
uint32_t WorldFluidUpdateHead(World *world, int x, int y);

/* For a surface cell: walks the column of the same liquid under it, giving
   every cell the head its depth accounts for or whatever more its neighbours
   offer, and lifts the bottom cell into the empty cell above the surface when
   that bottom is pushed by more head than the column's own height explains.
   The cell that rises is the bottom of the column, which leaves the hole
   where the pressure comes from rather than under the surface it just raised.
   Returns whether a cell was lifted. */
bool WorldFluidSurfaceStep(World *world, int x, int y);

/* Whether the pressed cell at (x, y) may run `distance` cells along
   `direction` into empty cells: it may unless the liquid just beyond them
   stands under at least as much head, in which case that liquid is pushing
   back as hard. Of two liquid cells either side of a hole, the one with the
   greater head gets it. */
bool WorldFluidMayFlowToward(const World *world, int x, int y, int direction,
                             int distance);

/* Advances every queued impulse by one step. Called once per tick before the
   cell traversal, so a pushed cell has moved when its row is visited and does
   not also flow. */
void WorldFluidStepImpulses(World *world);

#endif
