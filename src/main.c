#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <raylib.h>
#include <raymath.h>

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
    DrawText(TextFormat("ACTIVE CELLS: %d", world->activeCells), 24, 69, 18,
             (Color){233, 198, 105, 255});
    DrawText(TextFormat("POWER: %s", PowersCurrentName(powers)), 24, 91, 18,
             (Color){255, 126, 86, 255});
    DrawText(TextFormat("CURSOR: %d, %d  %s", (int)cursorCell.x, (int)cursorCell.y,
                        WorldMaterialName(cursorMaterial)),
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

int main(int argc, char **argv)
{
    World world;
    Player player;
    PowerSystem powers;
    ParticleSystem particles;
    Camera2D camera = {0};
    float simulationAccumulator = 0.0f;
    bool debugHud = true;
    bool smokeTest = argc > 1 && strcmp(argv[1], "--smoke-test") == 0;
    int smokeFrames = 0;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "EMBERFALL - pixel physics sandbox");
    if (!IsWindowReady()) {
        fprintf(stderr, "Failed to initialize the raylib window.\n");
        return 1;
    }

    SetWindowMinSize(640, 360);
    SetTargetFPS(120);
    SetExitKey(KEY_ESCAPE);

    if (!WorldInit(&world, WORLD_WIDTH, WORLD_HEIGHT)) {
        fprintf(stderr, "Failed to allocate or initialize the world.\n");
        CloseWindow();
        return 1;
    }

    WorldGenerate(&world);
    PlayerInit(&player, (Vector2){245.0f, 66.0f});
    PowersInit(&powers);
    ParticlesInit(&particles);
    camera.target = player.position;
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

        if (IsKeyPressed(KEY_F1)) {
            debugHud = !debugHud;
        }
        if (IsKeyPressed(KEY_R)) {
            WorldGenerate(&world);
            PlayerInit(&player, (Vector2){245.0f, 66.0f});
            PowersInit(&powers);
            ParticlesInit(&particles);
            simulationAccumulator = 0.0f;
        }

        PlayerUpdate(&player, deltaTime, world.width, world.height);

        camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                                  (float)GetScreenHeight() * 0.5f};
        camera.zoom = CameraZoomForWindow();
        desiredCamera = ClampCameraTarget(player.position, camera.zoom, &world);
        camera.target.x += (desiredCamera.x - camera.target.x) *
                           (1.0f - expf(-8.0f * deltaTime));
        camera.target.y += (desiredCamera.y - camera.target.y) *
                           (1.0f - expf(-8.0f * deltaTime));

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
        ParticlesUpdate(&particles, deltaTime);

        simulationAccumulator += deltaTime;
        while (simulationAccumulator >= SIMULATION_STEP) {
            WorldUpdate(&world);
            simulationAccumulator -= SIMULATION_STEP;
        }

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

    WorldUnload(&world);
    CloseWindow();
    return 0;
}
