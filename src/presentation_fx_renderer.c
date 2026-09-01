#include "presentation_fx_renderer.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

static float PresentationFxProgress(const PresentationFx *effect)
{
    return Clamp(effect->age / effect->description.lifetime, 0.0f, 1.0f);
}

/* --- pixel primitives ----------------------------------------------------

   The world is squares. An effect drawn with DrawCircle is a perfect vector
   curve laid over a raster field, and it reads as belonging to a different
   game — which is exactly what it looked like. Everything here is built from
   blocks snapped to a grid, with edges broken up by a hash so a rim is ragged
   rather than machined.

   The soft light around an effect is not drawn here at all: it comes from the
   emissive pass and the bloom behind it. These primitives are the hard,
   chunky part that sits inside that glow. */

/* Deterministic per-block noise. Per-block rather than per-frame, so a ragged
   edge is ragged the same way for the whole life of the effect instead of
   boiling. */
static float FxNoise(int x, int y, int salt)
{
    unsigned int h = (unsigned int)x * 374761393u ^
                     (unsigned int)y * 668265263u ^
                     (unsigned int)salt * 2246822519u;

    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

/* One block, snapped to the block grid so neighbouring blocks tile exactly
   instead of overlapping by fractions of a cell. */
static void FxBlock(float x, float y, float block, Color color)
{
    float snappedX = floorf(x / block) * block;
    float snappedY = floorf(y / block) * block;

    DrawRectangleV((Vector2){snappedX, snappedY}, (Vector2){block, block},
                   color);
}

/* A filled disc of blocks with a broken rim. */
static void FxPixelDisc(Vector2 centre, float radius, float block, Color color,
                        int salt)
{
    float step = block;
    float y;

    if (radius <= 0.0f || block <= 0.0f) {
        return;
    }
    for (y = -radius; y <= radius; y += step) {
        float x;

        for (x = -radius; x <= radius; x += step) {
            float distance = sqrtf(x * x + y * y);
            float edge;

            if (distance > radius) {
                continue;
            }
            /* Blocks near the rim drop out at random, which is what turns a
               circle into a shape that belongs in this world. */
            edge = distance / radius;
            if (edge > 0.62f &&
                FxNoise((int)floorf((centre.x + x) / block),
                        (int)floorf((centre.y + y) / block), salt) <
                    (edge - 0.62f) / 0.38f) {
                continue;
            }
            FxBlock(centre.x + x, centre.y + y, block, color);
        }
    }
}

/* A ring of separate blocks rather than a continuous band: in the reference the
   shock fronts read as a scatter of embers on a circle, not as a drawn curve. */
static void FxPixelRing(Vector2 centre, float radius, float block, Color color,
                        int salt)
{
    int count;
    int index;

    if (radius <= 0.0f || block <= 0.0f) {
        return;
    }
    count = (int)(radius * 6.283185f / (block * 1.35f));
    if (count < 8) count = 8;
    if (count > 220) count = 220;
    for (index = 0; index < count; ++index) {
        float angle = (float)index / (float)count * 6.283185f;
        float wobble = 1.0f + (FxNoise(index, salt, 7) - 0.5f) * 0.18f;
        float x = centre.x + cosf(angle) * radius * wobble;
        float y = centre.y + sinf(angle) * radius * wobble;

        /* Gaps. A complete ring is a curve again however it is drawn. */
        if (FxNoise(index, salt, 11) < 0.28f) {
            continue;
        }
        FxBlock(x, y, block, color);
    }
}

/* A chunky segmented line: blocks along the span with their thickness broken
   up, for beams, trails and the arcs of a shock front. */
static void FxPixelLine(Vector2 from, Vector2 to, float width, float block,
                        Color color, int salt)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float length = sqrtf(dx * dx + dy * dy);
    float travelled;
    int index = 0;

    if (length < 0.0001f || block <= 0.0f) {
        return;
    }
    dx /= length;
    dy /= length;
    for (travelled = 0.0f; travelled <= length; travelled += block, ++index) {
        float across;
        float half = width * 0.5f;

        for (across = -half; across <= half; across += block) {
            float x = from.x + dx * travelled - dy * across;
            float y = from.y + dy * travelled + dx * across;

            if (half > block && FxNoise(index, (int)(across / block), salt) <
                                    0.22f) {
                continue;
            }
            FxBlock(x, y, block, color);
        }
    }
}

/* Block size for an effect of this size. Big events get chunkier blocks, which
   is what keeps a large flash from turning back into a smooth shape simply
   because its blocks are small relative to it. */
static float FxBlockSize(float radius)
{
    float block = radius * 0.16f;

    if (block < 1.0f) return 1.0f;
    if (block > 4.0f) return 4.0f;
    return block;
}

