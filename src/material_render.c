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

/* Smooth value noise on a lattice `scale` cells apart, 0..1. The pattern
   layer only: what gives a material its structure — a clump, a band, a
   streak — is continuous across cells, and a hash per cell is only grain. */
static float MaterialLatticeValue(int x, int y, uint32_t salt)
{
    return (float)((MaterialCoordinateHash(x, y) ^ salt) * 0x9e3779b1u >> 8) /
           16777216.0f;
}

static float MaterialValueNoise(int x, int y, int scale, uint32_t salt)
{
    int cellX = (int)floorf((float)x / (float)scale);
    int cellY = (int)floorf((float)y / (float)scale);
    float fx = ((float)x - (float)cellX * (float)scale) / (float)scale;
    float fy = ((float)y - (float)cellY * (float)scale) / (float)scale;
    float a = MaterialLatticeValue(cellX, cellY, salt);
    float b = MaterialLatticeValue(cellX + 1, cellY, salt);
    float c = MaterialLatticeValue(cellX, cellY + 1, salt);
    float d = MaterialLatticeValue(cellX + 1, cellY + 1, salt);

    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

/* The cell's own grain as a tone in -1..1, from its shade. Stepped by a
   permutation so that neighbouring shade values are not neighbouring
   tones — the accent takes the lowest shades, and the tone must not. */
static float MaterialGrainTone(unsigned char shade)
{
    return (float)((shade * 37u + 11u) & 63u) / 31.5f - 1.0f;
}

/* Where the material's pattern puts this cell, -1 (dark) to 1 (light). */
static float MaterialPatternTone(MaterialPattern pattern, int x, int y,
                                 unsigned char shade, int liquidDepth)
{
    float grain = MaterialGrainTone(shade);

    switch (pattern) {
    case MATERIAL_PATTERN_CLUMP: {
        /* Clumps a few cells across, and grain over them. */
        float clump = MaterialValueNoise(x, y, 4, 0x51u) * 2.0f - 1.0f;

        return 0.7f * clump + 0.45f * grain;
    }
    case MATERIAL_PATTERN_STRATA: {
        /* Bands along the rows that wander up and down over tens of cells,
           each band its own tone, with fine grain inside it. */
        float wander = MaterialValueNoise(x, 0, 23, 0x7au) * 7.0f;
        int band = (int)floorf(((float)y + wander) / 3.0f);
        float bandTone = MaterialLatticeValue(band, 0, 0x3cu) * 2.0f - 1.0f;
        float grit = MaterialValueNoise(x, y, 2, 0x19u) * 2.0f - 1.0f;

        return 0.75f * bandTone + 0.2f * grit + 0.2f * grain;
    }
    case MATERIAL_PATTERN_FIBRE: {
        /* Streaks down the columns, drifting a little so the grain is not
           ruled with a pen. */
        int column = x + (int)floorf(MaterialValueNoise(x, y, 11, 0x2bu) * 2.0f);
        float streak = MaterialLatticeValue(column, 0, 0x6du) * 2.0f - 1.0f;

        return 0.7f * streak + 0.35f * grain;
    }
    case MATERIAL_PATTERN_CRYSTAL: {
        /* Glints along the diagonal, broken by noise. */
        float glint = MaterialValueNoise(x + y, x - y, 5, 0x44u) * 2.0f - 1.0f;

        return 0.6f * glint + 0.35f * grain;
    }
    case MATERIAL_PATTERN_FLUID: {
        /* Broad soft swirls, fainter the deeper, over a darkening with
           depth that the caller counted. */
        float swirl = MaterialValueNoise(x, y, 7, 0x0fu) * 2.0f - 1.0f;
        float depth = (float)liquidDepth / 10.0f;

        if (depth > 1.0f) depth = 1.0f;
        return 0.35f * swirl * (1.0f - depth) + 0.15f * grain - 0.9f * depth;
    }
    case MATERIAL_PATTERN_BRICK: {
        /* Courses two cells high over a row of mortar, bricks five long over
           a column of it, every other course moved half a brick along. */
        int course = (int)floorf((float)y / 3.0f);
        int row = y - course * 3;
        int shifted = x + ((course & 1) != 0 ? 3 : 0);
        int brick = (int)floorf((float)shifted / 6.0f);
        int column = shifted - brick * 6;
        float face;

        if (row == 2 || column == 5) {
            return -1.0f;
        }
        face = MaterialLatticeValue(brick, course, 0x5bu) * 2.0f - 1.0f;
        return 0.45f * face + 0.25f * grain + (row == 0 ? 0.15f : 0.0f);
    }
    case MATERIAL_PATTERN_PLATE: {
        /* Plates ten by seven, every other row of plates moved half along;
           a seam at the far edge of each, a rivet in from each corner. */
        int band = (int)floorf((float)y / 7.0f);
        int row = y - band * 7;
        int shifted = x + ((band & 1) != 0 ? 5 : 0);
        int plate = (int)floorf((float)shifted / 10.0f);
        int column = shifted - plate * 10;
        float face;

        if (row == 6 || column == 9) {
            return -1.0f;
        }
        if ((row == 1 || row == 4) && (column == 1 || column == 7)) {
            return 0.95f;
        }
        face = MaterialLatticeValue(plate, band, 0x2du) * 2.0f - 1.0f;
        return 0.35f * face + 0.25f * (MaterialValueNoise(x, y * 6, 5, 0x61u) * 2.0f - 1.0f) +
               (row == 0 ? 0.3f : 0.0f) + 0.1f * grain;
    }
    case MATERIAL_PATTERN_ASHLAR: {
        /* Blocks eight by five, every other course moved half a block, a
           joint one cell wide. Every fourth course is a frieze: a stepped
           key cut into it, the groove dark and its lip lit. */
        int course = (int)floorf((float)y / 5.0f);
        int row = y - course * 5;
        int shifted = x + ((course & 1) != 0 ? 4 : 0);
        int block = (int)floorf((float)shifted / 8.0f);
        int column = shifted - block * 8;
        float face;

        if (row == 4 || column == 7) {
            return -0.85f;
        }
        if ((course & 3) == 1 && row >= 1 && row <= 2) {
            /* The key: a groove that climbs and falls every four cells. */
            int step = ((x % 8) + 8) % 8;
            bool groove = row == 1 ? (step == 0 || step == 1 || step == 2 || step == 4)
                                   : (step == 2 || step == 4 || step == 5 || step == 6);

            if (groove) {
                return -0.7f;
            }
        }
        face = MaterialLatticeValue(block, course, 0x77u) * 2.0f - 1.0f;
        return 0.3f * face + 0.2f * grain + (row == 0 ? 0.25f : 0.0f);
    }
    case MATERIAL_PATTERN_BLADE:
        return (float)shade / 31.5f - 1.0f +
               0.2f * (MaterialValueNoise(x, y, 3, 0x2eu) * 2.0f - 1.0f);
    case MATERIAL_PATTERN_GRAIN:
    default:
        return grain;
    }
}

static Color MaterialMix(Color from, Color to, float amount)
{
    if (amount <= 0.0f) return from;
    if (amount > 1.0f) amount = 1.0f;
    return (Color){
        (unsigned char)((float)from.r + ((float)to.r - (float)from.r) * amount),
        (unsigned char)((float)from.g + ((float)to.g - (float)from.g) * amount),
        (unsigned char)((float)from.b + ((float)to.b - (float)from.b) * amount),
        (unsigned char)((float)from.a + ((float)to.a - (float)from.a) * amount),
    };
}

bool MaterialRenderOpenFace(CellMaterial material, CellMaterial neighbour)
{
    if (MaterialIsSolid(material)) {
        return !MaterialIsSolid(neighbour);
    }
    if (MaterialIsLiquid(material)) {
        return !MaterialIsLiquid(neighbour);
    }
    return neighbour != material;
}

static bool MaterialHasPalette(const MaterialInfo *info)
{
    return info->dark.a != 0u || info->light.a != 0u;
}

MaterialRenderSample MaterialRenderCell(CellMaterial material,
                                        float temperature,
                                        int patternX, int patternY,
                                        MaterialRenderContext context)
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
    if (MaterialHasPalette(info)) {
        float tone = MaterialPatternTone(info->pattern, patternX, patternY,
                                         context.shade, context.liquidDepth);

        /* A top face catches the light and an underside is in shadow: the
           edge of every ledge, grain and canopy reads as a shape rather
           than as a flat fill. A liquid's surface is lit harder still — it
           is the line the eye follows. */
        if (context.openAbove) {
            tone += info->pattern == MATERIAL_PATTERN_FLUID    ? 1.1f
                    : info->pattern == MATERIAL_PATTERN_GRAIN ? 0.25f
                                                              : 0.55f;
        }
        /* Loose grains have no ledges to shade: a heap is ragged at every
           row, and full-strength faces on it read as ruled stripes. */
        if (context.openBelow && info->pattern != MATERIAL_PATTERN_FLUID) {
            tone -= info->pattern == MATERIAL_PATTERN_GRAIN ? 0.15f : 0.45f;
        }
        color = tone < 0.0f ? MaterialMix(info->color, info->dark, -tone)
                            : MaterialMix(info->color, info->light, tone);
        /* The accent: a pebble, a vein, a knot, a spark. Carried by the
           shade, so a pebble in a falling pile stays a pebble. Never on a
           liquid's lit surface, which is the surface's own colour. */
        if (context.shade < info->accentShare &&
            !(context.openAbove && info->pattern == MATERIAL_PATTERN_FLUID)) {
            color = MaterialMix(color, info->accent, 0.85f);
        }
    }
    variation = (int)(MaterialCoordinateHash(patternX, patternY) % 13u) - 6;
    color.r = ChannelWithVariation(color.r, info->variationR, variation);
    color.g = ChannelWithVariation(color.g, info->variationG, variation);
    color.b = ChannelWithVariation(color.b, info->variationB, variation);
    color = MaterialHeatTint(color, info, temperature);
    color = MaterialFrostTint(color, info, temperature);
    sample.scene = color;

    /* Explicit emission, never brightness extraction: ordinary bright sand
       remains sharp while emissive materials and heated solids enter bloom.
       What does not glow is opaque black here, not transparent: the emissive
       plane has to occlude exactly where the scene plane does, or whatever
       glows behind a wall blooms through it. */
    strength = info->emission;
    heat = MaterialHeatAmount(info, temperature) * 0.72f;
    if (heat > strength) {
        strength = heat;
    }
    sample.emissive = (Color){0, 0, 0, 255};
    if (strength > 0.001f) {
        sample.emissive.r = (unsigned char)((float)color.r * strength);
        sample.emissive.g = (unsigned char)((float)color.g * strength);
        sample.emissive.b = (unsigned char)((float)color.b * strength);
    }
    return sample;
}

