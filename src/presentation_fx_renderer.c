#include "presentation_fx_renderer.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

static float PresentationFxProgress(const PresentationFx *effect)
{
    return Clamp(effect->age / effect->description.lifetime, 0.0f, 1.0f);
}

static int PresentationFxRingSegments(float radius)
{
    int segments = (int)(radius * 1.5f);

    if (segments < 12) return 12;
    if (segments > 72) return 72;
    return segments;
}

static void PresentationFxDrawPrimitive(const PresentationFx *effect,
                                        Color color)
{
    const PresentationFxDescription *description = &effect->description;
    float progress = PresentationFxProgress(effect);
    float remaining = 1.0f - progress;
    float radius = Lerp(description->startRadius, description->endRadius,
                        progress);

    switch (description->type) {
    case PRESENTATION_FX_FLASH:
    case PRESENTATION_FX_GLOW:
        DrawCircleV(description->start, radius, color);
        break;
    case PRESENTATION_FX_PUFF: {
        /* Three overlapping circles keep smoke/dust readable as a clustered
           puff rather than a single perfect vector circle. */
        float lobe = radius * 0.58f;

        DrawCircleV(description->start, radius * 0.72f, color);
        DrawCircleV((Vector2){description->start.x - radius * 0.40f,
                              description->start.y + radius * 0.12f},
                    lobe, Fade(color, 0.72f));
        DrawCircleV((Vector2){description->start.x + radius * 0.36f,
                              description->start.y - radius * 0.16f},
                    lobe * 0.82f, Fade(color, 0.62f));
        break;
    }
    case PRESENTATION_FX_RING: {
        float width = description->width * (0.45f + remaining * 0.55f);
        float innerRadius = fmaxf(0.0f, radius - width * 0.5f);

        DrawRing(description->start, innerRadius, radius + width * 0.5f,
                 0.0f, 360.0f, PresentationFxRingSegments(radius), color);
        break;
    }
    case PRESENTATION_FX_LINE:
        DrawLineEx(description->start, description->end, description->width,
                   color);
        break;
    case PRESENTATION_FX_TRAIL:
        DrawLineEx(description->start, description->end,
                   description->width * (0.30f + remaining * 0.70f), color);
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
        PresentationFxDrawPrimitive(effect, color);
    }
}
