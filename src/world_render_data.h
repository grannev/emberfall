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
typedef bool (*WorldRenderChunkVisitor)(void *context, Rectangle bounds,
                                        const Color *pixels,
                                        const Color *emissivePixels);

/* `maxChunks` caps how many chunks may be rebuilt in one call; zero or less
   means no cap. The cap has to live here rather than in the visitor because by
   the time the visitor is called the expensive part is already done: a chunk is
   a thousand cells of pixel conversion with a bilinear light sample each, and
   refusing the finished block only saves the upload. A chunk not reached keeps
   its dirty flag and is built by a later call. */
void WorldPrepareVisible(World *world, Rectangle visible, int maxChunks,
                         WorldRenderChunkVisitor visitor, void *context);

/* Marks every chunk overlapping `region` as owing the renderer a rebuild. The
   renderer calls this when a page cache slot is bound to a new page and its
   texture therefore holds someone else's pixels. */
void WorldMarkRegionDirty(World *world, Rectangle region);

#endif
