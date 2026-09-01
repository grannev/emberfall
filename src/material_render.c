#include "material_render.h"

#include <math.h>
#include <stdint.h>

#include <raymath.h>

#include "materials.h"

static uint32_t MaterialCoordinateHash(int x, int y)
{
    uint32_t value = (uint32_t)x * 0x45d9f3bu;

    value ^= (uint32_t)y * 0x27d4eb2du;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return value;
}

static unsigned char ChannelWithVariation(unsigned char base, signed char spread,
                                          int variation)
{
    int value = (int)base + variation * (int)spread / 2;

    return (unsigned char)Clamp((float)value, 0.0f, 255.0f);
}

/* Solid cells glow toward ember as they approach their own phase threshold, so
   a laser-preheated rock keeps the same appearance after extraction. */
static float MaterialHeatAmount(const MaterialInfo *info, float temperature)
{
    if (temperature < 60.0f || !info->solid || !info->onHeat.enabled ||
        info->onHeat.threshold <= 60.0f) {
        return 0.0f;
    }

    return sqrtf(Clamp((temperature - 60.0f) /
                           (info->onHeat.threshold - 60.0f),
                       0.0f, 1.0f));
}

/* The other half of the same idea: a cell driven well below ambient reads as
   frosted. Without it the cryo beam is invisible on anything that is not water
   — it really is chilling the rock, and the player simply cannot tell. */
static float MaterialChillAmount(const MaterialInfo *info, float temperature)
{
    if (temperature > 0.0f || !info->solid) {
        return 0.0f;
    }
    /* Full frost by −120, which a held beam reaches in about half a second. */
    return sqrtf(Clamp(-temperature / 120.0f, 0.0f, 1.0f));
}

static Color MaterialFrostTint(Color base, const MaterialInfo *info,
                               float temperature)
{
    float chill = MaterialChillAmount(info, temperature);

    if (chill <= 0.0f) {
        return base;
    }
    /* Toward pale blue-white, and never toward glowing: cold is a colour, not a
       light source, so nothing here touches the emissive plane. */
    base.r = (unsigned char)((float)base.r +
                             (196.0f - (float)base.r) * chill * 0.72f);
    base.g = (unsigned char)((float)base.g +
                             (226.0f - (float)base.g) * chill * 0.80f);
    base.b = (unsigned char)((float)base.b + (255.0f - (float)base.b) * chill);
    return base;
}

static Color MaterialHeatTint(Color base, const MaterialInfo *info,
                              float temperature)
{
    float heat = MaterialHeatAmount(info, temperature);

    base.r = (unsigned char)((float)base.r + (245.0f - (float)base.r) * heat);
    base.g = (unsigned char)((float)base.g +
                             (96.0f - (float)base.g) * heat * 0.8f);
    base.b = (unsigned char)((float)base.b * (1.0f - heat * 0.75f));
    return base;
}

MaterialRenderSample MaterialRenderCell(CellMaterial material,
                                        float temperature,
                                        int variationX, int variationY,
                                        float red, float green, float blue)
{
    const MaterialInfo *info = MaterialAt(material);
    MaterialRenderSample sample = {BLANK, BLANK};
    Color color;
    float strength;
    float heat;
    int variation;

    if (material <= MATERIAL_EMPTY || material >= MATERIAL_COUNT) {
        return sample;
    }

    color = info->color;
    variation = (int)(MaterialCoordinateHash(variationX, variationY) % 13u) - 6;
    color.r = ChannelWithVariation(color.r, info->variationR, variation);
    color.g = ChannelWithVariation(color.g, info->variationG, variation);
    color.b = ChannelWithVariation(color.b, info->variationB, variation);
    color = MaterialHeatTint(color, info, temperature);
    color = MaterialFrostTint(color, info, temperature);

    /* An emitter lights itself. This retains the old world-page behaviour and
       prevents lava from becoming darker in its own emissive centre. */
    if (info->emission < 0.999f) {
        float channel = red * (float)color.r;

        color.r = (unsigned char)(channel > 255.0f ? 255.0f : channel);
        channel = green * (float)color.g;
        color.g = (unsigned char)(channel > 255.0f ? 255.0f : channel);
        channel = blue * (float)color.b;
        color.b = (unsigned char)(channel > 255.0f ? 255.0f : channel);
    }
    sample.scene = color;

    /* Explicit emission, never brightness extraction: ordinary bright sand
       remains sharp while emissive materials and heated solids enter bloom. */
    strength = info->emission;
    heat = MaterialHeatAmount(info, temperature) * 0.72f;
    if (heat > strength) {
        strength = heat;
    }
    if (strength > 0.001f) {
        sample.emissive = (Color){
            (unsigned char)((float)color.r * strength),
            (unsigned char)((float)color.g * strength),
            (unsigned char)((float)color.b * strength),
            255u,
        };
    }
    return sample;
}
