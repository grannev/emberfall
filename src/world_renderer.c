#include "world_renderer.h"

#include <stddef.h>
#include <string.h>

#include "world_render_data.h"

static void WorldRendererUploadChunk(void *context, Rectangle bounds,
                                     const Color *pixels)
{
    WorldRenderer *renderer = context;
    uint64_t pixelCount = (uint64_t)bounds.width * (uint64_t)bounds.height;

    UpdateTextureRec(renderer->texture, bounds, pixels);
    ++renderer->lastFrame.dirtyRegions;
    ++renderer->lastFrame.textureUploads;
    renderer->lastFrame.uploadedBytes += pixelCount * sizeof(*pixels);
}

bool WorldRendererInit(WorldRenderer *renderer, const World *world)
{
    Image image;

    if (renderer == NULL || world == NULL || world->cells == NULL) {
        return false;
    }
    memset(renderer, 0, sizeof(*renderer));
    image = GenImageColor(world->width, world->height, BLACK);
    renderer->texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (renderer->texture.id == 0u) {
        return false;
    }
    SetTextureFilter(renderer->texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(renderer->texture, TEXTURE_WRAP_CLAMP);
    return true;
}

void WorldRendererDraw(WorldRenderer *renderer, World *world, Rectangle visible)
{
    double started;

    if (renderer == NULL || world == NULL || renderer->texture.id == 0u) {
        return;
    }
    renderer->lastFrame = (WorldRendererStats){0};
    started = GetTime();
    WorldPrepareVisible(world, visible, WorldRendererUploadChunk, renderer);
    renderer->lastFrame.preparationMilliseconds = (GetTime() - started) * 1000.0;
    DrawTexture(renderer->texture, 0, 0, WHITE);
}

void WorldRendererUnload(WorldRenderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    if (renderer->texture.id != 0u) {
        UnloadTexture(renderer->texture);
    }
    memset(renderer, 0, sizeof(*renderer));
}
