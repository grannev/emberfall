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
