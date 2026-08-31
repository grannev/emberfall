#include "renderer.h"

#include <stddef.h>

#include "ability_renderer.h"
#include "particle_renderer.h"
#include "player_renderer.h"

bool RendererInit(Renderer *renderer, const GameState *game)
{
    if (renderer == NULL || game == NULL) {
        return false;
    }
    *renderer = (Renderer){0};
    return WorldRendererInit(&renderer->world, &game->world);
}

void RendererDrawWorldSpace(Renderer *renderer, GameState *game,
                            Vector2 aimPosition, Rectangle visible)
{
    if (renderer == NULL || game == NULL) {
        return;
    }
    WorldRendererDraw(&renderer->world, &game->world, visible);
    DrawRectangleLines(0, 0, game->world.width, game->world.height,
                       (Color){74, 103, 127, 255});
    ParticleRendererDraw(&game->particles);
    PlayerRendererDraw(&game->player, aimPosition);
    AbilityRendererDraw(&game->powers, aimPosition);
}

const WorldRendererStats *RendererWorldStats(const Renderer *renderer)
{
    return renderer != NULL ? &renderer->world.lastFrame : NULL;
}

void RendererUnload(Renderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    WorldRendererUnload(&renderer->world);
    *renderer = (Renderer){0};
}
