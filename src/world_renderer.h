#ifndef WORLD_RENDERER_H
#define WORLD_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

typedef struct WorldRendererStats {
    uint32_t dirtyRegions;
    uint32_t textureUploads;
    uint64_t uploadedBytes;
    double preparationMilliseconds;
} WorldRendererStats;

typedef struct WorldRenderer {
    Texture2D texture;
    WorldRendererStats lastFrame;
} WorldRenderer;

bool WorldRendererInit(WorldRenderer *renderer, const World *world);
void WorldRendererDraw(WorldRenderer *renderer, World *world, Rectangle visible);
void WorldRendererUnload(WorldRenderer *renderer);

#endif
