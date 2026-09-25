#include "light_renderer.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <rlgl.h>

#include "material_render.h"
#include "world_lighting.h"

#define LIGHT_VERTEX_SHADER "assets/shaders/world_light.vs"
#define LIGHT_FRAGMENT_SHADER "assets/shaders/world_light.fs"
/* Texels uploaded beyond the visible region on every side. Two, because the
   shader samples bilinearly and a view edge can sit inside a texel. */
#define LIGHT_UPLOAD_MARGIN 2

static bool LightRendererLoadShader(LightRenderer *renderer)
{
    char *vertex = LoadFileText(LIGHT_VERTEX_SHADER);
    char *fragment = LoadFileText(LIGHT_FRAGMENT_SHADER);

    if (vertex == NULL || fragment == NULL) {
        UnloadFileText(vertex);
        UnloadFileText(fragment);
        return false;
    }
    renderer->shader = LoadShaderFromMemory(vertex, fragment);
    UnloadFileText(vertex);
    UnloadFileText(fragment);
    if (!IsShaderValid(renderer->shader)) {
        return false;
    }

    renderer->lightMapLocation = GetShaderLocation(renderer->shader, "lightMap");
    renderer->lightTexelLocation = GetShaderLocation(renderer->shader, "lightTexel");
    renderer->lightScaleLocation = GetShaderLocation(renderer->shader, "lightScale");
    renderer->daylightLocation = GetShaderLocation(renderer->shader, "daylight");
    renderer->minimumLightLocation =
        GetShaderLocation(renderer->shader, "minimumLight");
    renderer->warmthLocation = GetShaderLocation(renderer->shader, "warmth");
    renderer->veilLocation = GetShaderLocation(renderer->shader, "veil");
    renderer->veilAlphaLocation = GetShaderLocation(renderer->shader, "veilAlpha");
    renderer->airAlphaLocation = GetShaderLocation(renderer->shader, "airAlpha");
    renderer->emissivePassLocation =
        GetShaderLocation(renderer->shader, "emissivePass");
    return renderer->lightMapLocation >= 0 && renderer->lightTexelLocation >= 0 &&
           renderer->lightScaleLocation >= 0 && renderer->daylightLocation >= 0 &&
           renderer->minimumLightLocation >= 0 && renderer->warmthLocation >= 0 &&
           renderer->veilLocation >= 0 && renderer->veilAlphaLocation >= 0 &&
           renderer->airAlphaLocation >= 0 &&
           renderer->emissivePassLocation >= 0;
}

/* The constants the world's own reference functions use, set once: they are
   properties of the world's lighting model, not of any frame. */
