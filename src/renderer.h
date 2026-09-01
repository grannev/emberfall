#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "environment_renderer.h"
#include "game.h"
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
    bool bloomEnabled;
} RendererFrameStats;

typedef struct Renderer {
    WorldRenderer world;
    EnvironmentRenderer environment;
    PresentationFxSystem effects;
    TerrainBodyRenderer terrainBodies;
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
