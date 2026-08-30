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
    CellMaterial cursorMaterial = WorldGetCell(world, (int)cursorCell.x, (int)cursorCell.y);

    DrawRectangle(12, 12, panelWidth, panelHeight, (Color){4, 8, 15, 205});
    DrawRectangleLines(12, 12, panelWidth, panelHeight, (Color){82, 157, 208, 220});
    DrawText(TextFormat("FPS: %d", GetFPS()), 24, 23, 20, RAYWHITE);
    DrawText(TextFormat("PLAYER: %.1f, %.1f", player->position.x, player->position.y),
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
    if (powers->energy < powers->explosionEnergyCost) {
        DrawText("EXPLOSION: LOW ENERGY", 24, 133, 14, ORANGE);
    } else if (cooldown <= 0.0f) {
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

static void DrawStatusBar(int x, int y, int width, const char *label, float ratio,
                          Color fill)
{
    ratio = Clamp(ratio, 0.0f, 1.0f);
    DrawText(label, x, y, 15, RAYWHITE);
    DrawRectangle(x, y + 17, width, 13, (Color){10, 14, 21, 220});
    DrawRectangle(x + 2, y + 19, (int)((float)(width - 4) * ratio), 9, fill);
    DrawRectangleLines(x, y + 17, width, 13, (Color){183, 201, 214, 220});
}

static void DrawPlayerStatus(const Player *player, const PowerSystem *powers)
{
    const int width = 210;
    int x = GetScreenWidth() - width - 20;
    int y = 20;
    float healthRatio = player->health / player->maxHealth;
    Color healthColor = healthRatio > 0.55f ? (Color){66, 207, 113, 255}
                                           : (Color){239, 78, 65, 255};

    DrawStatusBar(x, y, width, player->alive ? "HEALTH" : "RESPAWNING",
                  healthRatio, healthColor);
    DrawStatusBar(x, y + 38, width, "ENERGY", powers->energy / powers->maxEnergy,
                  (Color){62, 164, 235, 255});
    DrawStatusBar(x, y + 76, width,
                  powers->laserOverheated ? "HEAT: OVERHEATED" : "LASER HEAT",
                  powers->laserHeat / powers->maxLaserHeat,
                  powers->laserOverheated ? (Color){255, 68, 41, 255}
                                          : (Color){244, 154, 48, 255});
    if (!player->alive) {
        DrawText(TextFormat("%.1fs", player->respawnTimer), x + width - 44, y, 15,
                 (Color){255, 181, 92, 255});
    }
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
        if (PlayerNeedsRespawn(&player)) {
            PlayerInit(&player, (Vector2){245.0f, 66.0f});
            PowersInit(&powers);
            cameraFocus = player.position;
            cameraShake = 0.0f;
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
        laserHeld = player.alive && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        explosionPressed = player.alive && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
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
        PlayerApplyWorldHazards(&player, &world);

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
        DrawPlayerStatus(&player, &powers);
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
                      !smokeExplosionObserved)) {
        fprintf(stderr,
                "Smoke test failed: reaction=%d laser_hit=%d explosion=%d\n",
                smokeReactionObserved, smokeLaserHitObserved, smokeExplosionObserved);
        exitCode = 2;
    }
    WorldUnload(&world);
    GameAudioUnload(&audio);
    CloseWindow();
    return exitCode;
}
