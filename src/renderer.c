#include "renderer.h"

#include <stddef.h>

#include "ability_renderer.h"
#include "particle_renderer.h"
#include "player_renderer.h"

typedef struct BloomTuning {
    float intensity;
    float radius;
    float threshold;
    int downsampleFactor;
} BloomTuning;

/* EF-RND-002 tuning lives here rather than being scattered between shaders,
   target creation and composite code. Half resolution keeps the blur cheap
   while retaining enough shape for one-cell lava and laser contributions. */
static const BloomTuning BLOOM = {
    .intensity = 0.72f,
    .radius = 1.35f,
    .threshold = 0.08f,
    .downsampleFactor = 2,
};

#define BLOOM_DOWNSAMPLE_SHADER "assets/shaders/bloom_downsample.fs"
#define BLOOM_BLUR_SHADER "assets/shaders/bloom_blur.fs"

static bool RendererTargetIsValid(RenderTexture2D target)
{
    return target.id != 0u && target.texture.id != 0u;
}

static void RendererUnloadTarget(RenderTexture2D *target)
{
    if (target == NULL) {
        return;
    }
    if (target->id != 0u || target->texture.id != 0u) {
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

static RenderTexture2D RendererLoadTarget(int width, int height, int filter,
                                          Color clear)
{
    RenderTexture2D target = LoadRenderTexture(width, height);

    if (!RendererTargetIsValid(target)) {
        RendererUnloadTarget(&target);
        return (RenderTexture2D){0};
    }
    SetTextureFilter(target.texture, filter);
    SetTextureWrap(target.texture, TEXTURE_WRAP_CLAMP);
    RendererClearTarget(target, clear);
    return target;
}

static void RendererUnloadShader(Shader *shader)
{
    if (shader == NULL) {
        return;
    }
    if (shader->id != 0u && IsShaderValid(*shader)) {
        UnloadShader(*shader);
    }
    *shader = (Shader){0};
}

static bool RendererLoadFragmentShader(const char *path, Shader *shader)
{
    char *source = LoadFileText(path);

    if (source == NULL) {
        return false;
    }
    *shader = LoadShaderFromMemory(NULL, source);
    UnloadFileText(source);
    return IsShaderValid(*shader);
}

static bool RendererLoadBloomShaders(Renderer *renderer)
{
    if (!RendererLoadFragmentShader(BLOOM_DOWNSAMPLE_SHADER,
                                    &renderer->bloomDownsampleShader) ||
        !RendererLoadFragmentShader(BLOOM_BLUR_SHADER,
                                    &renderer->bloomBlurShader)) {
        goto fallback;
    }

    renderer->downsampleSourceTexelLocation =
        GetShaderLocation(renderer->bloomDownsampleShader, "sourceTexelSize");
    renderer->downsampleThresholdLocation =
        GetShaderLocation(renderer->bloomDownsampleShader, "threshold");
    renderer->blurTexelLocation =
        GetShaderLocation(renderer->bloomBlurShader, "texelSize");
    renderer->blurDirectionLocation =
        GetShaderLocation(renderer->bloomBlurShader, "direction");
    renderer->blurRadiusLocation =
        GetShaderLocation(renderer->bloomBlurShader, "radius");
    if (renderer->downsampleSourceTexelLocation < 0 ||
        renderer->downsampleThresholdLocation < 0 ||
        renderer->blurTexelLocation < 0 ||
        renderer->blurDirectionLocation < 0 ||
        renderer->blurRadiusLocation < 0) {
        goto fallback;
    }

    renderer->bloomShadersReady = true;
    return true;

fallback:
    RendererUnloadShader(&renderer->bloomDownsampleShader);
    RendererUnloadShader(&renderer->bloomBlurShader);
    renderer->bloomShadersReady = false;
    TraceLog(LOG_WARNING,
             "RENDER: Bloom shaders unavailable; using sharp scene fallback");
    return false;
}

static bool RendererBloomReady(const Renderer *renderer)
{
    return renderer->bloomShadersReady && renderer->bloomTargetsReady &&
           RendererTargetIsValid(renderer->bloomPingTarget) &&
           RendererTargetIsValid(renderer->bloomPongTarget);
}

/* Creates the full-resolution pair before replacing either old attachment. A
   bloom allocation failure degrades to the sharp scene rather than making the
   whole renderer fail; a full-resolution failure keeps the previous pair. */
static bool RendererEnsureTargets(Renderer *renderer, int width, int height)
{
    RenderTexture2D scene = {0};
    RenderTexture2D emissive = {0};
    RenderTexture2D bloomPing = {0};
    RenderTexture2D bloomPong = {0};
    int bloomWidth;
    int bloomHeight;
    bool bloomTargetsReady = false;

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

    scene = RendererLoadTarget(width, height, TEXTURE_FILTER_POINT,
                               (Color){2, 4, 9, 255});
    if (!RendererTargetIsValid(scene)) {
        return false;
    }
    emissive = RendererLoadTarget(width, height, TEXTURE_FILTER_POINT, BLANK);
    if (!RendererTargetIsValid(emissive)) {
        RendererUnloadTarget(&scene);
        return false;
    }

    bloomWidth = (width + BLOOM.downsampleFactor - 1) / BLOOM.downsampleFactor;
    bloomHeight = (height + BLOOM.downsampleFactor - 1) /
                  BLOOM.downsampleFactor;
    if (renderer->bloomShadersReady) {
        bloomPing = RendererLoadTarget(bloomWidth, bloomHeight,
                                       TEXTURE_FILTER_BILINEAR, BLACK);
        bloomPong = RendererLoadTarget(bloomWidth, bloomHeight,
                                       TEXTURE_FILTER_BILINEAR, BLACK);
        bloomTargetsReady = RendererTargetIsValid(bloomPing) &&
                            RendererTargetIsValid(bloomPong);
        if (!bloomTargetsReady) {
            RendererUnloadTarget(&bloomPing);
            RendererUnloadTarget(&bloomPong);
            TraceLog(LOG_WARNING,
                     "RENDER: Bloom targets unavailable at %dx%d; using fallback",
                     bloomWidth, bloomHeight);
        }
    }

    RendererUnloadTarget(&renderer->sceneTarget);
    RendererUnloadTarget(&renderer->emissiveTarget);
    RendererUnloadTarget(&renderer->bloomPingTarget);
    RendererUnloadTarget(&renderer->bloomPongTarget);
    renderer->sceneTarget = scene;
    renderer->emissiveTarget = emissive;
    renderer->bloomPingTarget = bloomPing;
    renderer->bloomPongTarget = bloomPong;
    renderer->targetWidth = width;
    renderer->targetHeight = height;
    renderer->bloomWidth = bloomTargetsReady ? bloomWidth : 0;
    renderer->bloomHeight = bloomTargetsReady ? bloomHeight : 0;
    renderer->bloomTargetsReady = bloomTargetsReady;
    return true;
}

static void RendererDrawTarget(RenderTexture2D target, int destinationWidth,
                               int destinationHeight, Color tint)
{
    Rectangle source = {0.0f, 0.0f, (float)target.texture.width,
                        (float)-target.texture.height};
    Rectangle destination = {0.0f, 0.0f, (float)destinationWidth,
                             (float)destinationHeight};

    DrawTexturePro(target.texture, source, destination,
                   (Vector2){0.0f, 0.0f}, 0.0f, tint);
}

static void RendererFilterBloom(Renderer *renderer)
{
    Vector2 sourceTexel = {1.0f / (float)renderer->targetWidth,
                           1.0f / (float)renderer->targetHeight};
    Vector2 bloomTexel = {1.0f / (float)renderer->bloomWidth,
                          1.0f / (float)renderer->bloomHeight};
    Vector2 horizontal = {1.0f, 0.0f};
    Vector2 vertical = {0.0f, 1.0f};

    SetShaderValue(renderer->bloomDownsampleShader,
                   renderer->downsampleSourceTexelLocation, &sourceTexel,
                   SHADER_UNIFORM_VEC2);
    SetShaderValue(renderer->bloomDownsampleShader,
                   renderer->downsampleThresholdLocation, &BLOOM.threshold,
                   SHADER_UNIFORM_FLOAT);
    BeginTextureMode(renderer->bloomPingTarget);
    ClearBackground(BLACK);
    BeginShaderMode(renderer->bloomDownsampleShader);
    RendererDrawTarget(renderer->emissiveTarget, renderer->bloomWidth,
                       renderer->bloomHeight, WHITE);
    EndShaderMode();
    EndTextureMode();

    SetShaderValue(renderer->bloomBlurShader, renderer->blurTexelLocation,
                   &bloomTexel, SHADER_UNIFORM_VEC2);
    SetShaderValue(renderer->bloomBlurShader, renderer->blurRadiusLocation,
                   &BLOOM.radius, SHADER_UNIFORM_FLOAT);
    SetShaderValue(renderer->bloomBlurShader, renderer->blurDirectionLocation,
                   &horizontal, SHADER_UNIFORM_VEC2);
    BeginTextureMode(renderer->bloomPongTarget);
    ClearBackground(BLACK);
    BeginShaderMode(renderer->bloomBlurShader);
    RendererDrawTarget(renderer->bloomPingTarget, renderer->bloomWidth,
                       renderer->bloomHeight, WHITE);
    EndShaderMode();
    EndTextureMode();

    SetShaderValue(renderer->bloomBlurShader, renderer->blurDirectionLocation,
                   &vertical, SHADER_UNIFORM_VEC2);
    BeginTextureMode(renderer->bloomPingTarget);
    ClearBackground(BLACK);
    BeginShaderMode(renderer->bloomBlurShader);
    RendererDrawTarget(renderer->bloomPongTarget, renderer->bloomWidth,
                       renderer->bloomHeight, WHITE);
    EndShaderMode();
    EndTextureMode();
}

bool RendererInit(Renderer *renderer, const GameState *game)
{
    if (renderer == NULL || game == NULL) {
        return false;
    }
    *renderer = (Renderer){0};
    (void)RendererLoadBloomShaders(renderer);
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
    bool bloomReady;

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
    bloomReady = RendererBloomReady(renderer);
    renderer->lastFrame = (RendererFrameStats){
        .renderTargets = bloomReady ? 4u : 2u,
        .offscreenPasses = bloomReady ? 5u : 1u,
        .bloomWidth = bloomReady ? renderer->bloomWidth : 0,
        .bloomHeight = bloomReady ? renderer->bloomHeight : 0,
        .bloomEnabled = bloomReady,
    };

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

    if (bloomReady) {
        double started = GetTime();

        BeginTextureMode(renderer->emissiveTarget);
        ClearBackground(BLANK);
        BeginMode2D(camera);
            WorldRendererDrawEmissive(&renderer->world, &game->world, visible);
            ParticleRendererDrawEmissive(&game->particles);
            PlayerRendererDrawEmissive(&game->player);
            AbilityRendererDrawEmissive(&game->abilities);
        EndMode2D();
        EndTextureMode();

        RendererFilterBloom(renderer);
        renderer->lastFrame.bloomSubmissionMilliseconds =
            (GetTime() - started) * 1000.0;
    }
}

void RendererComposite(const Renderer *renderer)
{
    if (renderer == NULL || !RendererTargetIsValid(renderer->sceneTarget)) {
        return;
    }
    /* Render textures are vertically inverted in raylib/OpenGL. A negative
       source height flips only the final composite; world and camera
       coordinates stay exactly as they were in direct-to-backbuffer drawing. */
    RendererDrawTarget(renderer->sceneTarget, GetScreenWidth(), GetScreenHeight(),
                       WHITE);
    if (RendererBloomReady(renderer)) {
        BeginBlendMode(BLEND_ADDITIVE);
        RendererDrawTarget(renderer->bloomPingTarget, GetScreenWidth(),
                           GetScreenHeight(), Fade(WHITE, BLOOM.intensity));
        EndBlendMode();
    }
}

const WorldRendererStats *RendererWorldStats(const Renderer *renderer)
{
    return renderer != NULL ? &renderer->world.lastFrame : NULL;
}

const RendererFrameStats *RendererStats(const Renderer *renderer)
{
    return renderer != NULL ? &renderer->lastFrame : NULL;
}

void RendererUnload(Renderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    RendererUnloadTarget(&renderer->sceneTarget);
    RendererUnloadTarget(&renderer->emissiveTarget);
    RendererUnloadTarget(&renderer->bloomPingTarget);
    RendererUnloadTarget(&renderer->bloomPongTarget);
    RendererUnloadShader(&renderer->bloomDownsampleShader);
    RendererUnloadShader(&renderer->bloomBlurShader);
    WorldRendererUnload(&renderer->world);
    *renderer = (Renderer){0};
}