static void LightRendererSetConstants(const LightRenderer *renderer)
{
    Vector2 texel = {1.0f / (float)renderer->columns, 1.0f / (float)renderer->rows};
    float scale = (float)WORLD_LIGHT_SCALE;
    float minimum = WORLD_MINIMUM_LIGHT;
    Vector3 warmth = {WORLD_LIGHT_WARMTH_RED, WORLD_LIGHT_WARMTH_GREEN,
                      WORLD_LIGHT_WARMTH_BLUE};
    Vector2 veil = {WORLD_AIR_VEIL_OPEN, WORLD_AIR_VEIL_SEALED};
    Vector2 veilAlpha = {WORLD_AIR_VEIL_MINIMUM_ALPHA / 255.0f,
                         WORLD_AIR_VEIL_ALPHA_SPAN / 255.0f};
    float airAlpha = (float)MATERIAL_RENDER_AIR_ALPHA / 255.0f;

    SetShaderValue(renderer->shader, renderer->lightTexelLocation, &texel,
                   SHADER_UNIFORM_VEC2);
    SetShaderValue(renderer->shader, renderer->lightScaleLocation, &scale,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(renderer->shader, renderer->minimumLightLocation, &minimum,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(renderer->shader, renderer->warmthLocation, &warmth,
                   SHADER_UNIFORM_VEC3);
    SetShaderValue(renderer->shader, renderer->veilLocation, &veil,
                   SHADER_UNIFORM_VEC2);
    SetShaderValue(renderer->shader, renderer->veilAlphaLocation, &veilAlpha,
                   SHADER_UNIFORM_VEC2);
    SetShaderValue(renderer->shader, renderer->airAlphaLocation, &airAlpha,
                   SHADER_UNIFORM_FLOAT);
}

bool LightRendererInit(LightRenderer *renderer, const World *world)
{
    Image blank;

    if (renderer == NULL || world == NULL || world->lightColumns <= 0 ||
        world->lightRows <= 0) {
        return false;
    }
    memset(renderer, 0, sizeof(*renderer));
    renderer->columns = world->lightColumns;
    renderer->rows = world->lightRows;
    renderer->uploadedFirstColumn = 1;
    renderer->uploadedLastColumn = 0;
    renderer->uploadedFirstRow = 1;
    renderer->uploadedLastRow = 0;

    if (!LightRendererLoadShader(renderer)) {
        if (renderer->shader.id != 0u && IsShaderValid(renderer->shader)) {
            UnloadShader(renderer->shader);
        }
        renderer->shader = (Shader){0};
        TraceLog(LOG_WARNING,
                 "RENDER: World light shader unavailable; drawing the world unlit");
        return true;
    }

    renderer->staging = malloc((size_t)renderer->columns * (size_t)renderer->rows *
                               sizeof(*renderer->staging));
    blank = GenImageColor(renderer->columns, renderer->rows, BLACK);
    renderer->texture = LoadTextureFromImage(blank);
    UnloadImage(blank);
    if (renderer->staging == NULL || renderer->texture.id == 0u) {
        LightRendererUnload(renderer);
        memset(renderer, 0, sizeof(*renderer));
        TraceLog(LOG_WARNING,
                 "RENDER: World light texture unavailable; drawing the world unlit");
        return true;
    }
    /* Bilinear on purpose: it is the interpolation the CPU used to do for
       every cell, done by the sampler instead. Repeated across, because the
       world wraps: a page drawn one turn of the world along samples the same
       light as the page it is, and the seam blends like any other column.
       Clamped down, where the world does end. */
    SetTextureFilter(renderer->texture, TEXTURE_FILTER_BILINEAR);
    rlTextureParameters(renderer->texture.id, RL_TEXTURE_WRAP_S,
                        RL_TEXTURE_WRAP_REPEAT);
    rlTextureParameters(renderer->texture.id, RL_TEXTURE_WRAP_T,
                        RL_TEXTURE_WRAP_CLAMP);
    LightRendererSetConstants(renderer);
    renderer->ready = true;
    renderer->lastFrame.enabled = true;
    renderer->lastFrame.texels = (uint32_t)(renderer->columns * renderer->rows);
    return true;
}

static void LightRendererUpload(LightRenderer *renderer, const World *world,
                                int firstColumn, int lastColumn, int firstRow,
                                int lastRow)
{
    int width = lastColumn - firstColumn + 1;
    int height = lastRow - firstRow + 1;
    int row;

    for (row = 0; row < height; ++row) {
        const float *sky = world->lightSky +
                           (size_t)(firstRow + row) * (size_t)world->lightColumns +
                           (size_t)firstColumn;
        const float *ember = world->lightEmber +
                             (size_t)(firstRow + row) * (size_t)world->lightColumns +
                             (size_t)firstColumn;
        Color *out = renderer->staging + (size_t)row * (size_t)width;
        int column;

        for (column = 0; column < width; ++column) {
            float s = sky[column] < 0.0f ? 0.0f : (sky[column] > 1.0f ? 1.0f : sky[column]);
            float e = ember[column] < 0.0f ? 0.0f
                                           : (ember[column] > 1.0f ? 1.0f : ember[column]);

            out[column] = (Color){(unsigned char)(s * 255.0f + 0.5f),
                                  (unsigned char)(e * 255.0f + 0.5f), 0, 255};
        }
    }
    UpdateTextureRec(renderer->texture,
                     (Rectangle){(float)firstColumn, (float)firstRow, (float)width,
                                 (float)height},
                     renderer->staging);
    renderer->uploadedRevision = world->lightRevision;
    renderer->uploadedFirstColumn = firstColumn;
    renderer->uploadedLastColumn = lastColumn;
    renderer->uploadedFirstRow = firstRow;
    renderer->uploadedLastRow = lastRow;
    ++renderer->lastFrame.uploads;
    renderer->lastFrame.uploadedBytes +=
        (uint64_t)width * (uint64_t)height * sizeof(*renderer->staging);
}

void LightRendererSync(LightRenderer *renderer, World *world, Rectangle visible)
{
    double started;
    int firstColumn;
    int lastColumn;
    int firstRow;
    int lastRow;

    if (renderer == NULL || world == NULL) {
        return;
    }
    renderer->lastFrame.uploads = 0u;
    renderer->lastFrame.uploadedBytes = 0u;
    started = GetTime();
    WorldUpdateLighting(world, visible);
    if (!renderer->ready || world->lightColumns != renderer->columns ||
        world->lightRows != renderer->rows) {
        renderer->lastFrame.syncMilliseconds = (GetTime() - started) * 1000.0;
        return;
    }

    firstColumn = (int)floorf(visible.x / (float)WORLD_LIGHT_SCALE) -
                  LIGHT_UPLOAD_MARGIN;
    lastColumn = (int)floorf((visible.x + visible.width) / (float)WORLD_LIGHT_SCALE) +
                 LIGHT_UPLOAD_MARGIN;
    firstRow = (int)floorf(visible.y / (float)WORLD_LIGHT_SCALE) -
               LIGHT_UPLOAD_MARGIN;
    lastRow = (int)floorf((visible.y + visible.height) / (float)WORLD_LIGHT_SCALE) +
              LIGHT_UPLOAD_MARGIN;
    /* The world wraps and so does the texture: columns are kept unwrapped
       here, and a window across the seam is uploaded as the two pieces of
       the texture it covers. */
    if (lastColumn - firstColumn + 1 > renderer->columns) {
        firstColumn = 0;
        lastColumn = renderer->columns - 1;
    }
    if (firstRow < 0) firstRow = 0;
    if (lastRow > renderer->rows - 1) lastRow = renderer->rows - 1;
    if (firstColumn <= lastColumn && firstRow <= lastRow &&
        (renderer->uploadedRevision != world->lightRevision ||
         firstColumn < renderer->uploadedFirstColumn ||
         lastColumn > renderer->uploadedLastColumn ||
         firstRow < renderer->uploadedFirstRow ||
         lastRow > renderer->uploadedLastRow)) {
        int columns = renderer->columns;
        int start = ((firstColumn % columns) + columns) % columns;
        int width = lastColumn - firstColumn + 1;

        if (start + width <= columns) {
            LightRendererUpload(renderer, world, start, start + width - 1, firstRow,
                                lastRow);
        } else {
            LightRendererUpload(renderer, world, start, columns - 1, firstRow,
                                lastRow);
            LightRendererUpload(renderer, world, 0, start + width - 1 - columns,
                                firstRow, lastRow);
        }
        renderer->uploadedFirstColumn = firstColumn;
        renderer->uploadedLastColumn = lastColumn;
    }
    renderer->lastFrame.syncMilliseconds = (GetTime() - started) * 1000.0;
}

void LightRendererBegin(LightRenderer *renderer, const World *world,
                        LightPass pass)
{
    float daylight;
    int emissive;

    if (renderer == NULL || world == NULL || !renderer->ready) {
        return;
    }
    daylight = WorldShownDaylight(world);
    emissive = pass == LIGHT_PASS_EMISSIVE ? 1 : 0;
    SetShaderValue(renderer->shader, renderer->daylightLocation, &daylight,
                   SHADER_UNIFORM_FLOAT);
    SetShaderValue(renderer->shader, renderer->emissivePassLocation, &emissive,
                   SHADER_UNIFORM_INT);
    BeginShaderMode(renderer->shader);
    /* Bound per batch, not per shader: the batch forgets its extra samplers
       every time it is flushed, so this has to follow every flush. Begin is
       one. */
    SetShaderValueTexture(renderer->shader, renderer->lightMapLocation,
                          renderer->texture);
}

void LightRendererEnd(const LightRenderer *renderer)
{
    if (renderer == NULL || !renderer->ready) {
        return;
    }
    EndShaderMode();
}

void LightRendererUnload(LightRenderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    if (renderer->texture.id != 0u) {
        UnloadTexture(renderer->texture);
    }
    if (renderer->shader.id != 0u && IsShaderValid(renderer->shader)) {
        UnloadShader(renderer->shader);
    }
    free(renderer->staging);
    memset(renderer, 0, sizeof(*renderer));
}

const LightRendererStats *LightRendererStatistics(const LightRenderer *renderer)
{
    static const LightRendererStats empty = {0};

    return renderer != NULL ? &renderer->lastFrame : &empty;
}
