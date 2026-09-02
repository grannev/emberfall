#include "renderer.h"

#include <stddef.h>

#include "ability_renderer.h"
#include "particle_renderer.h"
#include "player_renderer.h"
#include "presentation_fx_renderer.h"
#include "terrain_grab_renderer.h"

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
#define RENDERER_RESIZE_RETRY_FRAMES 120u

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
    if (!RendererTargetIsValid(target)) {
        return;
    }
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
    bool targetsMatch;
    bool bloomMissing;

    if (width <= 0 || height <= 0) {
        return false;
    }
    targetsMatch = RendererTargetIsValid(renderer->sceneTarget) &&
                   RendererTargetIsValid(renderer->emissiveTarget) &&
                   renderer->targetWidth == width &&
                   renderer->targetHeight == height;
    bloomMissing = renderer->bloomShadersReady &&
                   (!RendererTargetIsValid(renderer->bloomPingTarget) ||
                    !RendererTargetIsValid(renderer->bloomPongTarget));
    if (targetsMatch && !bloomMissing) {
        renderer->resizeAttemptWidth = width;
        renderer->resizeAttemptHeight = height;
        renderer->resizeRetryFrames = 0u;
        return true;
    }
    /* A failed allocation must not become a LoadRenderTexture loop. Keep the
       previous valid scene and retry only after a long quiet interval, or
       immediately when the actual window size changes. */
    if (renderer->resizeAttemptWidth == width &&
        renderer->resizeAttemptHeight == height &&
        renderer->resizeRetryFrames > 0u) {
        --renderer->resizeRetryFrames;
        return targetsMatch;
    }
    renderer->resizeAttemptWidth = width;
    renderer->resizeAttemptHeight = height;

    scene = RendererLoadTarget(width, height, TEXTURE_FILTER_POINT,
                               (Color){2, 4, 9, 255});
    if (!RendererTargetIsValid(scene)) {
        renderer->resizeRetryFrames = RENDERER_RESIZE_RETRY_FRAMES;
        TraceLog(LOG_WARNING,
                 "RENDER: Scene target resize to %dx%d failed; retrying later",
                 width, height);
        return false;
    }
    emissive = RendererLoadTarget(width, height, TEXTURE_FILTER_POINT, BLANK);
    if (!RendererTargetIsValid(emissive)) {
        RendererUnloadTarget(&scene);
        renderer->resizeRetryFrames = RENDERER_RESIZE_RETRY_FRAMES;
        TraceLog(LOG_WARNING,
                 "RENDER: Emissive target resize to %dx%d failed; retrying later",
                 width, height);
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
            renderer->resizeRetryFrames = RENDERER_RESIZE_RETRY_FRAMES;
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
    if (bloomTargetsReady || !renderer->bloomShadersReady) {
        renderer->resizeRetryFrames = 0u;
    }
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

bool RendererInit(Renderer *renderer, const GameState *game,
                  EnvironmentPalette environmentPalette)
{
    if (renderer == NULL || game == NULL) {
        return false;
    }
    *renderer = (Renderer){0};
    SkyRendererInit(&renderer->sky, game->worldSeed);
    EnvironmentRendererInit(&renderer->environment, game->worldSeed,
                            environmentPalette);
    PresentationFxInit(&renderer->effects);
    TerrainBodyRendererInit(&renderer->terrainBodies);
    (void)RendererLoadBloomShaders(renderer);
    if (!WorldRendererInit(&renderer->world, &game->world) ||
        !RendererEnsureTargets(renderer, GetScreenWidth(), GetScreenHeight())) {
        RendererUnload(renderer);
        return false;
    }
    return true;
}

/* Which backdrop belongs to which ground. Not a property of either module —
   the world does not know what it looks like from a distance, and the
   environment does not know what biome is — so the mapping lives in the one
   place that sees both. */
static EnvironmentPalette RendererPaletteForBiome(WorldBiome biome)
{
    switch (biome) {
        case WORLD_BIOME_TEMPERATE: return ENVIRONMENT_PALETTE_VERDIGRIS_STORM;
        case WORLD_BIOME_DUNES: return ENVIRONMENT_PALETTE_AMBER_DUNES;
        case WORLD_BIOME_FROST: return ENVIRONMENT_PALETTE_GLACIER_SHELF;
        case WORLD_BIOME_VOLCANIC: return ENVIRONMENT_PALETTE_EMBER_WASTE;
        case WORLD_BIOME_COUNT: break;
    }
    return ENVIRONMENT_PALETTE_ABYSSAL_BLUE;
}

void RendererUpdatePresentation(Renderer *renderer,
                                const GameEventBuffer *events,
                                float deltaTime)
{
    if (renderer == NULL) {
        return;
    }
    if (deltaTime > 0.0f) {
        renderer->presentationTime += deltaTime;
    }
    /* Existing instances age before this frame's events are consumed, so a
       newly spawned flash is presented once at full intensity. */
    EnvironmentRendererUpdate(&renderer->environment, deltaTime);
    PresentationFxUpdate(&renderer->effects, deltaTime);
    (void)PresentationFxConsumeEvents(&renderer->effects, events);
}

void RendererClearPresentation(Renderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    PresentationFxClear(&renderer->effects);
}

bool RendererSetEnvironmentPalette(Renderer *renderer,
                                   EnvironmentPalette palette)
{
    return renderer != NULL &&
           EnvironmentRendererSetPalette(&renderer->environment, palette);
}

void RendererRenderScene(Renderer *renderer, GameState *game,
                         Camera2D presentationCamera, Camera2D aimCamera,
                         Vector2 aimPosition, Rectangle visible)
{
    bool bloomReady;
    const PresentationFxStats *fxStats;
    const TerrainBodyRendererStats *terrainStats;
    const EnvironmentRendererStats *environmentStats;

    if (renderer == NULL || game == NULL) {
        return;
    }
    EnvironmentRendererSyncSeed(&renderer->environment, game->worldSeed);
    SkyRendererSyncSeed(&renderer->sky, game->worldSeed);
    /* Comparing dimensions every frame is cheap and catches windowed,
       fullscreen and platform-driven resize paths. Allocation only happens
       when the dimensions really changed. */
    (void)RendererEnsureTargets(renderer, GetScreenWidth(), GetScreenHeight());
    if (!RendererTargetIsValid(renderer->sceneTarget)) {
        return;
    }
    bloomReady = RendererBloomReady(renderer);
    fxStats = PresentationFxGetStats(&renderer->effects);
    renderer->lastFrame = (RendererFrameStats){
        .renderTargets = bloomReady ? 4u : 2u,
        .offscreenPasses = bloomReady ? 5u : 1u,
        .targetWidth = renderer->targetWidth,
        .targetHeight = renderer->targetHeight,
        .bloomWidth = bloomReady ? renderer->bloomWidth : 0,
        .bloomHeight = bloomReady ? renderer->bloomHeight : 0,
        .activeFx = fxStats->active,
        .peakFx = fxStats->peak,
        .droppedFx = fxStats->dropped,
        .bloomEnabled = bloomReady,
    };

    /* The backdrop belongs to the biome the player is standing in and to the
       time of day, and this is the only place that can see all three. The
       environment module still never receives a World: it is told which
       backdrop and how much daylight, not where to look them up. */
    EnvironmentRendererFadeTo(
        &renderer->environment,
        RendererPaletteForBiome(WorldBiomeAt(&game->world,
                                             (int)game->player.position.x)));
    EnvironmentRendererSetDaylight(&renderer->environment,
                                   GameDaylightAt(game->dayPhase));
    /* Full backdrop at and below the clouds, none at and above the space line.
       Asked here because the environment renderer is never given a World and
       could not work it out; the answer itself belongs to the world, beside the
       gravity that fades across the same band.

       The player's altitude, not the camera's. Near the top of the world the
       camera cannot centre on the character at all — it is held inside the
       world's bounds — so its target says the view is lower than the character
       is, and the horizon would never fully go away. */
    EnvironmentRendererSetAltitude(
        &renderer->environment,
        WorldAirFractionAt(&game->world, game->player.position.y));

    BeginTextureMode(renderer->sceneTarget);
    ClearBackground((Color){2, 4, 9, 255});
    EnvironmentRendererDrawScene(&renderer->environment, presentationCamera,
                                 renderer->targetWidth,
                                 renderer->targetHeight);
    BeginMode2D(presentationCamera);
        /* Between the backdrop and the terrain, and inside the camera, because
           a cloud is at an altitude rather than at a place on the screen: the
           player is meant to be able to climb above it. */
        SkyRendererDraw(&renderer->sky, visible, game->world.height,
                        GameDaylightAt(game->dayPhase),
                        renderer->presentationTime);
        WorldRendererDraw(&renderer->world, &game->world, visible);
        renderer->lastFrame.skyClouds = SkyRendererStatistics(&renderer->sky)->cloudsDrawn;
        renderer->lastFrame.skyStars = SkyRendererStatistics(&renderer->sky)->starsDrawn;
        renderer->lastFrame.skySpaceVisible =
            SkyRendererStatistics(&renderer->sky)->spaceVisible;
        TerrainBodyRendererDrawScene(&renderer->terrainBodies,
                                     &game->dynamicTerrain, visible);
        DrawRectangleLines(0, 0, game->world.width, game->world.height,
                           (Color){74, 103, 127, 255});
        ParticleRendererDraw(&game->particles);
        PlayerRendererDraw(&game->player, aimPosition);
        AbilityRendererDraw(&game->abilities, &game->player,
                            renderer->presentationTime);
        /* After the player, so the beam of force reads as leaving the hand
           rather than passing behind the character. */
        TerrainGrabRendererDrawScene(&game->interaction, &game->dynamicTerrain,
                                     &game->player, renderer->presentationTime);
        PresentationFxRendererDrawScene(&renderer->effects);
    EndMode2D();
    /* The reticle uses exactly the stable transform that converted the mouse
       into aimWorld. Transient shake may move the presented world beneath the
       cursor, but it can never feed back into or visually displace aiming. */
    BeginMode2D(aimCamera);
        AbilityRendererDrawReticle(&game->abilities, aimPosition);
    EndMode2D();
    EndTextureMode();

    if (bloomReady) {
        double started = GetTime();

        BeginTextureMode(renderer->emissiveTarget);
        ClearBackground(BLANK);
        EnvironmentRendererDrawEmissive(&renderer->environment,
                                        presentationCamera,
                                        renderer->targetWidth,
                                        renderer->targetHeight);
        BeginMode2D(presentationCamera);
            SkyRendererDrawEmissive(&renderer->sky, visible, game->world.height,
                                    GameDaylightAt(game->dayPhase),
                                    renderer->presentationTime);
            WorldRendererDrawEmissive(&renderer->world, &game->world, visible);
            TerrainBodyRendererDrawEmissive(&renderer->terrainBodies,
                                            &game->dynamicTerrain, visible);
            ParticleRendererDrawEmissive(&game->particles);
            PlayerRendererDrawEmissive(&game->player);
            AbilityRendererDrawEmissive(&game->abilities, &game->player,
                                        renderer->presentationTime);
            TerrainGrabRendererDrawEmissive(&game->interaction,
                                            &game->dynamicTerrain,
                                            &game->player,
                                            renderer->presentationTime);
            PresentationFxRendererDrawEmissive(&renderer->effects);
        EndMode2D();
        EndTextureMode();

        RendererFilterBloom(renderer);
        renderer->lastFrame.bloomSubmissionMilliseconds =
            (GetTime() - started) * 1000.0;
    }

    terrainStats = TerrainBodyRendererStatistics(&renderer->terrainBodies);
    renderer->lastFrame.cachedTerrainBodies = terrainStats->cachedBodies;
    renderer->lastFrame.visibleTerrainBodies = terrainStats->visibleBodies;
    renderer->lastFrame.terrainBodyDrawCalls = terrainStats->drawCalls;
    renderer->lastFrame.terrainBodyTextureUpdates = terrainStats->textureUpdates;
    renderer->lastFrame.terrainBodyTextureMemoryBytes =
        terrainStats->textureMemoryBytes;
    environmentStats =
        EnvironmentRendererStatistics(&renderer->environment);
    renderer->lastFrame.environmentSceneDrawCalls =
        environmentStats->sceneDrawCalls;
    renderer->lastFrame.environmentEmissiveDrawCalls =
        environmentStats->emissiveDrawCalls;
    renderer->lastFrame.environmentEmissiveContributors =
        environmentStats->emissiveContributors;
    renderer->lastFrame.environmentPalette = environmentStats->palette;
    renderer->lastFrame.environmentViewValid = environmentStats->viewValid;
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
    TerrainBodyRendererUnload(&renderer->terrainBodies);
    WorldRendererUnload(&renderer->world);
    *renderer = (Renderer){0};
}
