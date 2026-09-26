#include "environment_renderer.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <rlgl.h>

#include "beam_render.h"

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
        .profile = {1.00f, 0.86f, 1.18f, 0.75f, 0.00f, 0.85f,
                    0.00f, 1.00f, 0.00f, 0.00f},
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
        .profile = {0.05f, 1.62f, 0.24f, 0.00f, 0.00f, 0.45f,
                    0.00f, 0.00f, 1.00f, 0.00f},
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
        .profile = {0.42f, 1.20f, 0.70f, 0.25f, 1.00f, 0.15f,
                    0.35f, 0.00f, 0.00f, 0.00f},
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
        .profile = {0.08f, 1.45f, 0.55f, 0.00f, 0.00f, 0.35f,
                    0.00f, 0.00f, 0.00f, 0.85f},
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
        .profile = {0.82f, 0.95f, 1.05f, 0.10f, 0.45f, 0.55f,
                    1.00f, 0.00f, 0.00f, 0.00f},
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
    blended.snow = from->profile.snow + (to->profile.snow - from->profile.snow) * amount;
    blended.volcanoes = from->profile.volcanoes +
                        (to->profile.volcanoes - from->profile.volcanoes) * amount;
    blended.sea = from->profile.sea + (to->profile.sea - from->profile.sea) * amount;
    blended.mesa = from->profile.mesa + (to->profile.mesa - from->profile.mesa) * amount;
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

void EnvironmentRendererSetDayPhase(EnvironmentRenderer *renderer, float dayPhase)
{
    float wrapped;

    if (renderer == NULL) {
        return;
    }
    wrapped = dayPhase - floorf(dayPhase);
    /* A day turned over: the moon moves on to its next phase. */
    if (wrapped < renderer->dayPhase - 0.5f) {
        ++renderer->days;
    }
    renderer->dayPhase = wrapped;
}

void EnvironmentRendererSetTravel(EnvironmentRenderer *renderer, float travel)
{
    if (renderer == NULL) {
        return;
    }
    renderer->travel = travel;
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
    float shift = (camera.target.x + renderer->travel) * camera.zoom * parallax;
    float animated = renderer->time * drift + feature->phase * 13.0f;

    return EnvironmentWrap(feature->x * period - shift + animated, period) -
           margin;
}

