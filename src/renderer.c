#include "renderer.h"

#include <stddef.h>

#include "ability_renderer.h"
#include "particle_renderer.h"
#include "player_renderer.h"

static bool RendererTargetIsValid(RenderTexture2D target)
{
    return target.id != 0u && target.texture.id != 0u;
}

static void RendererUnloadTarget(RenderTexture2D *target)
{
    if (target == NULL) {
        return;
    }
    if (RendererTargetIsValid(*target)) {
        UnloadRenderTexture(*target);
    }
    *target = (RenderTexture2D){0};
}

static void RendererClearTarget(RenderTexture2D target, Color color)
{
    BeginTextureMode(target);
    ClearBackground(color);
    EndTextureMode();
}

/* Creates both attachments before replacing either old one. A resize that
   cannot allocate its new pair leaves the previous frame targets usable
   instead of half-destroying presentation state. */
static bool RendererEnsureTargets(Renderer *renderer, int width, int height)
{
    RenderTexture2D scene;
    RenderTexture2D emissive;

    if (width <= 0 || height <= 0) {
        return false;
    }
    if (RendererTargetIsValid(renderer->sceneTarget) &&
        RendererTargetIsValid(renderer->emissiveTarget) &&
        renderer->targetWidth == width && renderer->targetHeight == height) {
        renderer->resizeAttemptWidth = width;
        renderer->resizeAttemptHeight = height;
        return true;
    }
    /* A failed pair allocation must not become a LoadRenderTexture loop. Try
       each observed size once and keep scaling the previous valid target until
       the window changes again. */
    if (renderer->resizeAttemptWidth == width &&
        renderer->resizeAttemptHeight == height) {
        return false;
    }
    renderer->resizeAttemptWidth = width;
    renderer->resizeAttemptHeight = height;

    scene = LoadRenderTexture(width, height);
    if (!RendererTargetIsValid(scene)) {
        return false;
    }
    emissive = LoadRenderTexture(width, height);
    if (!RendererTargetIsValid(emissive)) {
        UnloadRenderTexture(scene);
        return false;
    }

    SetTextureFilter(scene.texture, TEXTURE_FILTER_POINT);
    SetTextureFilter(emissive.texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(scene.texture, TEXTURE_WRAP_CLAMP);
    SetTextureWrap(emissive.texture, TEXTURE_WRAP_CLAMP);
    RendererClearTarget(scene, (Color){2, 4, 9, 255});
    RendererClearTarget(emissive, BLANK);

    RendererUnloadTarget(&renderer->sceneTarget);
    RendererUnloadTarget(&renderer->emissiveTarget);
    renderer->sceneTarget = scene;
    renderer->emissiveTarget = emissive;
    renderer->targetWidth = width;
    renderer->targetHeight = height;
    return true;
}

bool RendererInit(Renderer *renderer, const GameState *game)
{
    if (renderer == NULL || game == NULL) {
        return false;
    }
    *renderer = (Renderer){0};
    if (!WorldRendererInit(&renderer->world, &game->world) ||
        !RendererEnsureTargets(renderer, GetScreenWidth(), GetScreenHeight())) {
        RendererUnload(renderer);
        return false;
    }
    return true;
}

void RendererRenderScene(Renderer *renderer, GameState *game, Camera2D camera,
                         Vector2 aimPosition, Rectangle visible)
{
    if (renderer == NULL || game == NULL) {
        return;
    }
    /* Comparing dimensions every frame is cheap and catches windowed,
       fullscreen and platform-driven resize paths. Allocation only happens
       when the dimensions really changed. */
    (void)RendererEnsureTargets(renderer, GetScreenWidth(), GetScreenHeight());
    if (!RendererTargetIsValid(renderer->sceneTarget)) {
        return;
    }

    BeginTextureMode(renderer->sceneTarget);
    ClearBackground((Color){2, 4, 9, 255});
    BeginMode2D(camera);
        WorldRendererDraw(&renderer->world, &game->world, visible);
        DrawRectangleLines(0, 0, game->world.width, game->world.height,
                           (Color){74, 103, 127, 255});
        ParticleRendererDraw(&game->particles);
        PlayerRendererDraw(&game->player, aimPosition);
        AbilityRendererDraw(&game->abilities, aimPosition);
    EndMode2D();
    EndTextureMode();

    /* EF-RND-002 will draw selected contributors here. Keeping this target
       explicitly clear now makes it a stable contract without changing the
       current image or inventing a generic render graph. */
    RendererClearTarget(renderer->emissiveTarget, BLANK);
}

void RendererComposite(const Renderer *renderer)
{
    Rectangle source;
    Rectangle destination;

    if (renderer == NULL || !RendererTargetIsValid(renderer->sceneTarget)) {
        return;
    }
    /* Render textures are vertically inverted in raylib/OpenGL. A negative
       source height flips only the final composite; world and camera
       coordinates stay exactly as they were in direct-to-backbuffer drawing. */
    source = (Rectangle){0.0f, 0.0f, (float)renderer->targetWidth,
                         (float)-renderer->targetHeight};
    destination = (Rectangle){0.0f, 0.0f, (float)GetScreenWidth(),
                              (float)GetScreenHeight()};
    DrawTexturePro(renderer->sceneTarget.texture, source, destination,
                   (Vector2){0.0f, 0.0f}, 0.0f, WHITE);
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
    RendererUnloadTarget(&renderer->sceneTarget);
    RendererUnloadTarget(&renderer->emissiveTarget);
    WorldRendererUnload(&renderer->world);
    *renderer = (Renderer){0};
}
