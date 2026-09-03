#include "environment_renderer.h"

#include "beam_render.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Three deliberately restrained identities. Their silhouettes are kept below
   foreground material contrast; accent is the only colour submitted to the
   emissive pass. */
static const EnvironmentPaletteDefinition PALETTES[ENVIRONMENT_PALETTE_COUNT] = {
    [ENVIRONMENT_PALETTE_EMBER_WASTE] = {
        .name = "EMBER WASTE",
        .cliName = "ember",
        .skyTop = {12, 14, 28, 255},
        .skyBottom = {112, 57, 42, 255},
        .horizon = {188, 88, 45, 255},
        .farSilhouette = {66, 45, 46, 255},
        .midSilhouette = {39, 30, 36, 255},
        .nearSilhouette = {18, 18, 26, 255},
        .haze = {151, 91, 63, 255},
        .accent = {255, 113, 42, 255},
        .profile = {1.00f, 0.86f, 1.18f, 0.75f, 0.00f, 0.85f},
    },
    /* The sea's horizon, and now the ocean biome's own backdrop. It used to
       carry eight towers and a real ridge, which is the one silhouette a sea
       cannot have: what stands at the far edge of open water is a flat line
       with haze over it. Wide, low and unbroken, so that arriving at the coast
       reads as the land running out. */
    [ENVIRONMENT_PALETTE_ABYSSAL_BLUE] = {
        .name = "ABYSSAL BLUE",
        .cliName = "abyss",
        .skyTop = {4, 13, 34, 255},
        .skyBottom = {22, 76, 101, 255},
        .horizon = {47, 145, 171, 255},
        .farSilhouette = {22, 57, 75, 255},
        .midSilhouette = {12, 35, 52, 255},
        .nearSilhouette = {5, 20, 35, 255},
        .haze = {55, 129, 151, 255},
        .accent = {80, 216, 243, 255},
        .profile = {0.05f, 1.62f, 0.24f, 0.00f, 0.00f, 0.45f},
    },
    [ENVIRONMENT_PALETTE_VERDIGRIS_STORM] = {
        .name = "VERDIGRIS STORM",
        .cliName = "storm",
        .skyTop = {11, 21, 24, 255},
        .skyBottom = {73, 90, 59, 255},
        .horizon = {132, 145, 78, 255},
        .farSilhouette = {49, 64, 53, 255},
        .midSilhouette = {28, 44, 39, 255},
        .nearSilhouette = {11, 27, 28, 255},
        .haze = {100, 120, 85, 255},
        .accent = {188, 225, 103, 255},
        .profile = {0.42f, 1.20f, 0.70f, 0.25f, 1.00f, 0.15f},
    },
    [ENVIRONMENT_PALETTE_AMBER_DUNES] = {
        .name = "AMBER DUNES",
        .cliName = "dunes",
        .skyTop = {18, 20, 40, 255},
        .skyBottom = {126, 96, 54, 255},
        .horizon = {206, 158, 82, 255},
        .farSilhouette = {84, 66, 46, 255},
        .midSilhouette = {52, 41, 32, 255},
        .nearSilhouette = {22, 20, 22, 255},
        .haze = {172, 138, 88, 255},
        .accent = {255, 196, 96, 255},
        .profile = {0.08f, 1.45f, 0.55f, 0.00f, 0.00f, 0.35f},
    },
    [ENVIRONMENT_PALETTE_GLACIER_SHELF] = {
        .name = "GLACIER SHELF",
        .cliName = "glacier",
        .skyTop = {10, 18, 38, 255},
        .skyBottom = {70, 106, 134, 255},
        .horizon = {148, 186, 208, 255},
        .farSilhouette = {58, 78, 96, 255},
        .midSilhouette = {34, 50, 66, 255},
        .nearSilhouette = {14, 24, 36, 255},
        .haze = {126, 160, 184, 255},
        .accent = {186, 233, 255, 255},
        .profile = {0.82f, 0.95f, 1.05f, 0.10f, 0.45f, 0.55f},
    },
};

