#include "terrain_body_render_data.h"

#include <math.h>
#include <stddef.h>

static TerrainBodyRenderKey TerrainBodyRenderInvalidKey(void)
{
    return (TerrainBodyRenderKey){
        .handle = {TERRAIN_BODY_INVALID_INDEX, 0u},
    };
}

TerrainBodyRenderKey TerrainBodyRenderKeyAt(
    const DynamicTerrainSystem *system, uint16_t bodyIndex)
{
    const TerrainBody *body;

    if (system == NULL || bodyIndex >= MAX_TERRAIN_BODIES) {
        return TerrainBodyRenderInvalidKey();
    }
    body = &system->bodies[bodyIndex];
    if (!body->active || body->generation == 0u) {
        return TerrainBodyRenderInvalidKey();
    }
    return (TerrainBodyRenderKey){
        .handle = {bodyIndex, body->generation},
        .rasterRevision = body->rasterRevision,
        .width = body->width,
        .height = body->height,
    };
}

bool TerrainBodyRenderKeyIsLive(TerrainBodyRenderKey key)
{
    return key.handle.index < MAX_TERRAIN_BODIES &&
           key.handle.generation != 0u;
}

bool TerrainBodyRenderKeyEquals(TerrainBodyRenderKey a,
                                TerrainBodyRenderKey b)
{
    return TerrainBodyHandleEquals(a.handle, b.handle) &&
           a.rasterRevision == b.rasterRevision && a.width == b.width &&
           a.height == b.height;
}

bool TerrainBodyRenderIsDrawable(const TerrainBody *body)
{
    if (body == NULL || !body->active || body->cellCount <= 0 ||
        !(body->mass > 0.0f) || body->width <= 0 || body->height <= 0 ||
        body->width > TERRAIN_BODY_MAX_SPAN ||
        body->height > TERRAIN_BODY_MAX_SPAN ||
        body->width * body->height > TERRAIN_BODY_RASTER_CAPACITY ||
        body->minimumX < 0 || body->minimumY < 0 ||
        body->maximumX < body->minimumX ||
        body->maximumY < body->minimumY ||
        body->maximumX >= body->width || body->maximumY >= body->height) {
        return false;
    }
    return TerrainFiniteSample(body->position) &&
           TerrainFiniteSample(body->centerOfMass) && isfinite(body->angle);
}

Rectangle TerrainBodyRenderWorldBounds(const TerrainBody *body)
{
    Vector2 corners[4];
    float minimumX;
    float minimumY;
    float maximumX;
    float maximumY;
    int corner;

    if (!TerrainBodyRenderIsDrawable(body)) {
        return (Rectangle){0};
    }

    /* Occupied-cell bounds are corner coordinates: maximum + 1 includes the
       far edge of the final pixel. Going through the simulation transform is
       what keeps culling and DrawTexturePro on the same COM convention. */
    corners[0] = TerrainBodyLocalToWorld(
        body, (float)body->minimumX, (float)body->minimumY);
    corners[1] = TerrainBodyLocalToWorld(
        body, (float)body->maximumX + 1.0f, (float)body->minimumY);
    corners[2] = TerrainBodyLocalToWorld(
        body, (float)body->maximumX + 1.0f,
        (float)body->maximumY + 1.0f);
    corners[3] = TerrainBodyLocalToWorld(
        body, (float)body->minimumX, (float)body->maximumY + 1.0f);

    minimumX = maximumX = corners[0].x;
    minimumY = maximumY = corners[0].y;
    for (corner = 1; corner < 4; ++corner) {
        if (corners[corner].x < minimumX) minimumX = corners[corner].x;
        if (corners[corner].x > maximumX) maximumX = corners[corner].x;
        if (corners[corner].y < minimumY) minimumY = corners[corner].y;
        if (corners[corner].y > maximumY) maximumY = corners[corner].y;
    }
    return (Rectangle){minimumX, minimumY, maximumX - minimumX,
                       maximumY - minimumY};
}

bool TerrainBodyRenderIntersects(const TerrainBody *body, Rectangle visible)
{
    Rectangle bounds;

    if (visible.width <= 0.0f || visible.height <= 0.0f ||
        !isfinite(visible.x) || !isfinite(visible.y) ||
        !isfinite(visible.width) || !isfinite(visible.height)) {
        return false;
    }
    bounds = TerrainBodyRenderWorldBounds(body);
    if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
        return false;
    }
    return bounds.x < visible.x + visible.width &&
           bounds.x + bounds.width > visible.x &&
           bounds.y < visible.y + visible.height &&
           bounds.y + bounds.height > visible.y;
}
