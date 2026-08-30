#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <raylib.h>
#include <raymath.h>

#include "audio.h"
#include "particles.h"
#include "player.h"
#include "powers.h"
#include "world.h"

#define WORLD_WIDTH 512
#define WORLD_HEIGHT 288
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define VIEW_WIDTH 320.0f
#define VIEW_HEIGHT 180.0f
#define SIMULATION_STEP (1.0f / 60.0f)

static float CameraZoomForWindow(void)
{
    float horizontal = (float)GetScreenWidth() / VIEW_WIDTH;
    float vertical = (float)GetScreenHeight() / VIEW_HEIGHT;
    return fmaxf(1.0f, fminf(horizontal, vertical));
}

static Vector2 ClampCameraTarget(Vector2 target, float zoom, const World *world)
{
    float halfWidth = (float)GetScreenWidth() / (2.0f * zoom);
    float halfHeight = (float)GetScreenHeight() / (2.0f * zoom);

    if (halfWidth * 2.0f >= (float)world->width) {
        target.x = (float)world->width * 0.5f;
    } else {
        target.x = Clamp(target.x, halfWidth, (float)world->width - halfWidth);
    }
    if (halfHeight * 2.0f >= (float)world->height) {
        target.y = (float)world->height * 0.5f;
    } else {
        target.y = Clamp(target.y, halfHeight, (float)world->height - halfHeight);
    }
    return target;
}

static void DrawDebugHud(const World *world, const Player *player,
                         const PowerSystem *powers, Vector2 cursorCell)
{
    const int panelWidth = 330;
    const int panelHeight = 140;
    float cooldown = powers->explosionCooldown;
    float playerSpeed = sqrtf(player->velocity.x * player->velocity.x +
                              player->velocity.y * player->velocity.y);
    CellMaterial cursorMaterial = WorldGetCell(world, (int)cursorCell.x, (int)cursorCell.y);

    DrawRectangle(12, 12, panelWidth, panelHeight, (Color){4, 8, 15, 205});
    DrawRectangleLines(12, 12, panelWidth, panelHeight, (Color){82, 157, 208, 220});
    DrawText(TextFormat("FPS: %d", GetFPS()), 24, 23, 20, RAYWHITE);
    DrawText(TextFormat("PLAYER: %.1f, %.1f  V: %.0f", player->position.x,
                        player->position.y, playerSpeed),
             24, 47, 18, (Color){174, 219, 248, 255});
    DrawText(TextFormat("ACTIVE: %d CELLS | %d CHUNKS", world->activeCells,
                        world->activeChunkCount), 24, 69, 18,
             (Color){233, 198, 105, 255});
    DrawText(TextFormat("POWER: %s", PowersCurrentName(powers)), 24, 91, 18,
             (Color){255, 126, 86, 255});
    DrawText(TextFormat("CURSOR: %d, %d  %s  %.0fC", (int)cursorCell.x,
                        (int)cursorCell.y, WorldMaterialName(cursorMaterial),
                        WorldGetTemperature(world, (int)cursorCell.x, (int)cursorCell.y)),
             24, 113, 18, (Color){186, 194, 205, 255});
    if (cooldown <= 0.0f) {
        DrawText("EXPLOSION: READY", 24, 133, 14, LIME);
    } else {
        DrawText(TextFormat("EXPLOSION: %.2fs", cooldown), 24, 133, 14, LIGHTGRAY);
    }
}

static void DrawControlsHint(void)
{
    const char *hint = "WASD fly  |  LMB laser  |  RMB explosion  |  R regenerate  |  F1 HUD";
    int fontSize = 18;
    int width = MeasureText(hint, fontSize);
    int x = (GetScreenWidth() - width) / 2;
    int y = GetScreenHeight() - 34;

    DrawRectangle(x - 10, y - 5, width + 20, fontSize + 10, (Color){3, 6, 12, 190});
    DrawText(hint, x, y, fontSize, (Color){214, 221, 229, 255});
}

static void RunSmokePlayerProbe(World *world, bool *collisionObserved)
{
    Player probe;
    int y;

    for (y = 44; y <= 56; ++y) {
        WorldSetCell(world, 232, y, MATERIAL_ROCK);
    }
    PlayerInit(&probe, (Vector2){225.0f, 50.0f});
    probe.velocity.x = 180.0f;
    PlayerUpdate(&probe, world, 0.05f);
    *collisionObserved = probe.position.x < 229.0f && probe.velocity.x < -10.0f &&
                         probe.impactStrength > 80.0f &&
                         probe.impactNormal.x < -0.5f;
    for (y = 44; y <= 56; ++y) {
        WorldSetCell(world, 232, y, MATERIAL_EMPTY);
    }
}