static void PresentationFxDrawPrimitive(const PresentationFx *effect,
                                        Color color)
{
    const PresentationFxDescription *description = &effect->description;
    float progress = PresentationFxProgress(effect);
    float remaining = 1.0f - progress;
    float radius = Lerp(description->startRadius, description->endRadius,
                        progress);

    int salt = (int)(description->start.x * 7.0f + description->start.y * 13.0f);
    float block = FxBlockSize(radius);

    switch (description->type) {
    case PRESENTATION_FX_FLASH:
    case PRESENTATION_FX_GLOW:
        FxPixelDisc(description->start, radius, block, color, salt);
        break;
    case PRESENTATION_FX_PUFF: {
        /* Three overlapping lobes, so smoke reads as a clustered puff rather
           than one shape. */
        float lobe = radius * 0.58f;

        FxPixelDisc(description->start, radius * 0.72f, block, color, salt);
        FxPixelDisc((Vector2){description->start.x - radius * 0.40f,
                              description->start.y + radius * 0.12f},
                    lobe, block, Fade(color, 0.72f), salt + 1);
        FxPixelDisc((Vector2){description->start.x + radius * 0.36f,
                              description->start.y - radius * 0.16f},
                    lobe * 0.82f, block, Fade(color, 0.62f), salt + 2);
        break;
    }
    case PRESENTATION_FX_RING:
        FxPixelRing(description->start, radius, block, color, salt);
        break;
    case PRESENTATION_FX_LINE:
        FxPixelLine(description->start, description->end, description->width,
                    fminf(block, 1.5f), color, salt);
        break;
    case PRESENTATION_FX_TRAIL:
        FxPixelLine(description->start, description->end,
                    description->width * (0.30f + remaining * 0.70f),
                    fminf(block, 1.5f), color, salt);
        break;
    default:
        break;
    }
}

void PresentationFxRendererDrawScene(const PresentationFxSystem *system)
{
    uint16_t index;

    if (system == NULL) {
        return;
    }
    for (index = 0u; index < system->stats.active; ++index) {
        const PresentationFx *effect = &system->effects[index];

        if (effect->age < 0.0f) {
            continue;
        }
        float remaining = 1.0f - PresentationFxProgress(effect);
        float opacity = Clamp(effect->description.intensity * remaining,
                              0.0f, 1.0f);

        if (effect->description.type == PRESENTATION_FX_FLASH) {
            opacity *= remaining;
        }
        PresentationFxDrawPrimitive(
            effect, Fade(effect->description.color, opacity));
    }
}

void PresentationFxRendererDrawEmissive(const PresentationFxSystem *system)
{
    uint16_t index;

    if (system == NULL) {
        return;
    }
    for (index = 0u; index < system->stats.active; ++index) {
        const PresentationFx *effect = &system->effects[index];
        float alpha;
        float strength;
        Color color;

        if (effect->age < 0.0f || !effect->description.emissive) {
            continue;
        }
        alpha = (float)effect->description.color.a / 255.0f;
        strength = Clamp(effect->description.intensity *
                             (1.0f - PresentationFxProgress(effect)) * alpha,
                         0.0f, 1.0f);
        color = (Color){
            (unsigned char)((float)effect->description.color.r * strength),
            (unsigned char)((float)effect->description.color.g * strength),
            (unsigned char)((float)effect->description.color.b * strength),
            255u,
        };
        /* A wide, dim halo under the hard shape. What carries an event in the
           reference is not its outline but the light around it — a large soft
           bloom with a few chunky embers inside. The bloom pass is what makes
           it soft; all that is needed here is something broad and faint for it
           to spread, and it only goes into the emissive plane, so the scene
           colours underneath stay exactly as they were.

           Deliberately coarse blocks: the blur is about to smear them anyway,
           and a fine one costs many times the fill for a result nothing can
           tell apart. */
        {
            const PresentationFxDescription *description = &effect->description;
            float radius = Lerp(description->startRadius, description->endRadius,
                                PresentationFxProgress(effect));
            float halo = radius * 2.6f;
            Color glow = {(unsigned char)((float)color.r * 0.42f),
                          (unsigned char)((float)color.g * 0.42f),
                          (unsigned char)((float)color.b * 0.42f), 255u};

            if (halo > 1.0f && description->type != PRESENTATION_FX_LINE &&
                description->type != PRESENTATION_FX_TRAIL) {
                FxPixelDisc(description->start, halo, fmaxf(halo * 0.22f, 2.0f),
                            glow, 0);
            }
        }
        PresentationFxDrawPrimitive(effect, color);
    }
}
