#ifndef TERRAIN_BODY_RENDER_DATA_H
#define TERRAIN_BODY_RENDER_DATA_H

/* Headless presentation metadata derived from TerrainBody.
 *
 * Keeping identity and culling math free of GPU calls makes generation reuse,
 * reset invalidation and the centre-of-mass transform regression-testable.
 * The GPU cache consumes these values but never writes gameplay state.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "dynamic_terrain.h"

typedef struct TerrainBodyRenderKey {
    TerrainBodyHandle handle;
    uint32_t rasterRevision;
    int width;
    int height;
} TerrainBodyRenderKey;

TerrainBodyRenderKey TerrainBodyRenderKeyAt(
    const DynamicTerrainSystem *system, uint16_t bodyIndex);
bool TerrainBodyRenderKeyIsLive(TerrainBodyRenderKey key);
bool TerrainBodyRenderKeyEquals(TerrainBodyRenderKey a,
                                TerrainBodyRenderKey b);

bool TerrainBodyRenderIsDrawable(const TerrainBody *body);
Rectangle TerrainBodyRenderWorldBounds(const TerrainBody *body);
bool TerrainBodyRenderIntersects(const TerrainBody *body, Rectangle visible);

#endif