static float EnvironmentClamp(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

/* Seconds a backdrop takes to cross to another. Slow enough that the change is
   something the player notices having happened rather than something they see
   happen, which is how a horizon behaves. */
#define ENVIRONMENT_FADE_SECONDS 2.5f

static unsigned char EnvironmentMixChannel(unsigned char from, unsigned char to,
                                           float amount, float night)
{
    float mixed = (float)from + ((float)to - (float)from) * amount;

    /* Night is a scale toward black with a little blue left in it, rather than
       a separate set of colours: the same horizon at midnight has to read as
       the same place, only unlit. */
    return (unsigned char)EnvironmentClamp(mixed * night, 0.0f, 255.0f);
}

static Color EnvironmentMixColor(Color from, Color to, float amount,
                                 float daylight)
{
    float lit = 0.14f + 0.86f * daylight;
    Color mixed;

    mixed.r = EnvironmentMixChannel(from.r, to.r, amount, lit);
    mixed.g = EnvironmentMixChannel(from.g, to.g, amount, lit * 1.02f);
    mixed.b = EnvironmentMixChannel(from.b, to.b, amount,
                                    lit + (1.0f - daylight) * 0.10f);
    mixed.a = (unsigned char)((float)from.a +
                              ((float)to.a - (float)from.a) * amount);
    return mixed;
}

EnvironmentPaletteDefinition EnvironmentRendererResolvedPalette(
    const EnvironmentRenderer *renderer)
{
    const EnvironmentPaletteDefinition *from;
    const EnvironmentPaletteDefinition *to;
    EnvironmentPaletteDefinition resolved;
    float amount;
    float daylight;

    to = EnvironmentPaletteDefinitionAt(renderer != NULL ? renderer->palette
                                                         : ENVIRONMENT_PALETTE_AUTO);
    if (to == NULL) {
        to = &PALETTES[0];
    }
    from = renderer != NULL ? EnvironmentPaletteDefinitionAt(renderer->fadeFrom)
                            : NULL;
    if (from == NULL) {
        from = to;
    }
    amount = renderer != NULL ? EnvironmentClamp(renderer->fade, 0.0f, 1.0f)
                              : 1.0f;
    daylight = renderer != NULL ? EnvironmentClamp(renderer->daylight, 0.0f, 1.0f)
                                : 1.0f;

    resolved = *to;
    resolved.skyTop = EnvironmentMixColor(from->skyTop, to->skyTop, amount, daylight);
    resolved.skyBottom =
        EnvironmentMixColor(from->skyBottom, to->skyBottom, amount, daylight);
    resolved.horizon =
        EnvironmentMixColor(from->horizon, to->horizon, amount, daylight);
    resolved.farSilhouette =
        EnvironmentMixColor(from->farSilhouette, to->farSilhouette, amount, daylight);
    resolved.midSilhouette =
        EnvironmentMixColor(from->midSilhouette, to->midSilhouette, amount, daylight);
    resolved.nearSilhouette =
        EnvironmentMixColor(from->nearSilhouette, to->nearSilhouette, amount,
                            daylight);
    resolved.haze = EnvironmentMixColor(from->haze, to->haze, amount, daylight);
    /* The accent keeps its brightness through the night. It is what the stars,
       the far lights and the emissive pass are made of, and a night sky with no
       points of light in it is a black rectangle. */
    resolved.accent = EnvironmentMixColor(from->accent, to->accent, amount,
                                          0.55f + 0.45f * daylight);
    return resolved;
}

EnvironmentProfile EnvironmentRendererResolvedProfile(
    const EnvironmentRenderer *renderer)
{
    const EnvironmentPaletteDefinition *from;
    const EnvironmentPaletteDefinition *to;
    EnvironmentProfile blended;
    float amount;

    to = EnvironmentPaletteDefinitionAt(renderer != NULL ? renderer->palette
                                                         : ENVIRONMENT_PALETTE_AUTO);
    if (to == NULL) to = &PALETTES[0];
    from = renderer != NULL ? EnvironmentPaletteDefinitionAt(renderer->fadeFrom)
                            : NULL;
    if (from == NULL) from = to;
    amount = renderer != NULL ? EnvironmentClamp(renderer->fade, 0.0f, 1.0f)
                              : 1.0f;

    blended.sharpness = from->profile.sharpness +
                        (to->profile.sharpness - from->profile.sharpness) *
                            amount;
    blended.breadth = from->profile.breadth +
                      (to->profile.breadth - from->profile.breadth) * amount;
    blended.relief = from->profile.relief +
                     (to->profile.relief - from->profile.relief) * amount;
    blended.towers = from->profile.towers +
                     (to->profile.towers - from->profile.towers) * amount;
    blended.treeline = from->profile.treeline +
                       (to->profile.treeline - from->profile.treeline) * amount;
    blended.plume = from->profile.plume +
                    (to->profile.plume - from->profile.plume) * amount;
    return blended;
}

void EnvironmentRendererFadeTo(EnvironmentRenderer *renderer,
                               EnvironmentPalette palette)
{
    if (renderer == NULL || palette < 0 ||
        palette >= ENVIRONMENT_PALETTE_COUNT || renderer->palette == palette) {
        return;
    }
    /* A palette named on the command line is a decision about the whole
       session and outranks the ground the player happens to be standing on. */
    if (renderer->forcedPalette != ENVIRONMENT_PALETTE_AUTO) {
        return;
    }
    /* Starting from where the crossing has got to, not from the palette it
       began at: crossing a boundary back and forth must never snap. */
    renderer->fadeFrom = renderer->fade >= 1.0f ? renderer->palette
                                                : renderer->fadeFrom;
    renderer->palette = palette;
    renderer->fade = 0.0f;
    renderer->stats.palette = palette;
}

void EnvironmentRendererSetDaylight(EnvironmentRenderer *renderer,
                                    float daylight)
{
    if (renderer == NULL) {
        return;
    }
    renderer->daylight = EnvironmentClamp(daylight, 0.0f, 1.0f);
}

void EnvironmentRendererSetAltitude(EnvironmentRenderer *renderer,
                                    float altitude)
{
    if (renderer == NULL) {
        return;
    }
    renderer->altitude = EnvironmentClamp(altitude, 0.0f, 1.0f);
}

static int EnvironmentMaxInt(int first, int second)
{
    return first > second ? first : second;
}

static uint64_t EnvironmentMix(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

static float EnvironmentUnit(uint64_t seed, uint64_t index)
{
    uint64_t bits = EnvironmentMix(seed ^ (index * 0xd1b54a32d192ed03ull));

    return (float)((bits >> 40u) & 0xffffffu) / 16777215.0f;
}

static Color EnvironmentFade(Color color, float alpha)
{
    color.a = (unsigned char)(EnvironmentClamp(alpha, 0.0f, 1.0f) * 255.0f);
    return color;
}

static Color EnvironmentToward(Color color, Color target, float amount)
{
    amount = EnvironmentClamp(amount, 0.0f, 1.0f);
    color.r = (unsigned char)((float)color.r +
                              ((float)target.r - (float)color.r) * amount);
    color.g = (unsigned char)((float)color.g +
                              ((float)target.g - (float)color.g) * amount);
    color.b = (unsigned char)((float)color.b +
                              ((float)target.b - (float)color.b) * amount);
    return color;
}

static float EnvironmentWrap(float value, float period)
{
    float wrapped;

    if (!isfinite(value) || !isfinite(period) || period <= 0.0f) {
        return 0.0f;
    }
    wrapped = fmodf(value, period);
    return wrapped < 0.0f ? wrapped + period : wrapped;
}

static float EnvironmentFeatureX(const EnvironmentFeature *feature,
                                 const EnvironmentRenderer *renderer,
                                 Camera2D camera, int width, float parallax,
                                 float drift)
{
    float margin = (float)width * 0.12f + 48.0f;
    float period = (float)width + margin * 2.0f;
    float shift = camera.target.x * camera.zoom * parallax;
    float animated = renderer->time * drift + feature->phase * 13.0f;

    return EnvironmentWrap(feature->x * period - shift + animated, period) -
           margin;
}

static float EnvironmentViewScale(Camera2D camera, int width)
{
    float scale = camera.zoom * 320.0f / (float)width;

    return EnvironmentClamp(scale, 0.42f, 1.20f);
}

static float EnvironmentHorizon(Camera2D camera, int height, float parallax,
                                float base)
{
    float travel = (camera.target.y - 210.0f) * camera.zoom * parallax;
    float limitUp = (float)height * 0.18f;
    float limitDown = (float)height * 0.22f;
    float horizon;

    travel = EnvironmentClamp(travel, -limitUp, limitDown);
    horizon = (float)height * base - travel;
    /* A camera at the vertical world extremes must not produce negative
       rectangle heights or move every silhouette entirely off target. */
    return EnvironmentClamp(horizon, (float)height * 0.08f,
                            (float)height * 0.96f);
}

static void EnvironmentGenerateFeatures(EnvironmentRenderer *renderer,
                                        uint64_t seed)
{
    int index;

    renderer->seed = seed;
    renderer->time = 0.0f;
    for (index = 0; index < ENVIRONMENT_FAR_PEAK_COUNT; ++index) {
        float slot = ((float)index + 0.25f +
                      EnvironmentUnit(seed, (uint64_t)index + 1u) * 0.5f) /
                     (float)ENVIRONMENT_FAR_PEAK_COUNT;

        renderer->farPeaks[index] = (EnvironmentFeature){
            .x = slot,
            .y = EnvironmentUnit(seed, (uint64_t)index + 31u),
            .width = 0.55f + EnvironmentUnit(seed, (uint64_t)index + 61u),
            .height = 0.35f + EnvironmentUnit(seed, (uint64_t)index + 91u),
            .phase = EnvironmentUnit(seed, (uint64_t)index + 121u) * 6.2831853f,
        };
    }
    for (index = 0; index < ENVIRONMENT_STRUCTURE_COUNT; ++index) {
        float slot = ((float)index + 0.15f +
                      EnvironmentUnit(seed, (uint64_t)index + 211u) * 0.7f) /
                     (float)ENVIRONMENT_STRUCTURE_COUNT;

        renderer->structures[index] = (EnvironmentFeature){
            .x = slot,
            .y = EnvironmentUnit(seed, (uint64_t)index + 241u),
            .width = 0.35f + EnvironmentUnit(seed, (uint64_t)index + 271u),
            .height = 0.30f + EnvironmentUnit(seed, (uint64_t)index + 301u),
            .phase = EnvironmentUnit(seed, (uint64_t)index + 331u) * 6.2831853f,
        };
    }
    for (index = 0; index < ENVIRONMENT_HAZE_BAND_COUNT; ++index) {
        renderer->hazeBands[index] = (EnvironmentFeature){
            .x = EnvironmentUnit(seed, (uint64_t)index + 401u),
            .y = 0.12f + EnvironmentUnit(seed, (uint64_t)index + 431u) * 0.72f,
            .width = 0.38f + EnvironmentUnit(seed, (uint64_t)index + 461u) * 0.42f,
            .height = 0.018f + EnvironmentUnit(seed, (uint64_t)index + 491u) * 0.035f,
            .phase = EnvironmentUnit(seed, (uint64_t)index + 521u) * 6.2831853f,
        };
    }
    for (index = 0; index < ENVIRONMENT_SKY_DETAIL_COUNT; ++index) {
        renderer->skyDetails[index] = (EnvironmentFeature){
            .x = EnvironmentUnit(seed, (uint64_t)index + 601u),
            .y = 0.06f + EnvironmentUnit(seed, (uint64_t)index + 631u) * 0.48f,
            .width = 1.0f + EnvironmentUnit(seed, (uint64_t)index + 661u),
            .height = 1.0f + EnvironmentUnit(seed, (uint64_t)index + 691u),
            .phase = EnvironmentUnit(seed, (uint64_t)index + 721u) * 6.2831853f,
        };
    }
    for (index = 0; index < ENVIRONMENT_NEAR_SPIRE_COUNT; ++index) {
        renderer->nearSpires[index] = (EnvironmentFeature){
            .x = ((float)index + EnvironmentUnit(seed, (uint64_t)index + 801u)) /
                 (float)ENVIRONMENT_NEAR_SPIRE_COUNT,
            .y = EnvironmentUnit(seed, (uint64_t)index + 831u),
            .width = 0.35f + EnvironmentUnit(seed, (uint64_t)index + 861u),
            .height = 0.25f + EnvironmentUnit(seed, (uint64_t)index + 891u),
            .phase = EnvironmentUnit(seed, (uint64_t)index + 921u) * 6.2831853f,
        };
    }
}

bool EnvironmentPalettesValidate(void)
{
    int index;

    for (index = 0; index < ENVIRONMENT_PALETTE_COUNT; ++index) {
        const EnvironmentPaletteDefinition *palette = &PALETTES[index];

        if (palette->name == NULL || palette->name[0] == '\0' ||
            palette->cliName == NULL || palette->cliName[0] == '\0' ||
            palette->skyTop.a != 255u || palette->skyBottom.a != 255u ||
            palette->accent.a != 255u) {
            return false;
        }
    }
    return true;
}

bool EnvironmentPaletteParse(const char *text, EnvironmentPalette *palette)
{
    int index;

    if (text == NULL || palette == NULL) {
        return false;
    }
    if (strcmp(text, "auto") == 0) {
        *palette = ENVIRONMENT_PALETTE_AUTO;
        return true;
    }
    for (index = 0; index < ENVIRONMENT_PALETTE_COUNT; ++index) {
        if (strcmp(text, PALETTES[index].cliName) == 0) {
            *palette = (EnvironmentPalette)index;
            return true;
        }
    }
    return false;
}

const EnvironmentPaletteDefinition *EnvironmentPaletteDefinitionAt(
    EnvironmentPalette palette)
{
    if (palette < 0 || palette >= ENVIRONMENT_PALETTE_COUNT) {
        return NULL;
    }
    return &PALETTES[palette];
}

EnvironmentPalette EnvironmentPaletteForSeed(uint64_t seed)
{
    return (EnvironmentPalette)(EnvironmentMix(seed ^ 0x454e5649524f4eull) %
                                (uint64_t)ENVIRONMENT_PALETTE_COUNT);
}

void EnvironmentRendererInit(EnvironmentRenderer *renderer, uint64_t seed,
                             EnvironmentPalette forcedPalette)
{
    if (renderer == NULL) {
        return;
    }
    *renderer = (EnvironmentRenderer){0};
    renderer->forcedPalette =
        forcedPalette >= 0 && forcedPalette < ENVIRONMENT_PALETTE_COUNT
            ? forcedPalette
            : ENVIRONMENT_PALETTE_AUTO;
    renderer->palette = renderer->forcedPalette == ENVIRONMENT_PALETTE_AUTO
                            ? EnvironmentPaletteForSeed(seed)
                            : renderer->forcedPalette;
    EnvironmentGenerateFeatures(renderer, seed);
    renderer->stats.palette = renderer->palette;
    renderer->fadeFrom = renderer->palette;
    renderer->fade = 1.0f;
    renderer->daylight = 1.0f;
    renderer->altitude = 1.0f;
    renderer->stats.viewValid = true;
}

void EnvironmentRendererSyncSeed(EnvironmentRenderer *renderer, uint64_t seed)
{
    if (renderer == NULL || renderer->seed == seed) {
        return;
    }
    EnvironmentGenerateFeatures(renderer, seed);
    if (renderer->forcedPalette == ENVIRONMENT_PALETTE_AUTO) {
        renderer->palette = EnvironmentPaletteForSeed(seed);
    }
    renderer->stats.palette = renderer->palette;
}

bool EnvironmentRendererSetPalette(EnvironmentRenderer *renderer,
                                   EnvironmentPalette palette)
{
    if (renderer == NULL || palette < ENVIRONMENT_PALETTE_AUTO ||
        palette >= ENVIRONMENT_PALETTE_COUNT) {
        return false;
    }
    renderer->forcedPalette = palette;
    renderer->palette = palette == ENVIRONMENT_PALETTE_AUTO
                            ? EnvironmentPaletteForSeed(renderer->seed)
                            : palette;
    /* Immediate, and that means finishing whatever crossing was in flight.
       Leaving the blend alone looked harmless and was not: a forced palette set
       while the backdrop was six per cent of the way out of the previous one
       drew ninety-four per cent of the previous one, so every forced palette
       painted the same sky and the option appeared to do nothing. */
    renderer->fadeFrom = renderer->palette;
    renderer->fade = 1.0f;
    renderer->stats.palette = renderer->palette;
    return true;
}

void EnvironmentRendererUpdate(EnvironmentRenderer *renderer, float deltaTime)
{
    if (renderer == NULL || !isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }
    renderer->time = fmodf(renderer->time + fminf(deltaTime, 0.1f), 4096.0f);
    if (renderer->fade < 1.0f) {
        renderer->fade += fminf(deltaTime, 0.1f) / ENVIRONMENT_FADE_SECONDS;
        if (renderer->fade > 1.0f) renderer->fade = 1.0f;
    }
}

Rectangle EnvironmentRendererOverscanBounds(int width, int height)
{
    float diagonal;
    float margin;

    if (width <= 0 || height <= 0) {
        return (Rectangle){0};
    }
    diagonal = sqrtf((float)width * (float)width +
                     (float)height * (float)height);
    margin = diagonal * 0.08f + 24.0f;
    return (Rectangle){-margin, -margin, (float)width + margin * 2.0f,
                       (float)height + margin * 2.0f};
}

bool EnvironmentRendererViewIsValid(Camera2D camera, int width, int height)
{
    Rectangle overscan = EnvironmentRendererOverscanBounds(width, height);

    return width > 0 && height > 0 && isfinite(camera.target.x) &&
           isfinite(camera.target.y) && isfinite(camera.offset.x) &&
           isfinite(camera.offset.y) && isfinite(camera.rotation) &&
           isfinite(camera.zoom) && camera.zoom > 0.0f &&
           isfinite(overscan.x) && isfinite(overscan.y) &&
           isfinite(overscan.width) && isfinite(overscan.height) &&
           overscan.width >= (float)width && overscan.height >= (float)height;
}

bool EnvironmentRendererStateIsValid(const EnvironmentRenderer *renderer)
{
    const EnvironmentFeature *groups[] = {
        renderer != NULL ? renderer->farPeaks : NULL,
        renderer != NULL ? renderer->structures : NULL,
        renderer != NULL ? renderer->hazeBands : NULL,
        renderer != NULL ? renderer->skyDetails : NULL,
        renderer != NULL ? renderer->nearSpires : NULL,
    };
    const int counts[] = {
        ENVIRONMENT_FAR_PEAK_COUNT,
        ENVIRONMENT_STRUCTURE_COUNT,
        ENVIRONMENT_HAZE_BAND_COUNT,
        ENVIRONMENT_SKY_DETAIL_COUNT,
        ENVIRONMENT_NEAR_SPIRE_COUNT,
    };
    size_t group;

    if (renderer == NULL || renderer->palette < 0 ||
        renderer->palette >= ENVIRONMENT_PALETTE_COUNT ||
        !isfinite(renderer->time)) {
        return false;
    }
    for (group = 0u; group < sizeof(groups) / sizeof(groups[0]); ++group) {
        int index;

        for (index = 0; index < counts[group]; ++index) {
            const EnvironmentFeature *feature = &groups[group][index];

            if (!isfinite(feature->x) || !isfinite(feature->y) ||
                !isfinite(feature->width) || !isfinite(feature->height) ||
                !isfinite(feature->phase) || feature->width <= 0.0f ||
                feature->height <= 0.0f) {
                return false;
            }
        }
    }
    return true;
}

static void EnvironmentDrawSky(EnvironmentRenderer *renderer,
                               const EnvironmentPaletteDefinition *palette,
                               Camera2D camera, int width, int height)
{
    Rectangle bounds = EnvironmentRendererOverscanBounds(width, height);
    int horizon = (int)EnvironmentHorizon(camera, height, 0.010f, 0.48f);
    int index;

    DrawRectangleGradientV((int)bounds.x, (int)bounds.y, (int)bounds.width,
                           (int)bounds.height, palette->skyTop,
                           palette->skyBottom);
    ++renderer->stats.sceneDrawCalls;
    DrawRectangleGradientV(-16, horizon - height / 5, width + 32,
                           height / 3, EnvironmentFade(palette->horizon, 0.0f),
                           EnvironmentFade(palette->horizon, 0.28f));
    ++renderer->stats.sceneDrawCalls;
    DrawRectangle(0, horizon + height / 9, width, 2,
                  EnvironmentFade(palette->horizon, 0.12f));
    ++renderer->stats.sceneDrawCalls;

    for (index = 0; index < ENVIRONMENT_SKY_DETAIL_COUNT; ++index) {
        const EnvironmentFeature *detail = &renderer->skyDetails[index];
        float x = EnvironmentFeatureX(detail, renderer, camera, width, 0.008f,
                                      renderer->palette ==
                                              ENVIRONMENT_PALETTE_EMBER_WASTE
                                          ? -2.5f
                                          : 0.7f);
        float twinkle = 0.55f + 0.45f *
                                    sinf(renderer->time * 0.55f + detail->phase);
        int size = detail->width > 1.5f ? 2 : 1;
        Color color = renderer->palette == ENVIRONMENT_PALETTE_EMBER_WASTE
                          ? palette->horizon
                          : palette->accent;

        DrawRectangle((int)x, (int)(detail->y * (float)height), size, size,
                      EnvironmentFade(color, 0.18f + 0.22f * twinkle));
        ++renderer->stats.sceneDrawCalls;
    }
}

/* A ridge drawn as a stack of columns rather than as a triangle.
 *
 * The world is squares and every effect in it is squares; a smooth hypotenuse
 * on the horizon is the one edge in the picture that came from a different
 * program. Each column's height comes from a shape that runs between a cone and
 * a flat-topped mesa, so the same code draws a volcanic spire and a dune by
 * being handed a different number, and the crest is broken by the same hash the
 * beams use so no two ridges repeat. */
static void EnvironmentRidge(EnvironmentRenderer *renderer, float centreX,
                             float foot, float halfWidth, float peakHeight,
                             float sharpness, float step, Color color,
                             int salt)
{
    float x;

    if (halfWidth <= 0.0f || peakHeight <= 0.0f || step <= 0.0f) {
        return;
    }
    for (x = -halfWidth; x <= halfWidth; x += step) {
        float unit = fabsf(x) / halfWidth;
        /* Cone and mesa, mixed. The mesa is flat across its middle and falls
           away steeply at the sides; the cone falls away linearly from its
           apex. */
        float cone = 1.0f - unit;
        float mesa = unit < 0.62f ? 1.0f : (1.0f - unit) / 0.38f;
        float shape = mesa + (cone - mesa) * sharpness;
        float ragged;
        float top;

        if (shape <= 0.0f) continue;
        ragged = 1.0f - 0.16f * BeamNoise((int)(x / step), salt, 71);
        top = foot - peakHeight * shape * ragged;
        /* Snapped, so the crest steps in whole blocks like everything else. */
        top = floorf(top / step) * step;
        DrawRectangleV((Vector2){centreX + x, top},
                       (Vector2){step + 1.0f, foot - top + 1.0f}, color);
    }
    ++renderer->stats.sceneDrawCalls;
}

/* Trees along a crest: short ragged columns, drawn in the same silhouette
   colour so they read as part of the ridge rather than as objects on it. */
static void EnvironmentTreeline(EnvironmentRenderer *renderer, float fromX,
                                float toX, float crestY, float density,
                                float step, Color color, int salt)
{
    float x;

    if (density <= 0.01f || step <= 0.0f) {
        return;
    }
    for (x = fromX; x <= toX; x += step * 2.0f) {
        float roll = BeamNoise((int)(x / step), salt, 83);
        float height;

        if (roll > density * 0.55f) continue;
        height = step * (2.0f + BeamNoise((int)(x / step), salt, 89) * 5.0f);
        DrawRectangleV((Vector2){x, crestY - height},
                       (Vector2){step, height + 1.0f}, color);
    }
    ++renderer->stats.sceneDrawCalls;
}

/* A smudge rising off a peak: volcanic smoke, or snow blown off a ridge. */
static void EnvironmentPlume(EnvironmentRenderer *renderer, float x, float top,
                             float amount, float step, float time, Color color,
                             int salt)
{
    int puff;

    if (amount <= 0.01f) {
        return;
    }
    for (puff = 0; puff < 7; ++puff) {
        float rise = (float)puff * step * 3.0f * amount;
        float drift = sinf(time * 0.6f + (float)puff * 0.8f + (float)salt) *
                      step * (1.0f + (float)puff * 0.7f);
        float size = step * (1.6f - (float)puff * 0.12f);

        if (size <= 0.0f) break;
        DrawRectangleV((Vector2){x + drift - size * 0.5f, top - rise - size},
                       (Vector2){size, size},
                       EnvironmentFade(color, 0.42f * amount *
                                                  (1.0f - (float)puff / 8.0f)));
    }
    ++renderer->stats.sceneDrawCalls;
}

static void EnvironmentDrawFarLayer(EnvironmentRenderer *renderer,
                                    const EnvironmentPaletteDefinition *palette,
                                    Camera2D camera, int width, int height)
{
    float horizon = EnvironmentHorizon(camera, height, 0.018f, 0.64f);
    float scale = EnvironmentViewScale(camera, width);
    EnvironmentProfile profile = EnvironmentRendererResolvedProfile(renderer);
    /* One block of the backdrop, in screen pixels. Coarser than a world cell
       because the backdrop is far away: a horizon drawn at the same pitch as
       the ground in front of it stops reading as distance. */
    float step = fmaxf(2.0f, 3.0f * scale);
    Color farColor = EnvironmentToward(palette->farSilhouette,
                                       palette->skyBottom, 0.24f);
    int index;

    DrawRectangle(0, (int)horizon, width, height - (int)horizon + 1, farColor);
    ++renderer->stats.sceneDrawCalls;
    for (index = 0; index < ENVIRONMENT_FAR_PEAK_COUNT; ++index) {
        const EnvironmentFeature *peak = &renderer->farPeaks[index];
        float x = EnvironmentFeatureX(peak, renderer, camera, width, 0.018f, 0.0f);
        float halfWidth = (28.0f + peak->width * 46.0f) * scale * profile.breadth;
        float peakHeight = (42.0f + peak->height * 92.0f) * scale * profile.relief;
        float foot = horizon + 2.0f + peak->y * 12.0f;

        EnvironmentRidge(renderer, x, foot, halfWidth, peakHeight,
                         profile.sharpness, step, farColor, index * 13);
        EnvironmentPlume(renderer, x, foot - peakHeight, profile.plume, step,
                         renderer->time,
                         EnvironmentToward(palette->haze, palette->skyBottom,
                                           0.35f),
                         index);
    }
}

static void EnvironmentDrawStructures(
    EnvironmentRenderer *renderer,
    const EnvironmentPaletteDefinition *palette, Camera2D camera, int width,
    int height)
{
    float horizon = EnvironmentHorizon(camera, height, 0.045f, 0.79f);
    float scale = EnvironmentViewScale(camera, width);
    EnvironmentProfile profile = EnvironmentRendererResolvedProfile(renderer);
    int index;

    for (index = 0; index < ENVIRONMENT_STRUCTURE_COUNT; ++index) {
        const EnvironmentFeature *structure = &renderer->structures[index];

        /* A desert horizon has no towers on it, and a forest has a couple. The
           count is a fraction of the eight rather than a flag, so a crossing
           thins them out instead of switching them off. */
        if ((float)index >= profile.towers *
                                (float)ENVIRONMENT_STRUCTURE_COUNT) {
            continue;
        }
        float x = EnvironmentFeatureX(structure, renderer, camera, width,
                                      0.045f, 0.0f);
        float bodyWidth = (12.0f + structure->width * 34.0f) * scale;
        float bodyHeight = (88.0f + structure->height * 210.0f) * scale;
        float top = horizon - bodyHeight + structure->y * 18.0f;
        Color body = EnvironmentToward(palette->midSilhouette,
                                       palette->skyBottom, 0.10f);
        Color cap = EnvironmentToward(body, palette->haze, 0.18f);
        float flicker = 0.72f +
                        0.28f * sinf(renderer->time * 1.35f + structure->phase);

        DrawRectangle((int)(x - bodyWidth * 0.5f), (int)top, (int)bodyWidth,
                      height - (int)top + 8, body);
        DrawRectangle((int)(x - bodyWidth * 0.68f), (int)top,
                      (int)(bodyWidth * 1.36f),
                      (int)fmaxf(2.0f, 4.0f * scale), cap);
        DrawLineEx((Vector2){x, top},
                   (Vector2){x + (structure->phase > 3.1415926f ? -1.0f : 1.0f) *
                                      8.0f * scale,
                             top - (12.0f + structure->height * 20.0f) * scale},
                   fmaxf(1.0f, scale), cap);
        DrawRectangle((int)(x - 1.0f),
                      (int)(top + bodyHeight * (0.20f + structure->y * 0.45f)),
                      EnvironmentMaxInt(1, (int)(3.0f * scale)),
                      EnvironmentMaxInt(1, (int)(2.0f * scale)),
                      EnvironmentFade(palette->accent, 0.62f * flicker));
        renderer->stats.sceneDrawCalls += 4u;
    }
}

static void EnvironmentDrawNearLayer(EnvironmentRenderer *renderer,
                                     const EnvironmentPaletteDefinition *palette,
                                     Camera2D camera, int width, int height)
{
    float horizon = EnvironmentHorizon(camera, height, 0.075f, 0.88f);
    float scale = EnvironmentViewScale(camera, width);
    EnvironmentProfile profile = EnvironmentRendererResolvedProfile(renderer);
    float step = fmaxf(2.0f, 3.0f * scale);
    int index;

    DrawRectangle(0, (int)horizon, width, height - (int)horizon + 1,
                  palette->nearSilhouette);
    ++renderer->stats.sceneDrawCalls;
    for (index = 0; index < ENVIRONMENT_NEAR_SPIRE_COUNT; ++index) {
        const EnvironmentFeature *spire = &renderer->nearSpires[index];
        float x = EnvironmentFeatureX(spire, renderer, camera, width, 0.075f,
                                      0.0f);
        float halfWidth = (12.0f + spire->width * 26.0f) * scale *
                          profile.breadth;
        float spireHeight = (28.0f + spire->height * 82.0f) * scale *
                            profile.relief;
        float foot = horizon + spire->y * 14.0f;

        EnvironmentRidge(renderer, x, foot, halfWidth, spireHeight,
                         profile.sharpness, step, palette->nearSilhouette,
                         index * 29 + 5);
        /* A crest of trees on the near ridge, where a forest biome has one. */
        EnvironmentTreeline(renderer, x - halfWidth, x + halfWidth,
                            foot - spireHeight * 0.34f, profile.treeline, step,
                            palette->nearSilhouette, index * 31);
    }
    EnvironmentTreeline(renderer, 0.0f, (float)width, horizon,
                        profile.treeline, step, palette->nearSilhouette, 907);
}

static void EnvironmentDrawHaze(EnvironmentRenderer *renderer,
                                const EnvironmentPaletteDefinition *palette,
                                Camera2D camera, int width, int height)
{
    int index;

    for (index = 0; index < ENVIRONMENT_HAZE_BAND_COUNT; ++index) {
        const EnvironmentFeature *band = &renderer->hazeBands[index];
        float x = EnvironmentFeatureX(band, renderer, camera, width, 0.028f,
                                      2.0f + (float)index * 0.28f);
        int bandWidth = (int)(band->width * (float)width);
        int bandHeight = (int)fmaxf(4.0f, band->height * (float)height);
        int y = (int)(band->y * (float)height +
                      sinf(renderer->time * 0.12f + band->phase) * 5.0f);
        float alpha = 0.045f + 0.022f * (float)(index + 1);

        DrawRectangleGradientH((int)x, y, bandWidth, bandHeight,
                               EnvironmentFade(palette->haze, 0.0f),
                               EnvironmentFade(palette->haze, alpha));
        ++renderer->stats.sceneDrawCalls;
    }
}

void EnvironmentRendererDrawScene(EnvironmentRenderer *renderer,
                                  Camera2D camera, int width, int height)
{
    const EnvironmentPaletteDefinition *palette;
    EnvironmentPaletteDefinition resolved;

    if (renderer == NULL) {
        return;
    }
    renderer->stats.sceneDrawCalls = 0u;
    renderer->stats.emissiveDrawCalls = 0u;
    renderer->stats.emissiveContributors = 0u;
    renderer->stats.palette = renderer->palette;
    renderer->stats.viewValid =
        EnvironmentRendererViewIsValid(camera, width, height) &&
        EnvironmentRendererStateIsValid(renderer);
    resolved = EnvironmentRendererResolvedPalette(renderer);
    palette = EnvironmentPaletteDefinitionAt(renderer->palette) != NULL
                  ? &resolved
                  : NULL;
    if (!renderer->stats.viewValid || palette == NULL) {
        return;
    }

    /* The environment is screen-space procedural geometry. Camera target and
       zoom drive parallax, but transient camera rotation is deliberately not
       applied as a 2D transform: the full target remains covered and shake can
       never reveal empty corners. */
    EnvironmentDrawSky(renderer, palette, camera, width, height);
    EnvironmentDrawFarLayer(renderer, palette, camera, width, height);
    EnvironmentDrawStructures(renderer, palette, camera, width, height);
    EnvironmentDrawNearLayer(renderer, palette, camera, width, height);
    EnvironmentDrawHaze(renderer, palette, camera, width, height);

    /* And then dissolved back into its own sky by however far out of the air
       the camera has climbed. Drawn as one veil of the sky's own colour rather
       than by fading every layer: the layers overlap, and fading each of them
       separately shows the ones behind through the ones in front, which reads
       as the horizon becoming transparent rather than as its going away. */
    if (renderer->altitude < 1.0f) {
        Rectangle bounds = EnvironmentRendererOverscanBounds(width, height);
        float amount = 1.0f - renderer->altitude;
        Color top = EnvironmentFade(palette->skyTop, amount);
        Color bottom = EnvironmentFade(palette->skyBottom, amount);

        /* The sky's own gradient, laid back over the layers — not a flat fill.
           Flooding with one colour dissolves the horizon and the sky with it,
           and leaves a slab of paint where the atmosphere should be. */
        DrawRectangleGradientV((int)bounds.x, (int)bounds.y, (int)bounds.width,
                               (int)bounds.height, top, bottom);
        ++renderer->stats.sceneDrawCalls;
    }
}

void EnvironmentRendererDrawEmissive(EnvironmentRenderer *renderer,
                                     Camera2D camera, int width, int height)
{
    const EnvironmentPaletteDefinition *palette;
    EnvironmentProfile profile;
    float horizon;
    float scale;
    int index;

    if (renderer == NULL ||
        !EnvironmentRendererViewIsValid(camera, width, height) ||
        !EnvironmentRendererStateIsValid(renderer)) {
        return;
    }
    palette = EnvironmentPaletteDefinitionAt(renderer->palette);
    if (palette == NULL) {
        return;
    }
    horizon = EnvironmentHorizon(camera, height, 0.045f, 0.79f);
    scale = EnvironmentViewScale(camera, width);
    profile = EnvironmentRendererResolvedProfile(renderer);
    for (index = 0; index < ENVIRONMENT_STRUCTURE_COUNT; ++index) {
        const EnvironmentFeature *structure = &renderer->structures[index];

        /* A desert horizon has no towers on it, and a forest has a couple. The
           count is a fraction of the eight rather than a flag, so a crossing
           thins them out instead of switching them off. */
        if ((float)index >= profile.towers *
                                (float)ENVIRONMENT_STRUCTURE_COUNT) {
            continue;
        }
        float x = EnvironmentFeatureX(structure, renderer, camera, width,
                                      0.045f, 0.0f);
        float bodyHeight = (88.0f + structure->height * 210.0f) * scale;
        float top = horizon - bodyHeight + structure->y * 18.0f;
        float flicker = 0.68f +
                        0.32f * sinf(renderer->time * 1.35f + structure->phase);
        int lightY = (int)(top + bodyHeight *
                                     (0.20f + structure->y * 0.45f));

        DrawRectangle((int)(x - 1.0f), lightY,
                      EnvironmentMaxInt(1, (int)(3.0f * scale)),
                      EnvironmentMaxInt(1, (int)(2.0f * scale)),
                      EnvironmentFade(palette->accent, 0.52f * flicker));
        ++renderer->stats.emissiveDrawCalls;
        ++renderer->stats.emissiveContributors;
        if ((index % 3) == 0) {
            DrawRectangle((int)x, (int)(top - 15.0f * scale), 1,
                          (int)(bodyHeight * 0.38f),
                          EnvironmentFade(palette->accent,
                                          0.085f * flicker));
            ++renderer->stats.emissiveDrawCalls;
            ++renderer->stats.emissiveContributors;
        }
    }
}

const EnvironmentRendererStats *EnvironmentRendererStatistics(
    const EnvironmentRenderer *renderer)
{
    static const EnvironmentRendererStats empty = {0};

    return renderer != NULL ? &renderer->stats : &empty;
}
