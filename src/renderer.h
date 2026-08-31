#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>

#include <raylib.h>

#include "game.h"
#include "world_renderer.h"

typedef struct Renderer {
    WorldRenderer world;
} Renderer;

bool RendererInit(Renderer *renderer, const GameState *game);
void RendererDrawWorldSpace(Renderer *renderer, GameState *game,
                            Vector2 aimPosition, Rectangle visible);
const WorldRendererStats *RendererWorldStats(const Renderer *renderer);
void RendererUnload(Renderer *renderer);

#endif
