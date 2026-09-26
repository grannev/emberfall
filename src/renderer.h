#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "environment_renderer.h"
#include "sky_renderer.h"
#include "space_renderer.h"
#include "backwall_debris.h"
#include "weather_renderer.h"
#include "game.h"
#include "light_renderer.h"
#include "presentation_fx.h"
#include "terrain_body_renderer.h"
#include "world_renderer.h"

typedef struct RendererFrameStats {
    double bloomSubmissionMilliseconds;
    uint32_t renderTargets;
    uint32_t offscreenPasses;
    int targetWidth;
    int targetHeight;
    int bloomWidth;
    int bloomHeight;
    uint16_t activeFx;
    uint16_t peakFx;
    uint32_t droppedFx;
    uint32_t cachedTerrainBodies;
    uint32_t visibleTerrainBodies;
    uint32_t terrainBodyDrawCalls;
    uint32_t terrainBodyTextureUpdates;
    uint64_t terrainBodyTextureMemoryBytes;
    uint16_t environmentSceneDrawCalls;
    uint16_t environmentEmissiveDrawCalls;
    uint16_t environmentEmissiveContributors;
    EnvironmentPalette environmentPalette;
    bool environmentViewValid;
    uint16_t skyClouds;
    /* How far out of the air the view is, 0..1: space's share of the
       backdrop. */
    float spaceAmount;
    bool bloomEnabled;
    bool lightingEnabled;
    uint32_t lightUploads;
    uint64_t lightUploadedBytes;
    /* CPU time of the light solve and its upload, which is the other half of
       what the world costs to keep on screen. */
    double lightMilliseconds;
} RendererFrameStats;

typedef struct Renderer {
    WorldRenderer world;
    LightRenderer light;
    EnvironmentRenderer environment;
    SpaceRenderer space;
    BackWallDebris backWallDebris;
    WeatherRenderer weather;
    /* When the weather was last stepped, in presentation seconds. */
    float weatherTime;
    /* The last few blasts, for the grass and the leaves to be flattened by:
       x, y, radius, strength — fading as they age. */
    Vector4 swayBlasts[4];
    float swayBlastAge[4];
    SkyRenderer sky;
    PresentationFxSystem effects;
    TerrainBodyRenderer terrainBodies;
    /* Seconds of presentation, for effects that flicker or turn. Kept here
       rather than read from the clock so that a run stepping at a fixed rate —
       the smoke test — draws the same frame every time. */
    float presentationTime;
    /* How far the camera has travelled around the planet beyond where it is
       now: every time the character is moved back across the seam, the
       camera moves with it and this takes up the difference. The sky and the
       backdrop are drawn from the camera's position plus this, so they
       scroll on as if nothing had been moved. */
    float travel;
    RenderTexture2D sceneTarget;
    RenderTexture2D emissiveTarget;
    RenderTexture2D bloomPingTarget;
    RenderTexture2D bloomPongTarget;
    Shader bloomDownsampleShader;
    Shader bloomBlurShader;
    int targetWidth;
    int targetHeight;
    int bloomWidth;
    int bloomHeight;
    int resizeAttemptWidth;
    int resizeAttemptHeight;
    uint16_t resizeRetryFrames;
    int downsampleSourceTexelLocation;
    int downsampleThresholdLocation;
    int blurTexelLocation;
    int blurDirectionLocation;
    int blurRadiusLocation;
    bool bloomShadersReady;
    bool bloomTargetsReady;
    RendererFrameStats lastFrame;
} Renderer;

bool RendererInit(Renderer *renderer, const GameState *game,
                  EnvironmentPalette environmentPalette);
void RendererUpdatePresentation(Renderer *renderer,
                                const GameEventBuffer *events,
                                float deltaTime);
void RendererClearPresentation(Renderer *renderer);
/* Moves every presentation effect `dx` across, with the character, when the
   game has moved everything back over the seam. */
void RendererShiftPresentation(Renderer *renderer, float dx);
bool RendererSetEnvironmentPalette(Renderer *renderer,
                                   EnvironmentPalette palette);
void RendererRenderScene(Renderer *renderer, GameState *game,
                         Camera2D presentationCamera, Camera2D aimCamera,
                         Vector2 aimPosition, Rectangle visible);
void RendererComposite(const Renderer *renderer);
const WorldRendererStats *RendererWorldStats(const Renderer *renderer);
const RendererFrameStats *RendererStats(const Renderer *renderer);
void RendererUnload(Renderer *renderer);

#endif
