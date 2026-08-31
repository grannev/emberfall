#include "world_renderer.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "world_render_data.h"

/* Slots are created on demand and never shrink, so a window resize grows the
   cache once and steady-state rendering allocates nothing. The ceiling exists
   only so that a pathological view cannot ask for gigabytes of VRAM. */
#define WORLD_RENDER_PAGE_LIMIT 256

typedef struct PageUploadContext {
    WorldRenderer *renderer;
    const World *world;
} PageUploadContext;

static int PageIndexOfSlot(const WorldRenderer *renderer, int pageX, int pageY)
{
    int slot;

    for (slot = 0; slot < renderer->pageCapacity; ++slot) {
        if (renderer->pages[slot].pageX == pageX &&
            renderer->pages[slot].pageY == pageY) {
            return slot;
        }
    }
    return -1;
}

static bool WorldRendererGrow(WorldRenderer *renderer, int wanted)
{
    WorldRenderPage *grown;
    Image blank;
    int slot;

    if (wanted <= renderer->pageCapacity) {
        return true;
    }
    if (wanted > WORLD_RENDER_PAGE_LIMIT) {
        wanted = WORLD_RENDER_PAGE_LIMIT;
    }
    grown = realloc(renderer->pages, (size_t)wanted * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    renderer->pages = grown;

    blank = GenImageColor(WORLD_RENDER_PAGE_SIZE, WORLD_RENDER_PAGE_SIZE, BLACK);
    for (slot = renderer->pageCapacity; slot < wanted; ++slot) {
        renderer->pages[slot].texture = LoadTextureFromImage(blank);
        renderer->pages[slot].pageX = -1;
        renderer->pages[slot].pageY = -1;
        renderer->pages[slot].lastUsedFrame = 0u;
        if (renderer->pages[slot].texture.id == 0u) {
            break;
        }
        SetTextureFilter(renderer->pages[slot].texture, TEXTURE_FILTER_POINT);
        SetTextureWrap(renderer->pages[slot].texture, TEXTURE_WRAP_CLAMP);
    }
    UnloadImage(blank);
    renderer->pageCapacity = slot;
    return renderer->pageCapacity > 0;
}

/* Returns the slot holding this page, binding a free or least-recently-used
   slot to it if necessary. A freshly bound slot holds someone else's pixels, so
   the caller is told to have the world rebuild the whole page. */
static int WorldRendererAcquirePage(WorldRenderer *renderer, int pageX, int pageY,
                                    bool *bound)
{
    int slot = PageIndexOfSlot(renderer, pageX, pageY);
    int oldest;
    int candidate;

    *bound = false;
    if (slot >= 0) {
        renderer->pages[slot].lastUsedFrame = renderer->frame;
        return slot;
    }

    oldest = 0;
    for (candidate = 0; candidate < renderer->pageCapacity; ++candidate) {
        if (renderer->pages[candidate].pageX < 0) {
            oldest = candidate;
            break;
        }
        /* Never evict a page already claimed this frame: it is on screen. */
        if (renderer->pages[candidate].lastUsedFrame == renderer->frame) {
            continue;
        }
        if (renderer->pages[candidate].lastUsedFrame <
            renderer->pages[oldest].lastUsedFrame ||
            renderer->pages[oldest].lastUsedFrame == renderer->frame) {
            oldest = candidate;
        }
    }
    if (renderer->pages[oldest].lastUsedFrame == renderer->frame &&
        renderer->pages[oldest].pageX >= 0) {
        return -1;
    }

    renderer->pages[oldest].pageX = pageX;
    renderer->pages[oldest].pageY = pageY;
    renderer->pages[oldest].lastUsedFrame = renderer->frame;
    ++renderer->lastFrame.pageBinds;
    *bound = true;
    return oldest;
}

static bool WorldRendererUploadChunk(void *context, Rectangle bounds,
                                     const Color *pixels)
{
    PageUploadContext *upload = context;
    WorldRenderer *renderer = upload->renderer;
    int pageX = (int)bounds.x / WORLD_RENDER_PAGE_SIZE;
    int pageY = (int)bounds.y / WORLD_RENDER_PAGE_SIZE;
    int slot = PageIndexOfSlot(renderer, pageX, pageY);
    Rectangle local;
    uint64_t pixelCount;

    /* A chunk outside the resident set has nowhere to go. It keeps its dirty
       flag and is uploaded on the frame its page becomes resident. */
    if (slot < 0) {
        return false;
    }
    local = (Rectangle){bounds.x - (float)(pageX * WORLD_RENDER_PAGE_SIZE),
                        bounds.y - (float)(pageY * WORLD_RENDER_PAGE_SIZE),
                        bounds.width, bounds.height};
    pixelCount = (uint64_t)bounds.width * (uint64_t)bounds.height;

    UpdateTextureRec(renderer->pages[slot].texture, local, pixels);
    ++renderer->lastFrame.dirtyRegions;
    ++renderer->lastFrame.textureUploads;
    renderer->lastFrame.uploadedBytes += pixelCount * sizeof(*pixels);
    return true;
}

bool WorldRendererInit(WorldRenderer *renderer, const World *world)
{
    if (renderer == NULL || world == NULL || world->cells == NULL) {
        return false;
    }
    memset(renderer, 0, sizeof(*renderer));
    /* Enough for a default window; the cache grows if a larger view asks for
       more, which in practice happens once, on a resize. */
    return WorldRendererGrow(renderer, 12);
}

void WorldRendererDraw(WorldRenderer *renderer, World *world, Rectangle visible)
{
    PageUploadContext upload;
    double started;
    int firstPageX;
    int lastPageX;
    int firstPageY;
    int lastPageY;
    int pagesAcross;
    int pagesDown;
    int wanted;
    int pageY;

    if (renderer == NULL || world == NULL || renderer->pages == NULL) {
        return;
    }
    renderer->lastFrame = (WorldRendererStats){0};
    ++renderer->frame;
    started = GetTime();

    firstPageX = (int)floorf(visible.x / (float)WORLD_RENDER_PAGE_SIZE);
    lastPageX = (int)floorf((visible.x + visible.width) /
                            (float)WORLD_RENDER_PAGE_SIZE);
    firstPageY = (int)floorf(visible.y / (float)WORLD_RENDER_PAGE_SIZE);
    lastPageY = (int)floorf((visible.y + visible.height) /
                            (float)WORLD_RENDER_PAGE_SIZE);
    if (firstPageX < 0) firstPageX = 0;
    if (firstPageY < 0) firstPageY = 0;
    if (lastPageX > (world->width - 1) / WORLD_RENDER_PAGE_SIZE) {
        lastPageX = (world->width - 1) / WORLD_RENDER_PAGE_SIZE;
    }
    if (lastPageY > (world->height - 1) / WORLD_RENDER_PAGE_SIZE) {
        lastPageY = (world->height - 1) / WORLD_RENDER_PAGE_SIZE;
    }
    if (firstPageX > lastPageX || firstPageY > lastPageY) {
        return;
    }

    pagesAcross = lastPageX - firstPageX + 1;
    pagesDown = lastPageY - firstPageY + 1;
    /* One spare row and column so panning does not evict a page that is about
       to be needed again. */
    wanted = (pagesAcross + 1) * (pagesDown + 1);
    (void)WorldRendererGrow(renderer, wanted);

    /* Claim every visible page first, so the rebuild pass below can find the
       slot for any chunk it is handed, and so a page that has just been bound
       is filled on this frame rather than showing a stale neighbour's pixels
       for one frame. */
    for (pageY = firstPageY; pageY <= lastPageY; ++pageY) {
        int pageX;

        for (pageX = firstPageX; pageX <= lastPageX; ++pageX) {
            bool bound = false;
            int slot = WorldRendererAcquirePage(renderer, pageX, pageY, &bound);

            if (slot < 0) {
                continue;
            }
            ++renderer->lastFrame.visiblePages;
            if (bound) {
                WorldMarkRegionDirty(
                    world,
                    (Rectangle){(float)(pageX * WORLD_RENDER_PAGE_SIZE),
                                (float)(pageY * WORLD_RENDER_PAGE_SIZE),
                                (float)WORLD_RENDER_PAGE_SIZE,
                                (float)WORLD_RENDER_PAGE_SIZE});
            }
        }
    }

    upload.renderer = renderer;
    upload.world = world;
    WorldPrepareVisible(world, visible, WorldRendererUploadChunk, &upload);
    renderer->lastFrame.preparationMilliseconds = (GetTime() - started) * 1000.0;

    for (pageY = firstPageY; pageY <= lastPageY; ++pageY) {
        int pageX;

        for (pageX = firstPageX; pageX <= lastPageX; ++pageX) {
            int slot = PageIndexOfSlot(renderer, pageX, pageY);
            int originX = pageX * WORLD_RENDER_PAGE_SIZE;
            int originY = pageY * WORLD_RENDER_PAGE_SIZE;
            int width = WORLD_RENDER_PAGE_SIZE;
            int height = WORLD_RENDER_PAGE_SIZE;

            if (slot < 0) {
                continue;
            }
            /* The last page of a row or column hangs over the edge of the
               world; only the part backed by real cells is drawn. */
            if (originX + width > world->width) width = world->width - originX;
            if (originY + height > world->height) height = world->height - originY;
            DrawTextureRec(renderer->pages[slot].texture,
                           (Rectangle){0.0f, 0.0f, (float)width, (float)height},
                           (Vector2){(float)originX, (float)originY}, WHITE);
        }
    }
    renderer->lastFrame.residentPages = (uint32_t)renderer->pageCapacity;
}

void WorldRendererUnload(WorldRenderer *renderer)
{
    int slot;

    if (renderer == NULL) {
        return;
    }
    for (slot = 0; slot < renderer->pageCapacity; ++slot) {
        if (renderer->pages[slot].texture.id != 0u) {
            UnloadTexture(renderer->pages[slot].texture);
        }
    }
    free(renderer->pages);
    memset(renderer, 0, sizeof(*renderer));
}
