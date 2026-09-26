#include "renderer.h"

#include <math.h>
#include <stddef.h>

#include <rlgl.h>

#include "ability_renderer.h"
#include "particle_renderer.h"
#include "player_renderer.h"
#include "presentation_fx_renderer.h"
#include "reentry_renderer.h"
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

static PlayerVisualEnvironment RendererPlayerEnvironment(const GameState *game)
{
    const Player *player = &game->player;
    PlayerVisualEnvironment environment = {0};
    float extent = PlayerExtent(player);
    float heights[3] = {-extent * 0.55f, 0.0f, extent * 0.72f};
    int sample;

    environment.heat = game->atmosphere.playerHeat;
    for (sample = 0; sample < 3; ++sample) {
        CellMaterial material = WorldGetCell(&game->world,
            (int)floorf(player->position.x),
            (int)floorf(player->position.y + heights[sample]));

        if (material == MATERIAL_WATER) environment.water += 1.0f / 3.0f;
        if (material == MATERIAL_LAVA) environment.heat = fmaxf(environment.heat, 0.8f);
    }
    return environment;
}

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
    /* A backdrop without its textures simply draws nothing: space is not a
       reason to refuse to start. */
    (void)SpaceRendererInit(&renderer->space, game->worldSeed);
    (void)BackWallDebrisInit(&renderer->backWallDebris);
    WeatherRendererInit(&renderer->weather, game->worldSeed ^ 0x3ea7u);
    if (!SkyRendererLoad(&renderer->sky)) {
        TraceLog(LOG_WARNING, "RENDER: Cloud textures unavailable; the sky has no clouds");
    }
    EnvironmentRendererInit(&renderer->environment, game->worldSeed,
                            environmentPalette);
    PresentationFxInit(&renderer->effects);
    TerrainBodyRendererInit(&renderer->terrainBodies);
    (void)RendererLoadBloomShaders(renderer);
    if (!WorldRendererInit(&renderer->world, &game->world) ||
        !LightRendererInit(&renderer->light, &game->world) ||
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
        case WORLD_BIOME_OCEAN: return ENVIRONMENT_PALETTE_ABYSSAL_BLUE;
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
    /* Blasts the plants feel: every explosion, push of force and nuclear
       strike lays the grass and the leaves over, outward, for a moment. */
    {
        int slot;
        uint16_t index;

        for (slot = 0; slot < 4; ++slot) {
            renderer->swayBlastAge[slot] += deltaTime;
            if (renderer->swayBlastAge[slot] > 1.2f) {
                renderer->swayBlasts[slot].z = 0.0f;
            }
        }
        for (index = 0; events != NULL && index < events->count; ++index) {
            const GameEvent *event = &events->events[index];
            int oldest = 0;

            if (event->type != GAME_EVENT_EXPLOSION && event->type != GAME_EVENT_FORCE &&
                event->type != GAME_EVENT_LIQUID_SPLASH) {
                continue;
            }
            for (slot = 1; slot < 4; ++slot) {
                if (renderer->swayBlastAge[slot] > renderer->swayBlastAge[oldest]) {
                    oldest = slot;
                }
            }
            renderer->swayBlasts[oldest] = (Vector4){
                event->position.x, event->position.y,
                fmaxf(40.0f, event->radius * 2.5f),
                event->type == GAME_EVENT_LIQUID_SPLASH ? 3.0f : 8.0f};
            renderer->swayBlastAge[oldest] = 0.0f;
        }
    }
    BackWallDebrisUpdate(&renderer->backWallDebris, deltaTime);
    BackWallDebrisConsumeEvents(&renderer->backWallDebris, events);
}

void RendererClearPresentation(Renderer *renderer)
{
    if (renderer == NULL) {
        return;
    }
    PresentationFxClear(&renderer->effects);
    BackWallDebrisClear(&renderer->backWallDebris);
    WeatherRendererClear(&renderer->weather);
}

void RendererShiftPresentation(Renderer *renderer, float dx)
{
    if (renderer == NULL || dx == 0.0f) {
        return;
    }
    PresentationFxShift(&renderer->effects, dx);
    BackWallDebrisShift(&renderer->backWallDebris, dx);
    WeatherRendererShift(&renderer->weather, dx);
    renderer->travel -= dx;
}

