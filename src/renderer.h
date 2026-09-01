#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "game.h"
#include "presentation_fx.h"
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
    bool bloomEnabled;
} RendererFrameStats;

typedef struct Renderer {
    WorldRenderer world;
    PresentationFxSystem effects;
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

bool RendererInit(Renderer *renderer, const GameState *game);
void RendererUpdatePresentation(Renderer *renderer,
                                const GameEventBuffer *events,
                                float deltaTime);
void RendererClearPresentation(Renderer *renderer);
void RendererRenderScene(Renderer *renderer, GameState *game, Camera2D camera,
                         Vector2 aimPosition, Rectangle visible);
void RendererComposite(const Renderer *renderer);
const WorldRendererStats *RendererWorldStats(const Renderer *renderer);
const RendererFrameStats *RendererStats(const Renderer *renderer);
void RendererUnload(Renderer *renderer);

#endif