static bool RunSmokeFireContainmentProbe(void)
{
    const int minimumX = 8;
    const int maximumX = 40;
    const int minimumY = 8;
    const int maximumY = 24;
    const int initialFireRadius = 1;
    const int minimumRemainingDirt = 480;
    World probe;
    int remainingDirt = 0;
    int tick;
    int y;

    if (!WorldInit(&probe, 48, 32)) {
        return false;
    }

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            WorldSetCell(&probe, x, y, MATERIAL_DIRT);
        }
    }
    for (y = 16 - initialFireRadius; y <= 16 + initialFireRadius; ++y) {
        int x;

        for (x = 24 - initialFireRadius; x <= 24 + initialFireRadius; ++x) {
            WorldSetCell(&probe, x, y, MATERIAL_FIRE);
        }
    }

    for (tick = 0; tick < 240; ++tick) {
        WorldUpdate(&probe);
    }

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            if (WorldGetCell(&probe, x, y) == MATERIAL_DIRT) {
                ++remainingDirt;
            }
        }
    }
    WorldUnload(&probe);
    return remainingDirt >= minimumRemainingDirt;
}

int main(int argc, char **argv)
{
    World world;
    Player player;
    PowerSystem powers;
    ParticleSystem particles;
    GameAudio audio;
    Camera2D camera = {0};
    float simulationAccumulator = 0.0f;
    bool debugHud = true;
    bool smokeTest = argc > 1 && strcmp(argv[1], "--smoke-test") == 0;
    int smokeFrames = 0;
    Vector2 cameraFocus;
    float cameraShake = 0.0f;
    bool smokeReactionObserved = false;
    bool smokeLaserHitObserved = false;
    bool smokeExplosionObserved = false;
    bool smokeCollisionObserved = false;
    bool smokeFireContained = false;
    int exitCode = 0;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "EMBERFALL - pixel physics sandbox");
    if (!IsWindowReady()) {
        fprintf(stderr, "Failed to initialize the raylib window.\n");
        return 1;
    }

    SetWindowMinSize(640, 360);
    SetTargetFPS(120);
    SetExitKey(KEY_ESCAPE);
    (void)GameAudioInit(&audio);

    if (!WorldInit(&world, WORLD_WIDTH, WORLD_HEIGHT)) {
        fprintf(stderr, "Failed to allocate or initialize the world.\n");
        GameAudioUnload(&audio);
        CloseWindow();
        return 1;
    }

    WorldGenerate(&world);
    if (smokeTest) {
        smokeFireContained = RunSmokeFireContainmentProbe();
        RunSmokePlayerProbe(&world, &smokeCollisionObserved);
        WorldSetCell(&world, 252, 95, MATERIAL_WATER);
        WorldSetCell(&world, 253, 95, MATERIAL_LAVA);
    }
    PlayerInit(&player, (Vector2){245.0f, 66.0f});
    PowersInit(&powers);
    ParticlesInit(&particles);
    cameraFocus = player.position;
    camera.target = cameraFocus;
    camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                              (float)GetScreenHeight() * 0.5f};
    camera.rotation = 0.0f;
    camera.zoom = CameraZoomForWindow();

    while (!WindowShouldClose()) {
        float deltaTime = fminf(GetFrameTime(), 0.05f);
        Vector2 desiredCamera;
        Vector2 cursorCell;
        Vector2 aimPosition;
        bool laserHeld;
        bool explosionPressed;
        bool materialReaction = false;

        if (IsKeyPressed(KEY_F1)) {
            debugHud = !debugHud;
        }
        if (IsKeyPressed(KEY_R)) {
            WorldGenerate(&world);
            PlayerInit(&player, (Vector2){245.0f, 66.0f});
            PowersInit(&powers);
            ParticlesInit(&particles);
            simulationAccumulator = 0.0f;
            cameraFocus = player.position;
            cameraShake = 0.0f;
        }

        PlayerUpdate(&player, &world, deltaTime);
        if (player.impactStrength >= 14.0f) {
            float impactShake = Clamp((player.impactStrength - 10.0f) * 0.025f,
                                      0.0f, 2.6f);

            cameraShake = fmaxf(cameraShake, impactShake);
            ParticlesSpawnImpact(&particles, player.impactPosition,
                                 player.impactNormal, player.impactStrength);
        }

        camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                                  (float)GetScreenHeight() * 0.5f};
        camera.zoom = CameraZoomForWindow();
        desiredCamera = ClampCameraTarget(player.position, camera.zoom, &world);
        cameraFocus.x += (desiredCamera.x - cameraFocus.x) *
                         (1.0f - expf(-8.0f * deltaTime));
        cameraFocus.y += (desiredCamera.y - cameraFocus.y) *
                         (1.0f - expf(-8.0f * deltaTime));
        cameraShake *= expf(-9.0f * deltaTime);
        if (cameraShake < 0.02f) {
            cameraShake = 0.0f;
        }
        camera.target = (Vector2){
            cameraFocus.x + ((float)GetRandomValue(-1000, 1000) / 1000.0f) * cameraShake,
            cameraFocus.y + ((float)GetRandomValue(-1000, 1000) / 1000.0f) * cameraShake
        };

        cursorCell = WorldScreenToCell(&world, GetMousePosition(), camera);
        aimPosition = (Vector2){cursorCell.x + 0.5f, cursorCell.y + 0.5f};
        laserHeld = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        explosionPressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        if (smokeTest) {
            aimPosition = (Vector2){280.5f, 116.5f};
            cursorCell = (Vector2){280.0f, 116.0f};
            laserHeld = smokeFrames >= 1 && smokeFrames <= 9;
            explosionPressed = smokeFrames == 5;
        }
        PowersUpdate(&powers, &world, &particles, player.position, aimPosition, deltaTime,
                     laserHeld, explosionPressed);
        if (smokeTest) {
            smokeLaserHitObserved = smokeLaserHitObserved || powers.laserHit;
            smokeExplosionObserved = smokeExplosionObserved || powers.explosionTriggered;
        }
        if (powers.explosionTriggered) {
            PlayerApplyExplosionImpulse(&player, powers.explosionPosition,
                                        powers.explosionShockRadius, 145.0f);
            cameraShake = 4.8f;
            GameAudioPlayExplosion(&audio);
        }
        GameAudioUpdate(&audio, powers.laserActive, deltaTime);
        ParticlesUpdate(&particles, deltaTime);

        simulationAccumulator += deltaTime;
        while (simulationAccumulator >= SIMULATION_STEP) {
            int reaction;

            WorldUpdate(&world);
            materialReaction = materialReaction || world.reactionCount > 0;
            smokeReactionObserved = smokeReactionObserved ||
                                    (smokeTest && world.reactionCount > 0);
            for (reaction = 0; reaction < world.reactionCount; ++reaction) {
                ParticlesSpawnSteam(&particles, world.reactions[reaction].position);
            }
            simulationAccumulator -= SIMULATION_STEP;
        }
        if (materialReaction) {
            GameAudioPlayReaction(&audio);
        }
        PlayerResolveWorldCollision(&player, &world);

        BeginDrawing();
        ClearBackground((Color){2, 4, 9, 255});
        BeginMode2D(camera);
            WorldDraw(&world);
            DrawRectangleLines(0, 0, world.width, world.height, (Color){74, 103, 127, 255});
            ParticlesDraw(&particles);
            PlayerDraw(&player, aimPosition);
            PowersDrawWorld(&powers, aimPosition);
        EndMode2D();

        if (debugHud) {
            DrawDebugHud(&world, &player, &powers, cursorCell);
        }
        DrawControlsHint();
        EndDrawing();

        if (smokeTest) {
            if (smokeFrames == 10) {
                TakeScreenshot("build/emberfall-smoke.png");
            }
            if (++smokeFrames >= 12) {
                break;
            }
        }
    }

    if (smokeTest && (!smokeReactionObserved || !smokeLaserHitObserved ||
                      !smokeExplosionObserved || !smokeCollisionObserved ||
                      !smokeFireContained ||
                      world.activeChunkCount <= 0 ||
                      world.activeChunkCount >= world.chunkColumns * world.chunkRows)) {
        fprintf(stderr,
                "Smoke test failed: reaction=%d laser=%d explosion=%d collision=%d "
                "fire_contained=%d chunks=%d/%d\n",
                smokeReactionObserved, smokeLaserHitObserved, smokeExplosionObserved,
                smokeCollisionObserved, smokeFireContained, world.activeChunkCount,
                world.chunkColumns * world.chunkRows);
        exitCode = 2;
    }
    WorldUnload(&world);
    GameAudioUnload(&audio);
    CloseWindow();
    return exitCode;
}
