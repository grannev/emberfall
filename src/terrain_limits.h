#ifndef TERRAIN_LIMITS_H
#define TERRAIN_LIMITS_H

#include <stddef.h>

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

/* Raster slots come in three sizes, because a body of a building's size is
   worth having and a store where every slot could hold one would spend most
   of its memory on pebbles. A body takes the smallest slot its bounding box
   fits, and a larger one when those are all in use:

     192 small slots   12288 raster cells (a 110x110 box, or 64x192)
      48 medium slots  36864 raster cells (192x192)
      16 large slots  147456 raster cells (384x384)

   A large slot holds four times the cells a body could have before — a
   piece of cliff the size of a hall — and the three together take less
   memory than 256 slots of the old single size. */
#define TERRAIN_BODY_SMALL_SLOTS 192
#define TERRAIN_BODY_MEDIUM_SLOTS 48
#define TERRAIN_BODY_LARGE_SLOTS 16
#define TERRAIN_BODY_SMALL_RASTER 12288
#define TERRAIN_BODY_MEDIUM_RASTER 36864
#define TERRAIN_BODY_RASTER_CAPACITY 147456

/* Longest side of a body's bounding box, matching the detector's own region
   limit so that anything WorldFindComponent can report as detached is a shape
   this can hold. */
#define TERRAIN_BODY_MAX_SPAN 384

/* Occupied cells in one body, inherited from WORLD_COMPONENT_MAX_CELLS: a body
   can only ever be built from a component the detector proved free. A slot
   holds at most this many or its raster's size, whichever is smaller, and
   its surface list is sized the same. */
#define MAX_TERRAIN_BODY_CELLS 49152

/* Total raster storage: 1 byte material + 4 bytes temperature + 1 byte tone
   per cell over every slot, about 37 MiB, allocated once and never resized.
   Collision also owns a fixed surface list. */
#define MAX_TERRAIN_RASTER_CELLS                                   \
    (TERRAIN_BODY_SMALL_SLOTS * TERRAIN_BODY_SMALL_RASTER +        \
     TERRAIN_BODY_MEDIUM_SLOTS * TERRAIN_BODY_MEDIUM_RASTER +      \
     TERRAIN_BODY_LARGE_SLOTS * TERRAIN_BODY_RASTER_CAPACITY)
#define MAX_TERRAIN_SURFACE_CELLS                                  \
    (TERRAIN_BODY_SMALL_SLOTS * TERRAIN_BODY_SMALL_RASTER +        \
     TERRAIN_BODY_MEDIUM_SLOTS * TERRAIN_BODY_MEDIUM_RASTER +      \
     TERRAIN_BODY_LARGE_SLOTS * MAX_TERRAIN_BODY_CELLS)

_Static_assert(TERRAIN_BODY_SMALL_SLOTS + TERRAIN_BODY_MEDIUM_SLOTS +
                       TERRAIN_BODY_LARGE_SLOTS ==
                   MAX_TERRAIN_BODIES,
               "the slot sizes must add up to the body count");

/* Where slot `index`'s raster and surface list start, and how much each
   holds. Inline: every per-cell loop over a body asks. */
static inline int TerrainSlotRasterCapacity(int index)
{
    return index < TERRAIN_BODY_SMALL_SLOTS ? TERRAIN_BODY_SMALL_RASTER
           : index < TERRAIN_BODY_SMALL_SLOTS + TERRAIN_BODY_MEDIUM_SLOTS
               ? TERRAIN_BODY_MEDIUM_RASTER
               : TERRAIN_BODY_RASTER_CAPACITY;
}

static inline int TerrainSlotCellCapacity(int index)
{
    int raster = TerrainSlotRasterCapacity(index);

    return raster < MAX_TERRAIN_BODY_CELLS ? raster : MAX_TERRAIN_BODY_CELLS;
}

static inline size_t TerrainSlotRasterBase(int index)
{
    if (index < TERRAIN_BODY_SMALL_SLOTS) {
        return (size_t)index * TERRAIN_BODY_SMALL_RASTER;
    }
    if (index < TERRAIN_BODY_SMALL_SLOTS + TERRAIN_BODY_MEDIUM_SLOTS) {
        return (size_t)TERRAIN_BODY_SMALL_SLOTS * TERRAIN_BODY_SMALL_RASTER +
               (size_t)(index - TERRAIN_BODY_SMALL_SLOTS) * TERRAIN_BODY_MEDIUM_RASTER;
    }
    return (size_t)TERRAIN_BODY_SMALL_SLOTS * TERRAIN_BODY_SMALL_RASTER +
           (size_t)TERRAIN_BODY_MEDIUM_SLOTS * TERRAIN_BODY_MEDIUM_RASTER +
           (size_t)(index - TERRAIN_BODY_SMALL_SLOTS - TERRAIN_BODY_MEDIUM_SLOTS) *
               TERRAIN_BODY_RASTER_CAPACITY;
}

static inline size_t TerrainSlotSurfaceBase(int index)
{
    if (index < TERRAIN_BODY_SMALL_SLOTS) {
        return (size_t)index * TERRAIN_BODY_SMALL_RASTER;
    }
    if (index < TERRAIN_BODY_SMALL_SLOTS + TERRAIN_BODY_MEDIUM_SLOTS) {
        return (size_t)TERRAIN_BODY_SMALL_SLOTS * TERRAIN_BODY_SMALL_RASTER +
               (size_t)(index - TERRAIN_BODY_SMALL_SLOTS) * TERRAIN_BODY_MEDIUM_RASTER;
    }
    return (size_t)TERRAIN_BODY_SMALL_SLOTS * TERRAIN_BODY_SMALL_RASTER +
           (size_t)TERRAIN_BODY_MEDIUM_SLOTS * TERRAIN_BODY_MEDIUM_RASTER +
           (size_t)(index - TERRAIN_BODY_SMALL_SLOTS - TERRAIN_BODY_MEDIUM_SLOTS) *
               MAX_TERRAIN_BODY_CELLS;
}

/* Occupied cells across every live body. This is the budget that bounds *work*
   rather than memory: the raster arena above is allocated once whatever
   happens, but every occupied cell is a cell collision may test and a cell the
   renderer will eventually draw, so a long series of explosions must not be
   able to accumulate them without limit. A million buys every large slot
   full of the largest bodies the detector can hand over with room to spare,
   or every slot filled with an ordinary one. */
#define MAX_TERRAIN_DYNAMIC_CELLS 1048576

#endif
