#ifndef ENVIRONMENT_RENDERER_H
#define ENVIRONMENT_RENDERER_H

/* Renderer-owned atmospheric background.
 *
 * The descriptors below are generated from the world seed but are strictly
 * presentation state: this module never receives GameState or World and can
 * neither mutate nor influence deterministic simulation.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#define ENVIRONMENT_FAR_PEAK_COUNT 16
#define ENVIRONMENT_STRUCTURE_COUNT 8
#define ENVIRONMENT_HAZE_BAND_COUNT 5
#define ENVIRONMENT_SKY_DETAIL_COUNT 12
#define ENVIRONMENT_NEAR_SPIRE_COUNT 6

typedef enum EnvironmentPalette {
    ENVIRONMENT_PALETTE_AUTO = -1,
    ENVIRONMENT_PALETTE_EMBER_WASTE = 0,
    ENVIRONMENT_PALETTE_ABYSSAL_BLUE,
    ENVIRONMENT_PALETTE_VERDIGRIS_STORM,
    ENVIRONMENT_PALETTE_AMBER_DUNES,
    ENVIRONMENT_PALETTE_GLACIER_SHELF,
    ENVIRONMENT_PALETTE_COUNT
} EnvironmentPalette;

typedef struct EnvironmentFeature {
    float x;
    float y;
    float width;
    float height;
    float phase;
} EnvironmentFeature;

typedef struct EnvironmentPaletteDefinition {
    const char *name;
    const char *cliName;
    Color skyTop;
    Color skyBottom;
    Color horizon;
    Color farSilhouette;
    Color midSilhouette;
    Color nearSilhouette;
    Color haze;
    Color accent;
} EnvironmentPaletteDefinition;

typedef struct EnvironmentRendererStats {
    uint16_t sceneDrawCalls;
    uint16_t emissiveDrawCalls;
    uint16_t emissiveContributors;
    EnvironmentPalette palette;
    bool viewValid;
} EnvironmentRendererStats;

typedef struct EnvironmentRenderer {
    EnvironmentFeature farPeaks[ENVIRONMENT_FAR_PEAK_COUNT];
    EnvironmentFeature structures[ENVIRONMENT_STRUCTURE_COUNT];
    EnvironmentFeature hazeBands[ENVIRONMENT_HAZE_BAND_COUNT];
    EnvironmentFeature skyDetails[ENVIRONMENT_SKY_DETAIL_COUNT];
    EnvironmentFeature nearSpires[ENVIRONMENT_NEAR_SPIRE_COUNT];
    EnvironmentRendererStats stats;
    uint64_t seed;
    float time;
    EnvironmentPalette forcedPalette;
    EnvironmentPalette palette;
    /* The backdrop being left behind, and how far the crossing has got. A
       backdrop that changed the instant the player stepped over a biome
       boundary would flash a whole new sky in one frame; a boundary is a place
       the world gradually becomes something else, and the sky behind it has to
       agree. */
    EnvironmentPalette fadeFrom;
    float fade;
    /* How much daylight there is, 0..1. The backdrop is the only part of the
       picture with no cells in it, so nothing else can tell the player what
       time it is. */
    float daylight;
} EnvironmentRenderer;

bool EnvironmentPalettesValidate(void);
bool EnvironmentPaletteParse(const char *text, EnvironmentPalette *palette);
const EnvironmentPaletteDefinition *EnvironmentPaletteDefinitionAt(
    EnvironmentPalette palette);
EnvironmentPalette EnvironmentPaletteForSeed(uint64_t seed);

void EnvironmentRendererInit(EnvironmentRenderer *renderer, uint64_t seed,
                             EnvironmentPalette forcedPalette);
void EnvironmentRendererSyncSeed(EnvironmentRenderer *renderer, uint64_t seed);
bool EnvironmentRendererSetPalette(EnvironmentRenderer *renderer,
                                   EnvironmentPalette palette);
void EnvironmentRendererUpdate(EnvironmentRenderer *renderer, float deltaTime);
/* Starts a crossing to `palette`, or does nothing if that is already where the
   backdrop is heading. A crossing already under way is restarted from where it
   has got to, so crossing a boundary back and forth never snaps. */
void EnvironmentRendererFadeTo(EnvironmentRenderer *renderer,
                               EnvironmentPalette palette);
void EnvironmentRendererSetDaylight(EnvironmentRenderer *renderer,
                                    float daylight);
/* The palette actually drawn: the two sides of a crossing mixed, then taken
   toward night. Exposed so that a test can assert what the player sees rather
   than what the state says. */
EnvironmentPaletteDefinition EnvironmentRendererResolvedPalette(
    const EnvironmentRenderer *renderer);
void EnvironmentRendererDrawScene(EnvironmentRenderer *renderer,
                                  Camera2D camera, int width, int height);
void EnvironmentRendererDrawEmissive(EnvironmentRenderer *renderer,
                                     Camera2D camera, int width, int height);
Rectangle EnvironmentRendererOverscanBounds(int width, int height);
bool EnvironmentRendererViewIsValid(Camera2D camera, int width, int height);
bool EnvironmentRendererStateIsValid(const EnvironmentRenderer *renderer);
const EnvironmentRendererStats *EnvironmentRendererStatistics(
    const EnvironmentRenderer *renderer);

#endif
