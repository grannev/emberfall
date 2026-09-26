#ifndef WORLD_RENDER_DATA_H
#define WORLD_RENDER_DATA_H

#include <stdbool.h>

#include <raylib.h>

#include "world.h"

/* Internal bridge between CPU world data and the renderer. The callback is
   invoked synchronously while a stack-backed chunk staging block is valid.

   It returns whether it consumed the block. A chunk the renderer could not
   place — its page is not resident — keeps its dirty flag and is rebuilt on
   the frame that page arrives, instead of being silently dropped and leaving
   stale pixels on screen until something else happens to change it. */
/* `floraPixels` is the plants' own layer: a plant's colour where one stands,
   its alpha carrying how freely that pixel sways (see
   MATERIAL_RENDER_FLORA_ALPHA), and nothing elsewhere. Where a plant stands,
   `pixels` and `emissivePixels` hold what is behind it — the back wall or the
   air — so the plant can move without leaving a hole. */
typedef bool (*WorldRenderChunkVisitor)(void *context, Rectangle bounds,
                                        const Color *pixels,
                                        const Color *emissivePixels,
                                        const Color *floraPixels);

void WorldPrepareVisible(World *world, Rectangle visible,
                         WorldRenderChunkVisitor visitor, void *context);

/* Marks every chunk overlapping `region` as owing the renderer a rebuild. The
   renderer calls this when a page cache slot is bound to a new page and its
   texture therefore holds someone else's pixels. */
void WorldMarkRegionDirty(World *world, Rectangle region);

#endif
