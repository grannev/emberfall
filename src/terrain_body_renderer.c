#include "terrain_body_renderer.h"

#include <stddef.h>
#include <string.h>

#include "material_render.h"

/* Moving bodies do not yet sample the world's coarse light field: that would
   introduce a World dependency into this renderer. A stable neutral factor
   keeps the shared palette readable while hot/emissive cells still light
   themselves through the explicit emissive layer. */
#define TERRAIN_BODY_AMBIENT_LIGHT 0.72f
#define TERRAIN_BODY_TEXTURE_RETRY_FRAMES 120u

_Static_assert(sizeof(Color) == 4u,
               "Terrain body texture accounting assumes RGBA8 Color");

static bool TerrainBodyTextureIsValid(Texture2D texture)
{
    return texture.id != 0u;
}

static TerrainBodyRenderKey TerrainBodyRendererInvalidKey(void)
{
    return (TerrainBodyRenderKey){
        .handle = {TERRAIN_BODY_INVALID_INDEX, 0u},
    };
}

static void TerrainBodyRendererUnloadTextures(TerrainBodyTextureSlot *slot)
{
    if (TerrainBodyTextureIsValid(slot->sceneTexture)) {
        UnloadTexture(slot->sceneTexture);
    }
    if (TerrainBodyTextureIsValid(slot->emissiveTexture)) {
        UnloadTexture(slot->emissiveTexture);
    }
    slot->sceneTexture = (Texture2D){0};
    slot->emissiveTexture = (Texture2D){0};
    slot->hasEmission = false;
}

static void TerrainBodyRendererResetSlot(TerrainBodyTextureSlot *slot)
{
    TerrainBodyRendererUnloadTextures(slot);
    *slot = (TerrainBodyTextureSlot){0};
    slot->key = TerrainBodyRendererInvalidKey();
}

static bool TerrainBodyRendererSameIdentity(TerrainBodyRenderKey a,
                                            TerrainBodyRenderKey b)
{
    return TerrainBodyHandleEquals(a.handle, b.handle) &&
           a.width == b.width && a.height == b.height;
}

static bool TerrainBodyRendererBuildPixels(
    TerrainBodyRenderer *renderer, const DynamicTerrainSystem *terrain,
    TerrainBodyRenderKey key, bool *hasEmission)
{
    const TerrainBody *body = DynamicTerrainGetConst(terrain, key.handle);
    int cellCount;
    int localY;

    if (body == NULL || !TerrainBodyRenderIsDrawable(body) ||
        key.width != body->width || key.height != body->height) {
        return false;
    }
    cellCount = body->width * body->height;
    if (cellCount <= 0 || cellCount > TERRAIN_BODY_RASTER_CAPACITY) {
        return false;
    }

    *hasEmission = false;
    for (localY = 0; localY < body->height; ++localY) {
        int localX;

        for (localX = 0; localX < body->width; ++localX) {
            int index = localY * body->width + localX;
            CellMaterial material = DynamicTerrainCellAt(
                terrain, key.handle, localX, localY);

            if (material == MATERIAL_EMPTY) {
                renderer->sceneStaging[index] = BLANK;
                renderer->emissiveStaging[index] = BLANK;
            } else {
                MaterialRenderSample sample = MaterialRenderCell(
                    material,
                    DynamicTerrainTemperatureAt(terrain, key.handle,
                                                localX, localY),
                    body->sourceX + localX, body->sourceY + localY,
                    TERRAIN_BODY_AMBIENT_LIGHT,
                    TERRAIN_BODY_AMBIENT_LIGHT,
                    TERRAIN_BODY_AMBIENT_LIGHT);

                renderer->sceneStaging[index] = sample.scene;
                renderer->emissiveStaging[index] = sample.emissive;
                if (sample.emissive.a != 0u) {
                    *hasEmission = true;
                }
            }
        }
    }
    return true;
}

