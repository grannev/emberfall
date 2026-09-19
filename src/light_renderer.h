#ifndef LIGHT_RENDERER_H
#define LIGHT_RENDERER_H

/* The GPU half of world lighting.
 *
 * The world solves its coarse light field on the CPU; this module owns the
 * texture that field is uploaded into and the shader that lights unlit world
 * pixels from it. Everything drawn between LightRendererBegin and
 * LightRendererEnd — the world's pages, detached terrain bodies — is lit by
 * where it is in the world, not by what it is.
 *
 * This is what took lighting out of the chunk rebuild. Light used to be baked
 * into every page pixel, so a lamp moving one light cell rebuilt every chunk
 * it reached, a thousand cells each, every frame the player flew; the day
 * turning did the same to the whole screen. Now a moving lamp costs one solve
 * and one small texture upload, and a turning day costs a uniform.
 *
 * Presentation only: it reads the world's light arrays through a const pointer
 * after the world has solved them, and writes nothing back. If the shader is
 * missing or fails to compile, Begin and End do nothing and the world draws
 * unlit — playable, and flat.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

typedef enum LightPass {
    LIGHT_PASS_SCENE = 0,
    LIGHT_PASS_EMISSIVE = 1
} LightPass;

typedef struct LightRendererStats {
    bool enabled;
    uint32_t uploads;
    uint64_t uploadedBytes;
    /* Texels of the field the texture holds, for the HUD's memory line. */
    uint32_t texels;
    double syncMilliseconds;
} LightRendererStats;

typedef struct LightRenderer {
    Texture2D texture;
    Shader shader;
    int lightMapLocation;
    int lightTexelLocation;
    int lightScaleLocation;
    int daylightLocation;
    int minimumLightLocation;
    int warmthLocation;
    int veilLocation;
    int veilAlphaLocation;
    int airAlphaLocation;
    int emissivePassLocation;
    bool ready;
    int columns;
    int rows;
    /* Packs the region being uploaded, sized for the whole field once so a
       frame never allocates. */
    Color *staging;
    uint32_t uploadedRevision;
    /* The texel region the texture currently holds current values for. A
       view that moves onto texels outside it re-uploads even when the solve
       did not run. Inclusive. */
    int uploadedFirstColumn;
    int uploadedLastColumn;
    int uploadedFirstRow;
    int uploadedLastRow;
    LightRendererStats lastFrame;
} LightRenderer;

bool LightRendererInit(LightRenderer *renderer, const World *world);
/* Solves the world's light for `visible` and uploads whatever the texture is
   missing. Call once per frame before the lit passes. */
void LightRendererSync(LightRenderer *renderer, World *world, Rectangle visible);
/* Everything drawn until LightRendererEnd is lit from the field. Must be
   called inside the camera transform, because the shader reads the world
   position of every vertex it is handed. */
void LightRendererBegin(LightRenderer *renderer, const World *world,
                        LightPass pass);
void LightRendererEnd(const LightRenderer *renderer);
void LightRendererUnload(LightRenderer *renderer);
const LightRendererStats *LightRendererStatistics(const LightRenderer *renderer);

#endif
