#ifndef WORLD_RENDERER_H
#define WORLD_RENDERER_H

/* Paged world rendering.
 *
 * The renderer used to hold one Texture2D the size of the whole world. That
 * worked, but it made the maximum world width a property of the GPU: at
 * 16384x864 the map was already within sight of the smallest guaranteed
 * GL_MAX_TEXTURE_SIZE, and it cost 54 MiB of VRAM for a map of which a few
 * per cent is ever on screen.
 *
 * Instead the world is cut into fixed pages, and only the pages the camera can
 * see are resident. A page keeps its texture until it is evicted, so panning
 * costs one page upload at the leading edge rather than anything proportional
 * to the map. World size is now bounded by memory and by the simulation, not by
 * a texture dimension.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

/* Cells per page: 256 KiB per RGBA8 layer, 512 KiB for the resident scene and
   emissive pair. Small enough that panning uploads little, large enough that a
   full view is a couple of dozen draw calls rather than a couple of thousand.
   A multiple of WORLD_CHUNK_SIZE, so a dirty chunk always lies inside exactly
   one page. */
#define WORLD_RENDER_PAGE_SIZE 256

_Static_assert(WORLD_RENDER_PAGE_SIZE % WORLD_CHUNK_SIZE == 0,
               "a render page must be a whole number of simulation chunks");

typedef struct WorldRendererStats {
    uint32_t dirtyRegions;
    uint32_t textureUploads;
    uint64_t uploadedBytes;
    uint32_t residentPages;
    uint32_t visiblePages;
    uint32_t pageBinds;
    double preparationMilliseconds;
} WorldRendererStats;

typedef struct WorldRenderPage {
    Texture2D texture;
    Texture2D emissiveTexture;
    /* Which page of the world this texture currently holds, or -1 when the
       slot has never been bound. */
    int pageX;
    int pageY;
    uint32_t lastUsedFrame;
} WorldRenderPage;

typedef struct WorldRenderer {
    WorldRenderPage *pages;
    int pageCapacity;
    uint32_t frame;
    WorldRendererStats lastFrame;
} WorldRenderer;

bool WorldRendererInit(WorldRenderer *renderer, const World *world);
void WorldRendererDraw(WorldRenderer *renderer, World *world, Rectangle visible);
void WorldRendererDrawEmissive(const WorldRenderer *renderer, const World *world,
                               Rectangle visible);
void WorldRendererUnload(WorldRenderer *renderer);

#endif
