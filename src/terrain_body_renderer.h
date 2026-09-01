#ifndef TERRAIN_BODY_RENDERER_H
#define TERRAIN_BODY_RENDERER_H

/* GPU presentation cache for DynamicTerrainSystem.
 *
 * The simulation owns bodies and rasters. This module only reads them and owns
 * every Texture2D created from them. One cache slot maps to one generation-
 * checked simulation slot; no GPU object leaks into TerrainBody.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "dynamic_terrain.h"
#include "terrain_body_render_data.h"

typedef struct TerrainBodyRendererStats {
    uint32_t cachedBodies;
    uint32_t visibleBodies;
    uint32_t drawCalls;
    uint32_t textureUpdates;
    uint64_t textureMemoryBytes;
} TerrainBodyRendererStats;

typedef struct TerrainBodyTextureSlot {
    Texture2D sceneTexture;
    Texture2D emissiveTexture;
    TerrainBodyRenderKey key;
    uint16_t retryFrames;
    bool hasEmission;
} TerrainBodyTextureSlot;

typedef struct TerrainBodyRenderer {
    TerrainBodyTextureSlot slots[MAX_TERRAIN_BODIES];
    /* Reused upload storage: two RGBA8 layers at the largest legal raster.
       It is renderer-owned and makes new/dirty body uploads allocation-free on
       the CPU side. */
    Color sceneStaging[TERRAIN_BODY_RASTER_CAPACITY];
    Color emissiveStaging[TERRAIN_BODY_RASTER_CAPACITY];
    TerrainBodyRendererStats lastFrame;
} TerrainBodyRenderer;

void TerrainBodyRendererInit(TerrainBodyRenderer *renderer);
void TerrainBodyRendererDrawScene(TerrainBodyRenderer *renderer,
                                  const DynamicTerrainSystem *terrain,
                                  Rectangle visible);
void TerrainBodyRendererDrawEmissive(TerrainBodyRenderer *renderer,
                                     const DynamicTerrainSystem *terrain,
                                     Rectangle visible);
const TerrainBodyRendererStats *TerrainBodyRendererStatistics(
    const TerrainBodyRenderer *renderer);
void TerrainBodyRendererUnload(TerrainBodyRenderer *renderer);

#endif