MaterialRenderSample MaterialRenderAir(int y, int height)
{
    MaterialRenderSample sample;
    /* Empty space is a depth gradient rather than a flat colour. */
    unsigned char glow = (unsigned char)(10 + (y * 10) / (height > 0 ? height : 1));

    sample.scene = (Color){5, glow, (unsigned char)(18 + glow),
                           MATERIAL_RENDER_AIR_ALPHA};
    /* Marked in both planes: sealed air has to hide a glow behind it exactly
       as it hides the backdrop, and the shader can only do that for a texel
       it can tell from "nothing here". */
    sample.emissive = (Color){0, 0, 0, MATERIAL_RENDER_AIR_ALPHA};
    return sample;
}

MaterialRenderSample MaterialRenderBackWall(CellMaterial wall, int x, int y)
{
    MaterialRenderContext context = {0};
    MaterialRenderSample sample;
    float grey;

    /* A tone per cell, as a cell of that wall would carry; the accent
       share is the same hash's. */
    context.shade = (unsigned char)(MaterialCoordinateHash(x * 7 + 3, y * 5 + 1) & 63u);
    sample = MaterialRenderCell(wall, AMBIENT_TEMPERATURE, x, y, context);
    grey = ((float)sample.scene.r + (float)sample.scene.g + (float)sample.scene.b) / 3.0f;
    sample.scene.r = (unsigned char)(((float)sample.scene.r * 0.8f + grey * 0.2f) * 0.42f);
    sample.scene.g = (unsigned char)(((float)sample.scene.g * 0.8f + grey * 0.2f) * 0.42f);
    sample.scene.b = (unsigned char)(((float)sample.scene.b * 0.8f + grey * 0.2f) * 0.46f);
    sample.scene.a = 255;
    sample.emissive = (Color){0, 0, 0, 255};
    return sample;
}

MaterialRenderSample MaterialRenderOverWall(MaterialRenderSample front,
                                            MaterialRenderSample wall)
{
    float amount = (float)front.scene.a / 255.0f;

    front.scene = MaterialMix(wall.scene, (Color){front.scene.r, front.scene.g,
                                                  front.scene.b, 255}, amount);
    front.scene.a = 255;
    if (front.emissive.a < 255) {
        front.emissive.a = 255;
    }
    return front;
}

float MaterialRenderSway(CellMaterial material, unsigned char shade)
{
    switch (material) {
    case MATERIAL_GRASS:
        /* The shade of a blade is its height along it: tips move. */
        return 0.1f + 0.9f * (float)shade / 63.0f;
    case MATERIAL_LEAF:
        return 0.75f;
    case MATERIAL_FUNGUS:
        return 0.25f;
    case MATERIAL_WOOD:
        return 0.06f;
    case MATERIAL_DRYBRUSH:
        return 0.45f;
    case MATERIAL_KELP:
        /* Swayed by the sea, which moves whatever the wind does. */
        return 1.0f;
    case MATERIAL_EMBERBLOOM:
        return 0.3f;
    default:
        return 0.0f;
    }
}