bool RendererSetEnvironmentPalette(Renderer *renderer,
                                   EnvironmentPalette palette)
{
    return renderer != NULL &&
           EnvironmentRendererSetPalette(&renderer->environment, palette);
}

/* What moves the plants this frame. Blasts fade over their first second. */
static LightSway RendererSway(const Renderer *renderer, const GameState *game)
{
    LightSway sway;
    int slot;

    sway.time = renderer->presentationTime;
    sway.wind = renderer->weather.sample.wind * renderer->weather.outdoor;
    sway.player = (Vector4){game->player.position.x,
                            game->player.position.y + PlayerExtent(&game->player) * 0.5f,
                            game->player.velocity.x, game->player.velocity.y};
    for (slot = 0; slot < 4; ++slot) {
        float fade = 1.0f - renderer->swayBlastAge[slot] / 1.2f;

        sway.blasts[slot] = renderer->swayBlasts[slot];
        sway.blasts[slot].w *= fade > 0.0f ? fade : 0.0f;
    }
    return sway;
}

void RendererRenderScene(Renderer *renderer, GameState *game,
                         Camera2D presentationCamera, Camera2D aimCamera,
                         Vector2 aimPosition, Rectangle visible)
{
    bool bloomReady;
    const PresentationFxStats *fxStats;
    const TerrainBodyRendererStats *terrainStats;
    const EnvironmentRendererStats *environmentStats;
    const LightRendererStats *lightStats;

    if (renderer == NULL || game == NULL) {
        return;
    }
    EnvironmentRendererSyncSeed(&renderer->environment, game->worldSeed);
    SkyRendererSyncSeed(&renderer->sky, game->worldSeed);
    SpaceRendererSyncSeed(&renderer->space, game->worldSeed);
    /* The weather over the view, stepped at the presentation's own rate: the
       drops, the lightning, and what the clouds show. */
    {
        float step = renderer->presentationTime - renderer->weatherTime;

        if (step < 0.0f || step > 0.25f) step = 0.0f;
        renderer->weatherTime = renderer->presentationTime;
        {
            WeatherHero hero = {
                .feet = {game->player.position.x,
                         game->player.position.y + PlayerExtent(&game->player)},
                .mouth = PlayerVisorOrigin(&game->player, aimPosition),
                .velocity = game->player.velocity,
                .grounded = game->player.grounded,
            };

            WeatherRendererUpdate(&renderer->weather, &game->weather, &game->world, hero,
                                  visible, step);
        }
        SkyRendererSetWeather(&renderer->sky, renderer->weather.sample.cloudCover,
                              renderer->weather.sample.kind == WEATHER_STORM ||
                                      renderer->weather.sample.kind == WEATHER_BLIZZARD ||
                                      renderer->weather.sample.kind == WEATHER_SANDSTORM
                                  ? renderer->weather.sample.intensity
                                  : 0.3f * renderer->weather.sample.intensity);
    }
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
    EnvironmentRendererSetDayPhase(&renderer->environment, game->dayPhase);
    EnvironmentRendererSetTravel(&renderer->environment, renderer->travel);
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

    /* The world's CPU work for the frame, before any target is bound: pages
       for what the camera sees, the light solved and uploaded for the same
       region. Neither draws anything. */
    WorldRendererPrepare(&renderer->world, &game->world, visible);
    LightRendererSync(&renderer->light, &game->world, visible);

    BeginTextureMode(renderer->sceneTarget);
    ClearBackground((Color){2, 4, 9, 255});
    /* The backdrop, far to near: the sky, gone dark from the top by however
       far the camera has climbed; space where it is dark — faintly over the
       whole sky at night; then the sun, the moon, the glow over the limb and
       the ranges bending into the curve of the planet. */
    EnvironmentRendererDrawSky(&renderer->environment, presentationCamera,
                               renderer->targetWidth, renderer->targetHeight);
    {
        float climb = EnvironmentRendererSpaceAmount(&renderer->environment);
        float night = 0.75f * (1.0f - GameDaylightAt(game->dayPhase)) * (1.0f - climb);
        float fullY;
        float clearY;

        EnvironmentRendererSpaceMask(&renderer->environment, renderer->targetHeight,
                                     &fullY, &clearY);
        SpaceRendererDraw(&renderer->space, presentationCamera, renderer->travel,
                          renderer->targetWidth, renderer->targetHeight, night,
                          (float)renderer->targetHeight * 2.0f,
                          (float)renderer->targetHeight * 2.0f + 1.0f);
        SpaceRendererDraw(&renderer->space, presentationCamera, renderer->travel,
                          renderer->targetWidth, renderer->targetHeight,
                          climb > 0.0f ? 1.0f : 0.0f, fullY, clearY);
    }
    EnvironmentRendererDrawLandscape(&renderer->environment, presentationCamera,
                                     renderer->targetWidth, renderer->targetHeight);
    BeginMode2D(presentationCamera);
        /* Between the backdrop and the terrain, and inside the camera, because
           a cloud is at an altitude rather than at a place on the screen: the
           player is meant to be able to climb above it. */
        /* In the space of the camera's travel round the planet, and moved
           back into the world's own coordinates: the clouds and stars are
           placed from the view, and the view jumps a whole width when the
           character crosses the seam, while the travel does not. */
        rlPushMatrix();
        rlTranslatef(-renderer->travel, 0.0f, 0.0f);
        SkyRendererDraw(&renderer->sky,
                        (Rectangle){visible.x + renderer->travel, visible.y,
                                    visible.width, visible.height},
                        game->world.height, GameDaylightAt(game->dayPhase),
                        renderer->weather.cloudTravel);
        rlPopMatrix();
        renderer->lastFrame.skyClouds = SkyRendererStatistics(&renderer->sky)->cloudsDrawn;
        renderer->lastFrame.spaceAmount =
            EnvironmentRendererSpaceAmount(&renderer->environment);
        /* The world and whatever was torn out of it, lit by the same field:
           a slab is as dark as the cave it is carried into. */
        LightRendererBegin(&renderer->light, &game->world, LIGHT_PASS_SCENE);
            /* Behind the world: what falls away from the back layer falls
               behind everything still standing. */
            BackWallDebrisDraw(&renderer->backWallDebris);
            WorldRendererDrawScene(&renderer->world, &game->world, visible);
            {
                LightSway sway = RendererSway(renderer, game);

                LightRendererBeginSway(&renderer->light, &sway);
                WorldRendererDrawFlora(&renderer->world, &game->world, visible);
                LightRendererEndSway(&renderer->light);
            }
            TerrainBodyRendererDrawScene(&renderer->terrainBodies,
                                         &game->dynamicTerrain, visible);
        LightRendererEnd(&renderer->light);
        DrawRectangleLines(0, 0, game->world.width, game->world.height,
                           (Color){74, 103, 127, 255});
        ParticleRendererDraw(&game->particles);
        PlayerRendererDraw(&game->player, aimPosition,
                           RendererPlayerEnvironment(game));
        ReentryRendererDrawAir(&game->player,
            WorldGravityScaleAt(&game->world, game->player.position.y),
            game->atmosphere.playerHeat, renderer->presentationTime, false);
        /* In front of the character and the bodies: the cap of glowing air
           stands ahead of whatever is burning through it. */
        ReentryRendererDraw(&game->atmosphere, &game->player,
                            &game->dynamicTerrain, visible,
                            renderer->presentationTime);
        AbilityRendererDraw(&game->abilities, &game->player,
                            renderer->presentationTime);
        /* After the player, so the beam of force reads as leaving the hand
           rather than passing behind the character. */
        TerrainGrabRendererDrawScene(&game->interaction, &game->dynamicTerrain,
                                     &game->player, renderer->presentationTime);
        PresentationFxRendererDrawScene(&renderer->effects);
        /* The weather in front of everything in the world: rain falls
           before the character as much as behind. */
        WeatherRendererDraw(&renderer->weather, visible);
    EndMode2D();
    WeatherRendererDrawOverlay(&renderer->weather, renderer->targetWidth,
                               renderer->targetHeight);
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
        /* The brighter stars bloom, in space and faintly in a night sky. */
        {
            float climb = EnvironmentRendererSpaceAmount(&renderer->environment);
            float fullY;
            float clearY;

            EnvironmentRendererSpaceMask(&renderer->environment, renderer->targetHeight,
                                         &fullY, &clearY);
            SpaceRendererDrawEmissive(
                &renderer->space, presentationCamera, renderer->travel,
                renderer->targetWidth, renderer->targetHeight,
                0.5f * (1.0f - GameDaylightAt(game->dayPhase)) * (1.0f - climb),
                (float)renderer->targetHeight * 2.0f,
                (float)renderer->targetHeight * 2.0f + 1.0f);
            SpaceRendererDrawEmissive(&renderer->space, presentationCamera,
                                      renderer->travel, renderer->targetWidth,
                                      renderer->targetHeight, climb > 0.0f ? 1.0f : 0.0f,
                                      fullY, clearY);
        }
        EnvironmentRendererDrawEmissive(&renderer->environment,
                                        presentationCamera,
                                        renderer->targetWidth,
                                        renderer->targetHeight);
        BeginMode2D(presentationCamera);
            rlPushMatrix();
            rlTranslatef(-renderer->travel, 0.0f, 0.0f);
            SkyRendererDrawEmissive(&renderer->sky,
                                    (Rectangle){visible.x + renderer->travel,
                                                visible.y, visible.width,
                                                visible.height},
                                    game->world.height,
                                    GameDaylightAt(game->dayPhase),
                                    renderer->weather.cloudTravel);
            rlPopMatrix();
            /* Occluders first. The world and the bodies are opaque black
               wherever they do not glow, and the character draws its own
               silhouette, so nothing behind any of them can bloom through. */
            LightRendererBegin(&renderer->light, &game->world,
                               LIGHT_PASS_EMISSIVE);
                WorldRendererDrawEmissive(&renderer->world, &game->world, visible);
                {
                    LightSway sway = RendererSway(renderer, game);

                    LightRendererBeginSway(&renderer->light, &sway);
                    WorldRendererDrawFlora(&renderer->world, &game->world, visible);
                    LightRendererEndSway(&renderer->light);
                }
                TerrainBodyRendererDrawEmissive(&renderer->terrainBodies,
                                                &game->dynamicTerrain, visible);
            LightRendererEnd(&renderer->light);
            PlayerRendererDrawSilhouette(&game->player, aimPosition);
            ParticleRendererDrawEmissive(&game->particles);
            PlayerRendererDrawEmissive(&game->player);
            ReentryRendererDrawAir(&game->player,
                WorldGravityScaleAt(&game->world, game->player.position.y),
                game->atmosphere.playerHeat, renderer->presentationTime, true);
            ReentryRendererDrawEmissive(&game->atmosphere, &game->player,
                                        &game->dynamicTerrain, visible,
                                        renderer->presentationTime);
            AbilityRendererDrawEmissive(&game->abilities, &game->player,
                                        renderer->presentationTime);
            TerrainGrabRendererDrawEmissive(&game->interaction,
                                            &game->dynamicTerrain,
                                            &game->player,
                                            renderer->presentationTime);
            PresentationFxRendererDrawEmissive(&renderer->effects);
            WeatherRendererDrawEmissive(&renderer->weather, visible);
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
    lightStats = LightRendererStatistics(&renderer->light);
    renderer->lastFrame.lightingEnabled = lightStats->enabled;
    renderer->lastFrame.lightUploads = lightStats->uploads;
    renderer->lastFrame.lightUploadedBytes = lightStats->uploadedBytes;
    renderer->lastFrame.lightMilliseconds = lightStats->syncMilliseconds;
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
    LightRendererUnload(&renderer->light);
    WorldRendererUnload(&renderer->world);
    SkyRendererUnload(&renderer->sky);
    SpaceRendererUnload(&renderer->space);
    BackWallDebrisUnload(&renderer->backWallDebris);
    *renderer = (Renderer){0};
}