static float EnvironmentViewScale(Camera2D camera, int width)
{
    float scale = camera.zoom * 426.0f / (float)width;

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

/* ---- leaving the air ------------------------------------------------------

   Climbing out of the atmosphere is not a crossfade. Three things happen, the
   way they do on the way up in the painted backdrops' own language of stepped
   bands:

   - the sky goes dark from the top down, in bands, as the air under the
     camera thins — the zenith first, the horizon last;
   - the ranges sink, lose their height, and bend: the far edges of the
     screen drop away faster than the middle, until the land is the curve of
     a planet seen from above it;
   - over that curve a stack of glowing bands stands up, the atmosphere seen
     edge on, and it is the last of the sky that is left.

   `climb` is 0 on the ground and 1 in space. */
static float EnvironmentClimb(const EnvironmentRenderer *renderer)
{
    return EnvironmentClamp(1.0f - renderer->altitude, 0.0f, 1.0f);
}

/* How dark the sky is at screen fraction `t` (0 top, 1 bottom). */
static float EnvironmentDarkAt(float climb, float t)
{
    return EnvironmentClamp((climb * 1.9f - 0.02f - t * 1.3f) * 1.6f, 0.0f, 1.0f);
}

/* How far column `x` of the land has sunk under the climb: all of it a
   little, the edges a lot, which is the curve. */
static float EnvironmentCurveDrop(float climb, float x, int width, int height)
{
    float across = (x - (float)width * 0.5f) / ((float)width * 0.5f);
    float bend = powf(climb, 1.3f);

    return climb * (float)height * 0.26f + bend * (float)height * 0.5f * across * across;
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
    renderer->dayPhase = 0.25f;
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

    /* The dark coming down from the top, in steps. */
    {
        float climb = EnvironmentClimb(renderer);
        int band = EnvironmentMaxInt(2, height / 45);
        int y;

        if (climb > 0.0f) {
            for (y = (int)bounds.y; y < (int)(bounds.y + bounds.height); y += band) {
                float dark = EnvironmentDarkAt(climb, ((float)y + (float)band * 0.5f) /
                                                          (float)height);

                dark = floorf(dark * 8.0f + 0.5f) / 8.0f;
                if (dark <= 0.0f) continue;
                DrawRectangle((int)bounds.x, y, (int)bounds.width, band,
                              EnvironmentFade((Color){3, 4, 12, 255}, dark));
                ++renderer->stats.sceneDrawCalls;
            }
        }
    }

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

/* ---- the sun and the moon -------------------------------------------------

   Both ride one arc across the backdrop: up from the left end of the horizon,
   over the top of the view, down to the right end. The sun takes the day half
   of the phase and the moon the night half, so one is always setting as the
   other rises. They are infinitely far away and have no parallax: the arc is
   fixed on the screen, and only the horizon it sets behind moves with the
   camera.

   Drawn in blocks the size of a world cell, like everything else. */
typedef struct EnvironmentOrb {
    float x;
    float y;
    /* Height over the horizon, 0 at it and 1 at the top of the arc. */
    float elevation;
    bool visible;
} EnvironmentOrb;

static EnvironmentOrb EnvironmentOrbAt(float phase, Camera2D camera, int width,
                                       int height)
{
    EnvironmentOrb orb = {0};
    float horizon = EnvironmentHorizon(camera, height, 0.010f, 0.48f);
    float angle;

    phase -= floorf(phase);
    if (phase >= 0.5f) {
        return orb;
    }
    angle = phase / 0.5f * PI;
    orb.elevation = sinf(angle);
    orb.x = (float)width * 0.5f - cosf(angle) * (float)width * 0.44f;
    orb.y = horizon + (float)height * 0.06f -
            orb.elevation * (horizon + (float)height * 0.06f - (float)height * 0.12f);
    orb.visible = true;
    return orb;
}

static int EnvironmentBlock(int height)
{
    return EnvironmentMaxInt(1, height / 240);
}

/* A soft radial glow: `inner` at the centre fading to nothing at `radius`,
   drawn as a fan of triangles with a colour per vertex so the falloff is
   smooth. Light is the one thing in the picture that is not made of blocks:
   a halo drawn in blocks reads as a target, not as brightness. */
static void EnvironmentGlow(float x, float y, float radius, Color inner, int segments)
{
    int index;

    if (radius <= 0.5f || inner.a == 0u) return;
    rlBegin(RL_TRIANGLES);
    for (index = 0; index < segments; ++index) {
        float a0 = (float)index / (float)segments * 2.0f * PI;
        float a1 = (float)(index + 1) / (float)segments * 2.0f * PI;

        rlColor4ub(inner.r, inner.g, inner.b, inner.a);
        rlVertex2f(x, y);
        rlColor4ub(inner.r, inner.g, inner.b, 0);
        rlVertex2f(x + cosf(a1) * radius, y + sinf(a1) * radius);
        rlColor4ub(inner.r, inner.g, inner.b, 0);
        rlVertex2f(x + cosf(a0) * radius, y + sinf(a0) * radius);
    }
    rlEnd();
}

/* One ray: a long thin wedge from the sun, bright at its root and gone at its
   tip. */
static void EnvironmentRay(float x, float y, float angle, float length, float spread,
                           Color color)
{
    float left = angle - spread;
    float right = angle + spread;

    rlBegin(RL_TRIANGLES);
    rlColor4ub(color.r, color.g, color.b, color.a);
    rlVertex2f(x, y);
    rlColor4ub(color.r, color.g, color.b, 0);
    rlVertex2f(x + cosf(right) * length, y + sinf(right) * length);
    rlColor4ub(color.r, color.g, color.b, 0);
    rlVertex2f(x + cosf(left) * length, y + sinf(left) * length);
    rlEnd();
}

/* The sun: a pixel disc white at the heart and gold at the limb, in a
   great soft glow, with long rays turning slowly about it. As it sinks the
   air it is seen through thickens: the disc swells and reddens, the glow
   spreads wide and warm, and the rays lie longer over the horizon. */
static void EnvironmentDrawSun(EnvironmentRenderer *renderer, Camera2D camera,
                               int width, int height, float alpha, bool emissive)
{
    EnvironmentOrb orb = EnvironmentOrbAt(renderer->dayPhase, camera, width, height);
    int block = EnvironmentBlock(height);
    float low;
    int radius;
    int row;
    int ray;
    Color glow;
    Color rim;
    Color core;

    if (!orb.visible || alpha <= 0.0f) {
        return;
    }
    low = 1.0f - EnvironmentClamp(orb.elevation * 2.2f, 0.0f, 1.0f);
    if (emissive) {
        /* The far ranges are not drawn in the emissive plane and cannot hide
           a sun going down behind them, so its bloom fades as it sinks. */
        alpha *= EnvironmentClamp((orb.elevation - 0.03f) / 0.22f, 0.0f, 1.0f);
        if (alpha <= 0.0f) {
            return;
        }
    }
    radius = 11 + (int)(4.0f * low);
    glow = (Color){255, (unsigned char)(226.0f - 90.0f * low),
                   (unsigned char)(160.0f - 110.0f * low),
                   (unsigned char)(255.0f * alpha * (0.34f + 0.18f * low))};
    rim = (Color){255, (unsigned char)(196.0f - 96.0f * low),
                  (unsigned char)(92.0f - 70.0f * low), 255};
    core = (Color){255, 252, (unsigned char)(236.0f - 60.0f * low), 255};

    /* The glow, in two layers: a wide faint one and a tight bright one. */
    EnvironmentGlow(orb.x, orb.y, (float)(radius * block) * (9.0f + 6.0f * low), glow, 48);
    EnvironmentGlow(orb.x, orb.y, (float)(radius * block) * 3.2f,
                    EnvironmentFade(glow, alpha * 0.55f), 40);
    /* Rays, turning slowly, each its own length. */
    for (ray = 0; ray < 12; ++ray) {
        float angle = (float)ray / 12.0f * 2.0f * PI + renderer->time * 0.018f +
                      0.26f * sinf((float)ray * 1.7f);
        float length = (float)(radius * block) *
                       (5.0f + 4.5f * BeamNoise(ray, 7, 11) + 3.0f * low);

        EnvironmentRay(orb.x, orb.y, angle, length, 0.035f + 0.02f * BeamNoise(ray, 9, 13),
                       EnvironmentFade(glow, alpha * (emissive ? 0.25f : 0.18f)));
    }
    /* The disc, in blocks: limb-darkened from a white heart to a gold rim. */
    for (row = -radius; row <= radius; ++row) {
        int column;
        int half = (int)floorf(sqrtf((float)(radius * radius - row * row)) + 0.4f);

        for (column = -half; column <= half; ++column) {
            float r = sqrtf((float)(row * row + column * column)) / (float)radius;
            Color cell = r < 0.55f
                             ? core
                             : (Color){(unsigned char)((float)core.r + ((float)rim.r - (float)core.r) * (r - 0.55f) / 0.45f),
                                       (unsigned char)((float)core.g + ((float)rim.g - (float)core.g) * (r - 0.55f) / 0.45f),
                                       (unsigned char)((float)core.b + ((float)rim.b - (float)core.b) * (r - 0.55f) / 0.45f),
                                       255};

            DrawRectangle((int)orb.x + column * block - block / 2,
                          (int)orb.y + row * block - block / 2, block, block,
                          EnvironmentFade(cell, alpha));
        }
    }
    if (emissive) {
        ++renderer->stats.emissiveDrawCalls;
    } else {
        renderer->stats.sceneDrawCalls += 4u;
    }
}

/* The moon: a sphere lit from one side, the lit part growing and shrinking
   night by night; the dark side shows faintly by the light of the world
   below it. Seas and craters are pressed into it, each crater with a lit
   rim on the side toward the light and a shadow on the other. A cold glow
   stands around it. */
static void EnvironmentDrawMoon(EnvironmentRenderer *renderer, Camera2D camera,
                                int width, int height, float alpha, bool emissive)
{
    EnvironmentOrb orb = EnvironmentOrbAt(renderer->dayPhase - 0.5f, camera, width,
                                          height);
    int block = EnvironmentBlock(height);
    const int radius = 10;
    /* The phase: a lunar month of eight days, starting from full. */
    float cycle = (float)((renderer->days + 4u) % 8u) / 8.0f + renderer->dayPhase / 8.0f;
    float lightAngle = cycle * 2.0f * PI;
    float lightX = sinf(lightAngle);
    float lightZ = -cosf(lightAngle);
    int row;

    if (!orb.visible || alpha <= 0.0f) {
        return;
    }
    if (emissive) {
        alpha *= 0.4f * EnvironmentClamp((orb.elevation - 0.03f) / 0.22f, 0.0f, 1.0f);
        if (alpha <= 0.0f) {
            return;
        }
    }
    EnvironmentGlow(orb.x, orb.y, (float)(radius * block) * 6.0f,
                    (Color){150, 180, 230, (unsigned char)(255.0f * alpha * 0.16f)}, 40);
    EnvironmentGlow(orb.x, orb.y, (float)(radius * block) * 2.2f,
                    (Color){190, 210, 245, (unsigned char)(255.0f * alpha * 0.22f)}, 32);
    for (row = -radius; row <= radius; ++row) {
        int column;
        int half = (int)floorf(sqrtf((float)(radius * radius - row * row)) + 0.4f);

        for (column = -half; column <= half; ++column) {
            float nx = (float)column / (float)radius;
            float ny = (float)row / (float)radius;
            float nz = sqrtf(fmaxf(0.0f, 1.0f - nx * nx - ny * ny));
            float lit = nx * lightX + nz * lightZ;
            float sea = BeamNoise(column / 3 + 20, row / 3 + 20, 0x51) < 0.32f ? 1.0f : 0.0f;
            float crater = BeamNoise(column + 40, row + 40, 0x6d00);
            float shade;
            float tone;
            Color cell;

            /* Light: smooth across the terminator by two blocks, and a faint
               earthshine on the night side. */
            shade = EnvironmentClamp(lit * 3.0f + 0.35f, 0.0f, 1.0f);
            tone = 0.10f + 0.90f * shade;
            tone *= 0.82f + 0.18f * nz;
            if (sea > 0.0f) tone *= 0.82f;
            if (crater > 0.93f) {
                tone *= 0.72f;
            } else if (crater > 0.88f) {
                tone *= 1.10f;
            }
            if (emissive) {
                tone *= shade;
            }
            cell = (Color){(unsigned char)EnvironmentClamp(226.0f * tone, 0.0f, 255.0f),
                           (unsigned char)EnvironmentClamp(230.0f * tone, 0.0f, 255.0f),
                           (unsigned char)EnvironmentClamp(240.0f * tone + 14.0f * (1.0f - shade), 0.0f, 255.0f),
                           255};
            DrawRectangle((int)orb.x + column * block - block / 2,
                          (int)orb.y + row * block - block / 2, block, block,
                          EnvironmentFade(cell, alpha));
        }
    }
    if (emissive) {
        ++renderer->stats.emissiveDrawCalls;
    } else {
        renderer->stats.sceneDrawCalls += 2u;
    }
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

/* ---- the ranges ----------------------------------------------------------
 *
 * The backdrop is four ranges one behind the other, each a continuous ridge
 * line rather than a row of separate peaks: a horizon is a line the eye
 * follows, and a row of cones is a row of objects. Each is drawn in blocks a
 * few pixels across, like everything else, and each is further into the sky's
 * own colour than the one in front of it — atmospheric perspective is most of
 * what makes a flat picture read as distance. The top block of every column
 * catches the light, and the foot of each range sinks into the mist of the
 * valley in front of the next.
 */

typedef struct EnvironmentRange {
    /* How fast it scrolls against the camera, and where its foot is. */
    float parallax;
    float base;
    /* Peak height in backdrop pixels at scale one. */
    float amplitude;
    /* How far into the sky's colour it has faded, 0..1. */
    float fog;
    /* Its own colour before the fog. */
    Color color;
    /* Trees along it, 0..1 of the profile's treeline. */
    float trees;
    int salt;
} EnvironmentRange;

/* Smooth value noise in backdrop units, 0..1. */
static float EnvironmentNoise(const EnvironmentRenderer *renderer, float u,
                              float wavelength, int salt)
{
    float position = u / wavelength;
    float cell = floorf(position);
    float t = position - cell;
    int seedSalt = (int)(renderer->seed & 0x7fffu) + salt * 131;
    float a = BeamNoise((int)cell, seedSalt, 17);
    float b = BeamNoise((int)cell + 1, seedSalt, 17);

    t = t * t * (3.0f - 2.0f * t);
    return a + (b - a) * t;
}

/* The ridge height of a range at `u`, 0..1: rolling where the profile is
   soft, ridged and peaked where it is sharp, stepped where it is mesa. */
static float EnvironmentRangeHeight(const EnvironmentRenderer *renderer,
                                    const EnvironmentProfile *profile, float u,
                                    int salt)
{
    float breadth = fmaxf(0.4f, profile->breadth);
    float broad = EnvironmentNoise(renderer, u, 380.0f * breadth, salt + 1);
    float middle = EnvironmentNoise(renderer, u, 130.0f * breadth, salt + 2);
    float fine = EnvironmentNoise(renderer, u, 38.0f, salt + 3);
    float ridge = 1.0f - fabsf(EnvironmentNoise(renderer, u, 210.0f * breadth, salt + 4) *
                                   2.0f - 1.0f);
    float soft = 0.55f * broad + 0.32f * middle + 0.13f * fine;
    float sharp = 0.55f * ridge * ridge + 0.30f * broad + 0.15f * fine;
    float height = soft + (sharp - soft) * EnvironmentClamp(profile->sharpness, 0.0f, 1.0f);

    if (profile->mesa > 0.01f) {
        float steps = height * 4.0f;
        float step = floorf(steps);
        float rise = steps - step;
        float stepped = (step + rise * rise * rise * rise * rise * rise) / 4.0f;

        height += (stepped - height) * EnvironmentClamp(profile->mesa, 0.0f, 1.0f);
    }
    return EnvironmentClamp(height, 0.0f, 1.0f);
}

/* Where column `x` of the screen is along a range, in backdrop units. */
static float EnvironmentRangeU(const EnvironmentRenderer *renderer, Camera2D camera,
                               float parallax, float scale, float x)
{
    float shift = (camera.target.x + renderer->travel) * camera.zoom * parallax;

    return (x + shift) / scale;
}

/* A tree silhouette on a crest: a pine where the snow is, a round crown
   elsewhere. */
static void EnvironmentTree(float x, float crest, float step, float size, bool pine,
                            Color color)
{
    int row;
    int rows = (int)size;

    if (pine) {
        for (row = 0; row < rows; ++row) {
            float half = floorf((float)(row + 1) * 0.5f) * step;

            DrawRectangleV((Vector2){x - half, crest - (float)(rows - row) * step},
                           (Vector2){half * 2.0f + step, step}, color);
        }
        return;
    }
    DrawRectangleV((Vector2){x, crest - step * 2.0f}, (Vector2){step, step * 2.0f},
                   color);
    for (row = 0; row < rows; ++row) {
        float half = floorf(sqrtf((float)(row * (rows - row))) * 0.8f) * step;

        DrawRectangleV((Vector2){x - half, crest - step * 2.0f - (float)(rows - row) * step},
                       (Vector2){half * 2.0f + step, step}, color);
    }
}

static void EnvironmentDrawRange(EnvironmentRenderer *renderer,
                                 const EnvironmentPaletteDefinition *palette,
                                 const EnvironmentProfile *profile,
                                 const EnvironmentRange *range, Camera2D camera,
                                 int width, int height)
{
    float horizon = EnvironmentHorizon(camera, height, range->parallax, range->base);
    float scale = EnvironmentViewScale(camera, width);
    float step = fmaxf(2.0f, floorf(3.0f * scale));
    float climb = EnvironmentClimb(renderer);
    /* From above, mountains are wrinkles. */
    float amplitude = range->amplitude * scale * fmaxf(0.15f, profile->relief) *
                      (1.0f - 0.8f * climb);
    Color body = EnvironmentToward(range->color, palette->skyBottom, range->fog);
    Color lit = EnvironmentToward(body, palette->horizon, 0.28f);
    Color mist = EnvironmentToward(body, palette->haze, 0.45f);
    Color snow = EnvironmentToward((Color){232, 240, 250, 255}, palette->skyBottom,
                                   range->fog * 0.8f);
    Color treeColor = EnvironmentToward(body, palette->nearSilhouette, 0.18f);
    float snowLine = 1.0f - 0.55f * EnvironmentClamp(profile->snow, 0.0f, 1.0f);
    bool pines = profile->snow > 0.5f;
    float x;

    snow = EnvironmentToward(snow, body, 0.12f);
    for (x = -step; x <= (float)width + step; x += step) {
        float foot = horizon + floorf(EnvironmentCurveDrop(climb, x, width, height) / step) *
                                   step;
        float u = EnvironmentRangeU(renderer, camera, range->parallax, scale, x);
        float ridge = EnvironmentRangeHeight(renderer, profile, u, range->salt);
        float top = floorf((foot - amplitude * ridge) / step) * step;
        float bottom = (float)height + 1.0f;

        /* The body, sinking into the valley mist toward its foot. */
        DrawRectangleGradientV((int)x, (int)top, (int)step,
                               (int)(foot + step - top), body, mist);
        if (bottom > foot + step) {
            DrawRectangle((int)x, (int)(foot + step), (int)step,
                          (int)(bottom - foot - step), mist);
        }
        /* Snow on what reaches above the line, deeper the higher. */
        if (profile->snow > 0.01f && ridge > snowLine) {
            float depth = floorf((ridge - snowLine) * amplitude * 0.9f / step) * step +
                          step;

            DrawRectangle((int)x, (int)top, (int)step, (int)depth, snow);
        } else {
            /* The crest catches the light. */
            DrawRectangle((int)x, (int)top, (int)step, (int)step, lit);
        }
        /* Trees on the crest. */
        if (range->trees > 0.01f && profile->treeline > 0.01f &&
            (profile->snow < 0.01f || ridge <= snowLine)) {
            float roll = BeamNoise((int)floorf(u / 7.0f), range->salt, 83);

            if (roll < range->trees * profile->treeline * 0.4f &&
                fmodf(fabsf(u), 7.0f) < 7.0f * step / scale / 3.0f) {
                EnvironmentTree(x, top, step,
                                3.0f + BeamNoise((int)floorf(u / 7.0f), range->salt, 89) * 4.0f,
                                pines, treeColor);
            }
        }
    }
    renderer->stats.sceneDrawCalls += 4u;
}

/* The volcanoes on the middle range: cones rising out of it, a notch at the
   top where the crater is, and smoke above. Their crater glows in the
   emissive plane. */
static bool EnvironmentVolcano(const EnvironmentRenderer *renderer,
                               const EnvironmentProfile *profile, Camera2D camera,
                               int width, int height, int index, float *x, float *foot,
                               float *halfWidth, float *peak)
{
    const EnvironmentFeature *feature = &renderer->farPeaks[index];
    float scale = EnvironmentViewScale(camera, width);

    if ((float)index >= profile->volcanoes * 4.0f) {
        return false;
    }
    *x = EnvironmentFeatureX(feature, renderer, camera, width, 0.022f, 0.0f);
    *foot = EnvironmentHorizon(camera, height, 0.022f, 0.70f) + 4.0f;
    *halfWidth = (60.0f + feature->width * 70.0f) * scale;
    *peak = (90.0f + feature->height * 90.0f) * scale;
    return true;
}

static void EnvironmentDrawVolcanoes(EnvironmentRenderer *renderer,
                                     const EnvironmentPaletteDefinition *palette,
                                     const EnvironmentProfile *profile,
                                     Camera2D camera, int width, int height)
{
    float scale = EnvironmentViewScale(camera, width);
    float step = fmaxf(2.0f, floorf(3.0f * scale));
    Color body = EnvironmentToward(palette->farSilhouette, palette->skyBottom, 0.22f);
    Color lit = EnvironmentToward(body, palette->horizon, 0.3f);
    int index;

    for (index = 0; index < 4; ++index) {
        float x;
        float foot;
        float halfWidth;
        float peak;
        float dx;

        if (!EnvironmentVolcano(renderer, profile, camera, width, height, index, &x,
                                &foot, &halfWidth, &peak)) {
            continue;
        }
        for (dx = -halfWidth; dx <= halfWidth; dx += step) {
            float unit = fabsf(dx) / halfWidth;
            /* Concave flanks, and a crater notch in the top eighth. */
            float shape = (1.0f - unit) * (1.0f - unit) * 0.35f + (1.0f - unit) * 0.65f;
            float top;

            if (unit < 0.12f) shape = 0.92f - 0.08f * (1.0f - unit / 0.12f);
            top = floorf((foot - peak * shape) / step) * step;
            DrawRectangle((int)(x + dx), (int)top, (int)step, (int)(foot - top + step),
                          body);
            DrawRectangle((int)(x + dx), (int)top, (int)step, (int)step, lit);
        }
        EnvironmentPlume(renderer, x, foot - peak * 0.92f, profile->plume, step * 1.6f,
                         renderer->time,
                         EnvironmentToward(palette->haze, palette->skyBottom, 0.25f), index);
        renderer->stats.sceneDrawCalls += 2u;
    }
}

/* A ruined tower on the middle range: tapered, broken at the top into a
   jagged stump, a window or two still lit. The emissive pass draws the lit
   windows from the same geometry. */
static bool EnvironmentTower(const EnvironmentRenderer *renderer,
                             const EnvironmentProfile *profile, Camera2D camera,
                             int width, int height, int index, float *x, float *foot,
                             float *bodyWidth, float *bodyHeight)
{
    const EnvironmentFeature *structure = &renderer->structures[index];
    float scale = EnvironmentViewScale(camera, width);
    float u;
    float ridge;
    float rangeFoot;

    if ((float)index >= profile->towers * (float)ENVIRONMENT_STRUCTURE_COUNT) {
        return false;
    }
    *x = EnvironmentFeatureX(structure, renderer, camera, width, 0.045f, 0.0f);
    /* Standing on the range, not floating in front of it. */
    u = EnvironmentRangeU(renderer, camera, 0.045f, scale, *x);
    ridge = EnvironmentRangeHeight(renderer, profile, u, 300);
    rangeFoot = EnvironmentHorizon(camera, height, 0.045f, 0.80f);
    *foot = rangeFoot - 70.0f * scale * fmaxf(0.15f, profile->relief) * ridge + 6.0f;
    *bodyWidth = (10.0f + structure->width * 14.0f) * scale;
    *bodyHeight = (40.0f + structure->height * 70.0f) * scale;
    return true;
}

static void EnvironmentDrawTowers(EnvironmentRenderer *renderer,
                                  const EnvironmentPaletteDefinition *palette,
                                  const EnvironmentProfile *profile, Camera2D camera,
                                  int width, int height)
{
    float scale = EnvironmentViewScale(camera, width);
    float step = fmaxf(2.0f, floorf(3.0f * scale));
    Color body = EnvironmentToward(palette->midSilhouette, palette->skyBottom, 0.10f);
    int index;

    for (index = 0; index < ENVIRONMENT_STRUCTURE_COUNT; ++index) {
        float x;
        float foot;
        float bodyWidth;
        float bodyHeight;
        float row;

        if (!EnvironmentTower(renderer, profile, camera, width, height, index, &x, &foot,
                              &bodyWidth, &bodyHeight)) {
            continue;
        }
        for (row = 0.0f; row < bodyHeight; row += step) {
            float taper = 1.0f - 0.18f * row / bodyHeight;
            float half = floorf(bodyWidth * 0.5f * taper / step) * step;
            float y = foot - row - step;
            float column;

            /* The broken top: the last fifth is ragged, block by block. */
            for (column = -half; column <= half; column += step) {
                if (row > bodyHeight * 0.8f &&
                    BeamNoise((int)(column / step), (int)(row / step) + index * 37, 97) <
                        (row - bodyHeight * 0.8f) / (bodyHeight * 0.2f)) {
                    continue;
                }
                DrawRectangle((int)(x + column), (int)y, (int)step, (int)step, body);
            }
        }
        renderer->stats.sceneDrawCalls += 1u;
    }
}

/* Open water in front of the far ranges: a flat band with the light lying on
   it in broken lines. */
static void EnvironmentDrawSea(EnvironmentRenderer *renderer,
                               const EnvironmentPaletteDefinition *palette,
                               const EnvironmentProfile *profile, Camera2D camera,
                               int width, int height)
{
    float amount = EnvironmentClamp(profile->sea, 0.0f, 1.0f);
    float horizon = EnvironmentHorizon(camera, height, 0.03f, 0.72f);
    float scale = EnvironmentViewScale(camera, width);
    float step = fmaxf(2.0f, floorf(2.0f * scale));
    Color sea = EnvironmentFade(EnvironmentToward(palette->midSilhouette, palette->horizon,
                                                  0.25f),
                                amount);
    Color glint = EnvironmentFade(palette->horizon, 0.55f * amount);
    float y;

    if (amount <= 0.01f) {
        return;
    }
    DrawRectangle(0, (int)horizon, width, height - (int)horizon + 1, sea);
    for (y = horizon + step; y < (float)height; y += step * 3.0f) {
        float depth = (y - horizon) / ((float)height - horizon);
        float x;

        for (x = 0.0f; x < (float)width; x += step * 8.0f) {
            float u = EnvironmentRangeU(renderer, camera, 0.03f + depth * 0.05f, scale, x);
            float roll = BeamNoise((int)(u / 9.0f), (int)(y / step), 53);

            if (roll < 0.35f) {
                DrawRectangle((int)x, (int)y, (int)(step * (3.0f + roll * 8.0f)),
                              (int)step,
                              EnvironmentFade(glint, 0.55f * amount * (1.0f - depth)));
            }
        }
    }
    renderer->stats.sceneDrawCalls += 2u;
}

static void EnvironmentDrawRanges(EnvironmentRenderer *renderer,
                                  const EnvironmentPaletteDefinition *palette,
                                  Camera2D camera, int width, int height)
{
    EnvironmentProfile profile = EnvironmentRendererResolvedProfile(renderer);
    EnvironmentRange ranges[4] = {
        {0.010f, 0.60f, 200.0f, 0.62f, palette->farSilhouette, 0.0f, 100},
        {0.022f, 0.68f, 165.0f, 0.36f, palette->farSilhouette, 0.0f, 200},
        {0.045f, 0.80f, 70.0f, 0.12f, palette->midSilhouette, 0.6f, 300},
        {0.075f, 0.90f, 56.0f, 0.0f, palette->nearSilhouette, 1.0f, 400},
    };
    int index;

    for (index = 0; index < 4; ++index) {
        /* The sea lies in front of the far ranges and the land beyond it is
           low: over open water the nearer ranges drop away. */
        EnvironmentRange range = ranges[index];

        if (index >= 2) {
            range.amplitude *= 1.0f - 0.85f * EnvironmentClamp(profile.sea, 0.0f, 1.0f);
        }
        EnvironmentDrawRange(renderer, palette, &profile, &range, camera, width, height);
        /* The small things on the land are lost first on the way up. */
        if (index == 1 && EnvironmentClimb(renderer) < 0.2f) {
            EnvironmentDrawVolcanoes(renderer, palette, &profile, camera, width, height);
            EnvironmentDrawSea(renderer, palette, &profile, camera, width, height);
        }
        if (index == 2 && EnvironmentClimb(renderer) < 0.2f) {
            EnvironmentDrawTowers(renderer, palette, &profile, camera, width, height);
        }
    }
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
        float alpha = (0.045f + 0.022f * (float)(index + 1)) *
                      (1.0f - EnvironmentClimb(renderer));

        DrawRectangleGradientH((int)x, y, bandWidth, bandHeight,
                               EnvironmentFade(palette->haze, 0.0f),
                               EnvironmentFade(palette->haze, alpha));
        ++renderer->stats.sceneDrawCalls;
    }
}

/* The atmosphere seen edge on: a stack of glowing bands standing over the
   far range's curve, the horizon's own colour at the bottom going to a deep
   blue at the top, each band thinner and fainter than the one below it.
   Nothing on the ground — it grows as the dark comes down. */
static void EnvironmentDrawLimb(EnvironmentRenderer *renderer,
                                const EnvironmentPaletteDefinition *palette,
                                Camera2D camera, int width, int height)
{
    float climb = EnvironmentClimb(renderer);
    float strength = EnvironmentClamp(climb * 2.2f, 0.0f, 1.0f);
    float scale = EnvironmentViewScale(camera, width);
    float step = fmaxf(2.0f, floorf(3.0f * scale));
    float horizon = EnvironmentHorizon(camera, height, 0.010f, 0.60f);
    float thickness = (float)height * (0.05f + 0.07f * climb);
    const int bands = 6;
    Color deep = {40, 70, 150, 255};
    float x;

    if (strength <= 0.01f) {
        return;
    }
    for (x = -step; x <= (float)width + step; x += step) {
        float base = horizon + floorf(EnvironmentCurveDrop(climb, x, width, height) / step) *
                                   step -
                     (float)height * 0.02f;
        int band;

        for (band = 0; band < bands; ++band) {
            float along = (float)band / (float)(bands - 1);
            float bottom = base - floorf(thickness * along / step) * step;
            float top = base - floorf(thickness * (along + 1.0f / (float)bands) / step) * step;
            Color colour = EnvironmentToward(palette->horizon, deep, along);

            DrawRectangle((int)x, (int)top, (int)step, (int)(bottom - top + step),
                          EnvironmentFade(colour, strength * 0.5f * (1.0f - along * 0.85f)));
        }
    }
    renderer->stats.sceneDrawCalls += 6u;
}

/* Resolves the palette for this frame and resets the counters; NULL when the
   view or the state cannot be drawn. */
static const EnvironmentPaletteDefinition *EnvironmentBeginFrame(
    EnvironmentRenderer *renderer, EnvironmentPaletteDefinition *resolved,
    Camera2D camera, int width, int height)
{
    renderer->stats.sceneDrawCalls = 0u;
    renderer->stats.emissiveDrawCalls = 0u;
    renderer->stats.emissiveContributors = 0u;
    renderer->stats.palette = renderer->palette;
    renderer->stats.viewValid =
        EnvironmentRendererViewIsValid(camera, width, height) &&
        EnvironmentRendererStateIsValid(renderer);
    *resolved = EnvironmentRendererResolvedPalette(renderer);
    if (!renderer->stats.viewValid ||
        EnvironmentPaletteDefinitionAt(renderer->palette) == NULL) {
        return NULL;
    }
    return resolved;
}

void EnvironmentRendererDrawSky(EnvironmentRenderer *renderer, Camera2D camera,
                                int width, int height)
{
    EnvironmentPaletteDefinition resolved;
    const EnvironmentPaletteDefinition *palette;

    if (renderer == NULL) {
        return;
    }
    palette = EnvironmentBeginFrame(renderer, &resolved, camera, width, height);
    if (palette == NULL) {
        return;
    }
    /* The environment is screen-space procedural geometry. Camera target and
       zoom drive parallax, but transient camera rotation is deliberately not
       applied as a 2D transform: the full target remains covered and shake can
       never reveal empty corners. */
    EnvironmentDrawSky(renderer, palette, camera, width, height);
}

void EnvironmentRendererDrawLandscape(EnvironmentRenderer *renderer, Camera2D camera,
                                      int width, int height)
{
    EnvironmentPaletteDefinition resolved;
    const EnvironmentPaletteDefinition *palette;

    if (renderer == NULL || !renderer->stats.viewValid) {
        return;
    }
    resolved = EnvironmentRendererResolvedPalette(renderer);
    palette = &resolved;
    /* In the sky and behind every hill: a setting sun goes down behind the
       far ridges, not in front of them. */
    EnvironmentDrawMoon(renderer, camera, width, height, 1.0f, false);
    EnvironmentDrawSun(renderer, camera, width, height, 1.0f, false);
    EnvironmentDrawLimb(renderer, palette, camera, width, height);
    EnvironmentDrawRanges(renderer, palette, camera, width, height);
    EnvironmentDrawHaze(renderer, palette, camera, width, height);
}

float EnvironmentRendererSpaceAmount(const EnvironmentRenderer *renderer)
{
    if (renderer == NULL) {
        return 0.0f;
    }
    return EnvironmentClimb(renderer);
}

void EnvironmentRendererSpaceMask(const EnvironmentRenderer *renderer, int height,
                                  float *fullY, float *clearY)
{
    /* Where EnvironmentDarkAt reaches one and where it leaves zero. */
    float climb = renderer != NULL ? EnvironmentClimb(renderer) : 0.0f;
    float full = (climb * 1.9f - 0.02f - 1.0f / 1.6f) / 1.3f;
    float clear = (climb * 1.9f - 0.02f) / 1.3f;

    *fullY = full * (float)height;
    *clearY = clear * (float)height;
}

void EnvironmentRendererDrawEmissive(EnvironmentRenderer *renderer,
                                     Camera2D camera, int width, int height)
{
    const EnvironmentPaletteDefinition *palette;
    EnvironmentProfile profile;
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
    /* The sun and the moon glow. The world is drawn over them in this plane
       as it is in the scene, so they bloom only where the sky shows. */
    EnvironmentDrawMoon(renderer, camera, width, height, 1.0f, true);
    EnvironmentDrawSun(renderer, camera, width, height, 1.0f, true);
    profile = EnvironmentRendererResolvedProfile(renderer);
    scale = EnvironmentViewScale(camera, width);
    /* Lit windows in the ruined towers, from the same geometry the scene
       drew them with. */
    for (index = 0; index < ENVIRONMENT_STRUCTURE_COUNT; ++index) {
        const EnvironmentFeature *structure = &renderer->structures[index];
        float x;
        float foot;
        float bodyWidth;
        float bodyHeight;
        float flicker = 0.68f +
                        0.32f * sinf(renderer->time * 1.35f + structure->phase);

        if (!EnvironmentTower(renderer, &profile, camera, width, height, index, &x,
                              &foot, &bodyWidth, &bodyHeight)) {
            continue;
        }
        DrawRectangle((int)(x - scale), (int)(foot - bodyHeight * (0.35f + structure->y * 0.3f)),
                      EnvironmentMaxInt(1, (int)(3.0f * scale)),
                      EnvironmentMaxInt(1, (int)(3.0f * scale)),
                      EnvironmentFade(palette->accent, 0.55f * flicker));
        ++renderer->stats.emissiveDrawCalls;
        ++renderer->stats.emissiveContributors;
    }
    /* The craters. */
    for (index = 0; index < 4; ++index) {
        float x;
        float foot;
        float halfWidth;
        float peak;
        float pulse = 0.75f + 0.25f * sinf(renderer->time * 0.9f + (float)index);

        if (!EnvironmentVolcano(renderer, &profile, camera, width, height, index, &x,
                                &foot, &halfWidth, &peak)) {
            continue;
        }
        DrawRectangle((int)(x - halfWidth * 0.1f), (int)(foot - peak * 0.93f),
                      (int)(halfWidth * 0.2f), (int)fmaxf(3.0f, 4.0f * scale),
                      EnvironmentFade(palette->accent, 0.8f * pulse));
        ++renderer->stats.emissiveDrawCalls;
        ++renderer->stats.emissiveContributors;
    }
}

const EnvironmentRendererStats *EnvironmentRendererStatistics(
    const EnvironmentRenderer *renderer)
{
    static const EnvironmentRendererStats empty = {0};

    return renderer != NULL ? &renderer->stats : &empty;
}
