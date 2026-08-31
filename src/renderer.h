#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>

#include <raylib.h>

#include "game.h"
#include "world_renderer.h"

typedef struct Renderer {
    WorldRenderer world;
    RenderTexture2D sceneTarget;
    RenderTexture2D emissiveTarget;
    int targetWidth;
    int targetHeight;
    int resizeAttemptWidth;
    int resizeAttemptHeight;
} Renderer;

bool RendererInit(Renderer *renderer, const GameState *game);
void RendererRenderScene(Renderer *renderer, GameState *game, Camera2D camera,
                         Vector2 aimPosition, Rectangle visible);
void RendererComposite(const Renderer *renderer);
const WorldRendererStats *RendererWorldStats(const Renderer *renderer);
void RendererUnload(Renderer *renderer);

#endif
