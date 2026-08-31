#ifndef WORLD_RENDER_DATA_H
#define WORLD_RENDER_DATA_H

#include <raylib.h>

#include "world.h"

/* Internal bridge between CPU world data and the renderer. The callback is
   invoked synchronously while a stack-backed chunk staging block is valid. */
typedef void (*WorldRenderChunkVisitor)(void *context, Rectangle bounds,
                                        const Color *pixels);

void WorldPrepareVisible(World *world, Rectangle visible,
                         WorldRenderChunkVisitor visitor, void *context);

#endif
