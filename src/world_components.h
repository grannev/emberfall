#ifndef WORLD_COMPONENTS_H
#define WORLD_COMPONENTS_H

/* Bounded detection of connected solid components.
 *
 * This is the foundation for detaching pieces of terrain, and it is
 * deliberately behaviour-neutral: it reads the world, never writes it, and
 * nothing in the game calls it yet.
 *
 * The hard requirement is that a query can never become a full-world flood
 * fill. The production map is 16384x864, and every solid cell in it is one
 * connected mass reaching the bottom of the world, so a fill started anywhere
 * in the ground would visit fourteen million cells. A caller therefore supplies
 * a query region, and the search is confined to it. What lies outside is not
 * explored — at most it is peeked at, one cell deep, to learn whether the
 * component continues.
 *
 * Semantics are conservative in one direction only: the detector may report
 * that it could not prove a component free, but it must never report a
 * component free when it is not. Every caller may act on WORLD_COMPONENT_DETACHED
 * and must treat everything else as "leave it alone".
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

/* The longest side of a query region, in cells. The workspace carries a visited
   bit for every cell of a region this size, which is what makes the cost of a
   query a compile-time bound rather than a function of the world. A region
   larger than this is rejected rather than silently clipped, because silently
   shrinking a caller's region is exactly how a "detached" answer stops meaning
   what the caller thought it meant. */
#define WORLD_COMPONENT_MAX_SPAN 128

/* The largest component the workspace can hold. A component that would exceed
   it stops the search and reports WORLD_COMPONENT_TOO_LARGE. 4096 cells is a
   64x64 block of rock, far larger than anything a single blast is expected to
   sever. */
#define WORLD_COMPONENT_MAX_CELLS 4096

typedef enum WorldComponentStatus {
    /* Proven free: the component was explored to completion inside the query
       region, and every cell adjacent to it is either non-solid or outside the
       region with a non-solid cell there. This is the only status a caller may
       act on. */
    WORLD_COMPONENT_DETACHED = 0,
    /* The component reaches the edge of the world. Outside the map reads as
       rock to the whole simulation, so this is a proven attachment. */
    WORLD_COMPONENT_ANCHORED,
    /* The component continues past the query region. It may or may not reach
       the ground; the detector refuses to guess and refuses to widen the
       search. */
    WORLD_COMPONENT_UNKNOWN,
    /* More cells than the workspace can hold. */
    WORLD_COMPONENT_TOO_LARGE,
    /* The query itself was malformed: no world or workspace, an empty or
       oversized region, a seed outside the region, or a seed that is not
       solid. */
    WORLD_COMPONENT_INVALID
} WorldComponentStatus;

/* Scratch storage for one query, owned by the caller. It is deliberately not a
   global and deliberately not heap-allocated: a query performs no allocation at
   all, so it is safe to run from a simulation tick.

   At roughly 34 KiB this belongs in a long-lived owner — the future dynamic
   terrain system will hold exactly one — rather than on the stack of whatever
   happens to ask a question. */
typedef struct WorldComponentWorkspace {
    /* One bit per cell of the query region, indexed region-locally. Only the
       prefix the current region actually uses is cleared, so the clear costs
       the region's area and not the maximum. */
    uint32_t visited[(WORLD_COMPONENT_MAX_SPAN * WORLD_COMPONENT_MAX_SPAN + 31) / 32];
    /* The component's cells, in world coordinates. This array doubles as the
       search frontier: a cell is appended when it is first seen and expanded
       later by index, so the queue needs no storage of its own. */
    int32_t cellX[WORLD_COMPONENT_MAX_CELLS];
    int32_t cellY[WORLD_COMPONENT_MAX_CELLS];
} WorldComponentWorkspace;

typedef struct WorldComponentResult {
    WorldComponentStatus status;
    /* Cells found, and their inclusive bounds in world coordinates. Meaningful
       only when `status` is WORLD_COMPONENT_DETACHED. Every other status
       returns them zeroed rather than describing however much was explored
       before the search gave up: a partial component is not a smaller
       component, and handing one back invites a caller to act on it. */
    int cellCount;
    int minimumX;
    int minimumY;
    int maximumX;
    int maximumY;
    /* How many cells the search had reached in the workspace when it stopped,
       on every status including the failures. This is deliberately *not* a
       component: it is a partial exploration, and acting on it as if it were a
       piece of terrain is exactly the mistake `cellCount` refuses to enable.

       It exists for one honest purpose. A caller that tries several seeds in
       the same damaged area will keep landing inside the same mass, and
       re-exploring it each time is the difference between a bounded check and a
       slow one. Such a caller can mark
       `workspace->cellX/cellY[0 .. exploredCells)` as already covered and skip
       any seed that falls inside. */
    int exploredCells;
} WorldComponentResult;

/* Explores the connected solid component containing (seedX, seedY), confined to
   `region` (in cells, the same convention as WorldActivateRegion).

   `maximumCells` caps the component size for this query; it is clamped to
   WORLD_COMPONENT_MAX_CELLS. Passing that constant asks for the whole
   workspace.

   The world is only read. On success the component's cells are in
   `workspace->cellX/cellY[0 .. result.cellCount)`. */
WorldComponentResult WorldFindComponent(const World *world,
                                        WorldComponentWorkspace *workspace,
                                        Rectangle region, int seedX, int seedY,
                                        int maximumCells);

#endif
