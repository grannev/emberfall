#ifndef TERRAIN_LIMITS_H
#define TERRAIN_LIMITS_H

/* ---- hard budgets of dynamic terrain ------------------------------------
 *
 * Dynamic terrain must never be able to grow without bound: a player with a
 * force blast and some patience would otherwise turn a hillside into an
 * unbounded allocation. Every limit here is compile-time, and every one of them
 * is enforced by refusing work rather than by growing.
 *
 * They live in their own header because the contact workspace is sized by
 * them and is itself a member of the body store: dynamic_terrain.h needs the
 * workspace, the workspace needs these, and a header that needed
 * dynamic_terrain.h would be a cycle.
 */

/* Bodies alive at once. A single blast severs a handful of pieces; sixty-four
   leaves room for a chaotic scene without pretending the budget is infinite.
   It could be raised because settled rubble no longer holds a slot for the rest
   of the session — see terrain_weld.h — so the number bounds how much is loose
   at one time rather than how much has ever come loose. */
#define MAX_TERRAIN_BODIES 64

/* Raster slots reserved for each body. A body's bounding box must fit in this
   many cells — not in a square, so a long thin slab is as welcome as a
   compact lump. 27648 holds a 144x192 shard, which is what lets a body be a
   piece of cliff rather than a boulder. */
#define TERRAIN_BODY_RASTER_CAPACITY 27648

/* Longest side of a body's bounding box, matching the detector's own region
   limit so that anything WorldFindComponent can report as detached is a shape
   this can hold. */
#define TERRAIN_BODY_MAX_SPAN 192

/* Occupied cells in one body, inherited from WORLD_COMPONENT_MAX_CELLS: a body
   can only ever be built from a component the detector proved free. It is also
   the cap on a body's surface list, because a filigree body can have every one
   of its cells on the surface. */
#define MAX_TERRAIN_BODY_CELLS 12288

/* Total material/temperature raster storage:
   64 x 27648 x (1 byte material + 4 bytes temperature) = 8.4 MiB, allocated
   once and never resized. Collision also owns a fixed surface list. */
#define MAX_TERRAIN_RASTER_CELLS (MAX_TERRAIN_BODIES * TERRAIN_BODY_RASTER_CAPACITY)

/* Occupied cells across every live body. This is the budget that bounds *work*
   rather than memory: the raster arena above is allocated once whatever
   happens, but every occupied cell is a cell collision may test and a cell the
   renderer will eventually draw, so a long series of explosions must not be
   able to accumulate them without limit. A quarter of the theoretical maximum
   (64 x 12288) buys either sixteen of the largest bodies the detector can now
   hand over or every slot filled with an ordinary one. */
#define MAX_TERRAIN_DYNAMIC_CELLS 196608

#endif