static bool TerrainBodyRendererCreateTextures(
    TerrainBodyRenderer *renderer, TerrainBodyTextureSlot *slot,
    const DynamicTerrainSystem *terrain, TerrainBodyRenderKey key)
{
    Image sceneImage;
    Image emissiveImage;
    Texture2D scene;
    Texture2D emissive;
    bool hasEmission;

    if (!TerrainBodyRendererBuildPixels(renderer, terrain, key,
                                        &hasEmission)) {
        return false;
    }
    sceneImage = (Image){renderer->sceneStaging, key.width, key.height, 1,
                         PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    emissiveImage = (Image){renderer->emissiveStaging, key.width, key.height,
                            1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    scene = LoadTextureFromImage(sceneImage);
    emissive = LoadTextureFromImage(emissiveImage);
    if (!TerrainBodyTextureIsValid(scene) ||
        !TerrainBodyTextureIsValid(emissive)) {
        if (TerrainBodyTextureIsValid(scene)) {
            UnloadTexture(scene);
        }
        if (TerrainBodyTextureIsValid(emissive)) {
            UnloadTexture(emissive);
        }
        return false;
    }

    SetTextureFilter(scene, TEXTURE_FILTER_POINT);
    SetTextureFilter(emissive, TEXTURE_FILTER_POINT);
    SetTextureWrap(scene, TEXTURE_WRAP_CLAMP);
    SetTextureWrap(emissive, TEXTURE_WRAP_CLAMP);
    slot->sceneTexture = scene;
    slot->emissiveTexture = emissive;
    slot->hasEmission = hasEmission;
    slot->key = key;
    renderer->lastFrame.textureUpdates += 2u;
    return true;
}

static bool TerrainBodyRendererUpdateTextures(
    TerrainBodyRenderer *renderer, TerrainBodyTextureSlot *slot,
    const DynamicTerrainSystem *terrain, TerrainBodyRenderKey key)
{
    bool hasEmission;

    if (!TerrainBodyRendererBuildPixels(renderer, terrain, key,
                                        &hasEmission)) {
        return false;
    }
    UpdateTexture(slot->sceneTexture, renderer->sceneStaging);
    UpdateTexture(slot->emissiveTexture, renderer->emissiveStaging);
    slot->hasEmission = hasEmission;
    slot->key = key;
    renderer->lastFrame.textureUpdates += 2u;
    return true;
}

static void TerrainBodyRendererSynchronize(
    TerrainBodyRenderer *renderer, const DynamicTerrainSystem *terrain)
{
    uint16_t bodyIndex;

    for (bodyIndex = 0u; bodyIndex < MAX_TERRAIN_BODIES; ++bodyIndex) {
        TerrainBodyTextureSlot *slot = &renderer->slots[bodyIndex];
        TerrainBodyRenderKey key = TerrainBodyRenderKeyAt(terrain, bodyIndex);
        const TerrainBody *body;
        bool identityChanged;

        if (!TerrainBodyRenderKeyIsLive(key)) {
            if (TerrainBodyRenderKeyIsLive(slot->key) ||
                TerrainBodyTextureIsValid(slot->sceneTexture) ||
                TerrainBodyTextureIsValid(slot->emissiveTexture)) {
                TerrainBodyRendererResetSlot(slot);
            }
            continue;
        }

        identityChanged = !TerrainBodyRendererSameIdentity(slot->key, key);
        if (identityChanged) {
            TerrainBodyRendererResetSlot(slot);
            slot->key = key;
        } else if (slot->key.rasterRevision != key.rasterRevision &&
                   !TerrainBodyTextureIsValid(slot->sceneTexture)) {
            /* A new revision should not inherit the retry delay of content that
               no longer exists. Record it so a persistent allocation failure
               still backs off instead of retrying every frame. */
            slot->key.rasterRevision = key.rasterRevision;
            slot->retryFrames = 0u;
        }

        body = DynamicTerrainGetConst(terrain, key.handle);
        if (!TerrainBodyRenderIsDrawable(body)) {
            TerrainBodyRendererUnloadTextures(slot);
            slot->key = key;
            slot->retryFrames = 0u;
            continue;
        }

        if (!TerrainBodyTextureIsValid(slot->sceneTexture) ||
            !TerrainBodyTextureIsValid(slot->emissiveTexture)) {
            TerrainBodyRendererUnloadTextures(slot);
            if (slot->retryFrames > 0u) {
                --slot->retryFrames;
                continue;
            }
            if (!TerrainBodyRendererCreateTextures(renderer, slot, terrain,
                                                   key)) {
                slot->key = key;
                slot->retryFrames = TERRAIN_BODY_TEXTURE_RETRY_FRAMES;
                TraceLog(LOG_WARNING,
                         "RENDER: Terrain body %u textures unavailable; retrying later",
                         (unsigned int)bodyIndex);
                continue;
            }
        } else if (slot->key.rasterRevision != key.rasterRevision) {
            (void)TerrainBodyRendererUpdateTextures(renderer, slot, terrain,
                                                    key);
        }

        if (TerrainBodyTextureIsValid(slot->sceneTexture) &&
            TerrainBodyTextureIsValid(slot->emissiveTexture)) {
            uint64_t pixels = (uint64_t)slot->key.width *
                              (uint64_t)slot->key.height;

            ++renderer->lastFrame.cachedBodies;
            renderer->lastFrame.textureMemoryBytes +=
                pixels * 2u * (uint64_t)sizeof(Color);
        }
    }
}

static void TerrainBodyRendererDrawTexture(Texture2D texture,
                                           const TerrainBody *body)
{
    Rectangle source = {0.0f, 0.0f, (float)body->width,
                        (float)body->height};
    Rectangle destination = {body->position.x, body->position.y,
                             (float)body->width, (float)body->height};

    /* raylib interprets destination.x/y as the rotation position and origin as
       an offset inside that destination. Using the simulation COM verbatim
       therefore implements position + rotate(local - COM, angle). */
    DrawTexturePro(texture, source, destination, body->centerOfMass,
                   body->angle * RAD2DEG, WHITE);
}

void TerrainBodyRendererInit(TerrainBodyRenderer *renderer)
{
    uint16_t bodyIndex;

    if (renderer == NULL) {
        return;
    }
    memset(renderer, 0, sizeof(*renderer));
    for (bodyIndex = 0u; bodyIndex < MAX_TERRAIN_BODIES; ++bodyIndex) {
        renderer->slots[bodyIndex].key = TerrainBodyRendererInvalidKey();
    }
}

void TerrainBodyRendererDrawScene(TerrainBodyRenderer *renderer,
                                  const DynamicTerrainSystem *terrain,
                                  Rectangle visible)
{
    uint16_t bodyIndex;

    if (renderer == NULL || terrain == NULL) {
        return;
    }
    renderer->lastFrame = (TerrainBodyRendererStats){0};
    TerrainBodyRendererSynchronize(renderer, terrain);

    for (bodyIndex = 0u; bodyIndex < MAX_TERRAIN_BODIES; ++bodyIndex) {
        TerrainBodyTextureSlot *slot = &renderer->slots[bodyIndex];
        const TerrainBody *body = DynamicTerrainGetConst(terrain,
                                                         slot->key.handle);

        if (body == NULL ||
            slot->key.rasterRevision != body->rasterRevision ||
            !TerrainBodyTextureIsValid(slot->sceneTexture) ||
            !TerrainBodyRenderIntersects(body, visible)) {
            continue;
        }
        TerrainBodyRendererDrawTexture(slot->sceneTexture, body);
        ++renderer->lastFrame.visibleBodies;
        ++renderer->lastFrame.drawCalls;
    }
}

void TerrainBodyRendererDrawEmissive(TerrainBodyRenderer *renderer,
                                     const DynamicTerrainSystem *terrain,
                                     Rectangle visible)
{
    uint16_t bodyIndex;

    if (renderer == NULL || terrain == NULL) {
        return;
    }
    for (bodyIndex = 0u; bodyIndex < MAX_TERRAIN_BODIES; ++bodyIndex) {
        TerrainBodyTextureSlot *slot = &renderer->slots[bodyIndex];
        const TerrainBody *body = DynamicTerrainGetConst(terrain,
                                                         slot->key.handle);

        if (body == NULL || !slot->hasEmission ||
            slot->key.rasterRevision != body->rasterRevision ||
            !TerrainBodyTextureIsValid(slot->emissiveTexture) ||
            !TerrainBodyRenderIntersects(body, visible)) {
            continue;
        }
        TerrainBodyRendererDrawTexture(slot->emissiveTexture, body);
        ++renderer->lastFrame.drawCalls;
    }
}

const TerrainBodyRendererStats *TerrainBodyRendererStatistics(
    const TerrainBodyRenderer *renderer)
{
    static const TerrainBodyRendererStats empty = {0};

    return renderer != NULL ? &renderer->lastFrame : &empty;
}

void TerrainBodyRendererUnload(TerrainBodyRenderer *renderer)
{
    uint16_t bodyIndex;

    if (renderer == NULL) {
        return;
    }
    for (bodyIndex = 0u; bodyIndex < MAX_TERRAIN_BODIES; ++bodyIndex) {
        TerrainBodyRendererResetSlot(&renderer->slots[bodyIndex]);
    }
    memset(renderer, 0, sizeof(*renderer));
}
