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

/* Bodies alive at once. Sixty-four was reached in ordinary play: a cave-in
   that peels a cliff into slabs, each slab cracking as it lands, ran out of
   slots in seconds, and a refused slot is a piece of rock that stays hanging
   in the air or two rocks that move as one. Two hundred and fifty-six bounds
   how much is loose at one time rather than how much has ever come loose,
   since settled rubble gives its slot back — see terrain_weld.h — and every
   per-slot cost below is sized by it: the raster arena is 35 MiB, the
   contact workspace a few hundred KiB, and a sleeping body costs a bounds
   check a substep and nothing else. */
#define MAX_TERRAIN_BODIES 256

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
   256 x 27648 x (1 byte material + 4 bytes temperature) = 33.75 MiB,
   allocated once and never resized. Collision also owns a fixed surface
   list. */
#define MAX_TERRAIN_RASTER_CELLS (MAX_TERRAIN_BODIES * TERRAIN_BODY_RASTER_CAPACITY)

/* Occupied cells across every live body. This is the budget that bounds *work*
   rather than memory: the raster arena above is allocated once whatever
   happens, but every occupied cell is a cell collision may test and a cell the
   renderer will eventually draw, so a long series of explosions must not be
   able to accumulate them without limit. A quarter of the theoretical maximum
   (256 x 12288) buys either sixty-four of the largest bodies the detector can
   hand over or every slot filled with an ordinary one. */
#define MAX_TERRAIN_DYNAMIC_CELLS 786432

#endif
