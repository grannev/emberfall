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

#define WORLD_WIDTH 1536
#define WORLD_HEIGHT 864
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720
#define VIEW_WIDTH 320.0f
#define VIEW_HEIGHT 180.0f
#define SIMULATION_STEP (1.0f / 60.0f)

/* How far the camera leads the player, as a fraction of the view in each
   direction, and over how many seconds of travel that lead is measured. At the
   boost speed cap a centred camera shows only 0.68 s of travel ahead, which is
   less than it takes to react to what the tunnel runs into. */
#define CAMERA_LOOKAHEAD_TIME 0.35f
#define CAMERA_LOOKAHEAD_VIEW_FRACTION 0.18f
/* The view also widens with speed, so the fastest flight is the one that sees
   the most. It follows measured speed rather than the boost key: a player
   thrown by an explosion gets the same widening, and a boost stalled against
   rock does not. */
#define CAMERA_FAST_VIEW_SCALE 1.28f

static float CameraZoomForWindow(float viewScale)
{
    float horizontal = (float)GetScreenWidth() / (VIEW_WIDTH * viewScale);
    float vertical = (float)GetScreenHeight() / (VIEW_HEIGHT * viewScale);
    return fmaxf(1.0f, fminf(horizontal, vertical));
}

static Vector2 CameraLookahead(Vector2 velocity)
{
    float limitX = VIEW_WIDTH * CAMERA_LOOKAHEAD_VIEW_FRACTION;
    float limitY = VIEW_HEIGHT * CAMERA_LOOKAHEAD_VIEW_FRACTION;

    return (Vector2){
        Clamp(velocity.x * CAMERA_LOOKAHEAD_TIME, -limitX, limitX),
        Clamp(velocity.y * CAMERA_LOOKAHEAD_TIME, -limitY, limitY)
    };
}

static float CameraViewScaleForSpeed(const Player *player)
{
    float speed = sqrtf(player->velocity.x * player->velocity.x +
                        player->velocity.y * player->velocity.y);
    float range = player->boostMaxSpeed - player->maxSpeed;
    float excess;

    if (range <= 0.001f) {
        return 1.0f;
    }
    excess = Clamp((speed - player->maxSpeed) / range, 0.0f, 1.0f);
    return 1.0f + (CAMERA_FAST_VIEW_SCALE - 1.0f) * excess;
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
    DrawText(TextFormat("PLAYER: %.1f, %.1f  V: %.0f%s", player->position.x,
                        player->position.y, playerSpeed,
                        player->boosting ? " BOOST" : ""),
             24, 47, 18, (Color){174, 219, 248, 255});
    DrawText(TextFormat("ACTIVE: %d CELLS | %d CHUNKS",
                        WorldCountDynamicCells(world),
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
    const char *hint =
        "WASD fly  |  Shift boost/drill  |  LMB laser  |  RMB explosion  |  R regenerate  |  F1 HUD";
    int fontSize = 18;
    int width = MeasureText(hint, fontSize);
    int x = (GetScreenWidth() - width) / 2;
    int y = GetScreenHeight() - 34;

    DrawRectangle(x - 10, y - 5, width + 20, fontSize + 10, (Color){3, 6, 12, 190});
    DrawText(hint, x, y, fontSize, (Color){214, 221, 229, 255});
}

static void RunSmokePlayerProbe(World *world, ParticleSystem *particles,
                                bool *collisionObserved, bool *drillObserved)
{
    Player probe;
    int drilledCells = 0;
    int step;
    int x;
    int y;

    for (y = 44; y <= 56; ++y) {
        WorldSetCell(world, 232, y, MATERIAL_ROCK);
    }
    PlayerInit(&probe, (Vector2){225.0f, 50.0f});
    probe.velocity.x = 180.0f;
    PlayerUpdate(&probe, world, (Vector2){0.0f, 0.0f}, false, 0.05f);
    *collisionObserved = probe.position.x < 229.0f && probe.velocity.x < -10.0f &&
                         probe.impactStrength > 80.0f &&
                         probe.impactNormal.x < -0.5f;
    for (y = 44; y <= 56; ++y) {
        WorldSetCell(world, 232, y, MATERIAL_EMPTY);
    }

    for (x = 232; x <= 242; ++x) {
        for (y = 44; y <= 56; ++y) {
            WorldSetCell(world, x, y, MATERIAL_ROCK);
        }
    }
    PlayerInit(&probe, (Vector2){212.0f, 50.0f});
    for (step = 0; step < 10; ++step) {
        PlayerUpdate(&probe, world, (Vector2){1.0f, 0.0f}, true, 0.05f);
        drilledCells += probe.drilledCells;
        /* Drive the same feedback the frame loop does, so the boost trail and
           drill debris paths stay covered without steering the live player. */
        if (probe.boostTrailEmitted) {
            ParticlesSpawnBoostTrail(particles, probe.position, probe.velocity);
        }
        if (probe.drilledCells > 0) {
            ParticlesSpawnDrillDebris(particles, probe.drillPosition,
                                      probe.velocity, probe.drilledCells);
        }
    }
    *drillObserved = probe.position.x > 242.0f && drilledCells > 20;
    for (x = 232; x <= 242; ++x) {
        for (y = 44; y <= 56; ++y) {
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
        }
    }
}

/* Guarantees the smoke run has terrain under the cursor to burn, plus a
   water/lava pair that must react, wherever the world generator put things. */
static void SetupSmokeTarget(World *world, Vector2 aim)
{
    int centerX = (int)aim.x;
    int centerY = (int)aim.y;
    int x;
    int y;

    for (y = centerY - 10; y <= centerY + 22; ++y) {
        for (x = centerX - 20; x <= centerX + 20; ++x) {
            WorldSetCell(world, x, y, MATERIAL_DIRT);
        }
    }
    for (y = centerY - 26; y < centerY - 10; ++y) {
        for (x = centerX - 20; x <= centerX + 20; ++x) {
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
        }
    }
    WorldSetCell(world, centerX - 6, centerY - 12, MATERIAL_WATER);
    WorldSetCell(world, centerX - 5, centerY - 12, MATERIAL_LAVA);
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
    Vector2 smokeAim = {0.0f, 0.0f};
    float cameraShake = 0.0f;
    float cameraViewScale = 1.0f;
    bool smokeReactionObserved = false;
    bool smokeLaserHitObserved = false;
    bool smokeExplosionObserved = false;
    bool smokeCollisionObserved = false;
    bool smokeDrillObserved = false;
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

    if (!WorldInit(&world, WORLD_WIDTH, WORLD_HEIGHT) || !WorldInitRenderer(&world)) {
        fprintf(stderr, "Failed to allocate or initialize the world.\n");
        WorldUnload(&world);
        GameAudioUnload(&audio);
        CloseWindow();
        return 1;
    }

    WorldGenerate(&world);
    PlayerInit(&player, WorldPlayerSpawn(&world));
    PowersInit(&powers);
    ParticlesInit(&particles);
    if (smokeTest) {
        smokeFireContained = RunSmokeFireContainmentProbe();
        RunSmokePlayerProbe(&world, &particles, &smokeCollisionObserved,
                            &smokeDrillObserved);
        /* The probe has exercised the spawn paths; drop what it emitted so the
           reference screenshot starts from a clean frame. */
        ParticlesInit(&particles);
        /* Build the laser/explosion target relative to the spawn instead of at
           fixed coordinates: generation is randomised, so a hardcoded point is
           not guaranteed to contain terrain. */
        smokeAim = (Vector2){player.position.x + 14.0f, player.position.y + 40.0f};
        SetupSmokeTarget(&world, smokeAim);
    }
    cameraFocus = player.position;
    camera.target = cameraFocus;
    camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                              (float)GetScreenHeight() * 0.5f};
    camera.rotation = 0.0f;
    camera.zoom = CameraZoomForWindow(cameraViewScale);

    while (!WindowShouldClose()) {
        float deltaTime = fminf(GetFrameTime(), 0.05f);
        Vector2 desiredCamera;
        Vector2 cursorCell;
        Vector2 aimPosition;
        Vector2 moveInput = {0.0f, 0.0f};
        bool laserHeld;
        bool explosionPressed;
        bool boostHeld;
        bool materialReaction = false;

        if (IsKeyPressed(KEY_F1)) {
            debugHud = !debugHud;
        }
        if (IsKeyPressed(KEY_R)) {
            WorldGenerate(&world);
            PlayerInit(&player, WorldPlayerSpawn(&world));
            PowersInit(&powers);
            ParticlesInit(&particles);
            simulationAccumulator = 0.0f;
            cameraFocus = player.position;
            cameraShake = 0.0f;
        }

        if (IsKeyDown(KEY_A)) moveInput.x -= 1.0f;
        if (IsKeyDown(KEY_D)) moveInput.x += 1.0f;
        if (IsKeyDown(KEY_W)) moveInput.y -= 1.0f;
        if (IsKeyDown(KEY_S)) moveInput.y += 1.0f;
        boostHeld = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        PlayerUpdate(&player, &world, moveInput, boostHeld, deltaTime);
        if (player.impactStrength >= 14.0f) {
            float impactShake = Clamp((player.impactStrength - 10.0f) * 0.025f,
                                      0.0f, 2.6f);

            cameraShake = fmaxf(cameraShake, impactShake);
            ParticlesSpawnImpact(&particles, player.impactPosition,
                                 player.impactNormal, player.impactStrength);
        }
        if (player.boostTrailEmitted) {
            ParticlesSpawnBoostTrail(&particles, player.position, player.velocity);
        }
        if (player.drilledCells > 0) {
            float drillShake = Clamp(0.25f + (float)player.drilledCells * 0.025f,
                                     0.0f, 1.4f);

            cameraShake = fmaxf(cameraShake, drillShake);
            ParticlesSpawnDrillDebris(&particles, player.drillPosition,
                                      player.velocity, player.drilledCells);
        }

        camera.offset = (Vector2){(float)GetScreenWidth() * 0.5f,
                                  (float)GetScreenHeight() * 0.5f};
        /* Smooth the view scale rather than the zoom so the rate does not depend
           on window size, and keep it slower than the focus so the frame breathes
           instead of snapping. */
        cameraViewScale += (CameraViewScaleForSpeed(&player) - cameraViewScale) *
                           (1.0f - expf(-4.5f * deltaTime));
        camera.zoom = CameraZoomForWindow(cameraViewScale);
        {
            Vector2 lookahead = CameraLookahead(player.velocity);
            Vector2 lead = {player.position.x + lookahead.x,
                            player.position.y + lookahead.y};

            desiredCamera = ClampCameraTarget(lead, camera.zoom, &world);
        }
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
            aimPosition = (Vector2){smokeAim.x + 0.5f, smokeAim.y + 0.5f};
            cursorCell = smokeAim;
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
        GameAudioUpdate(&audio, powers.laserActive, player.drilledCells > 0,
                        deltaTime);
        ParticlesUpdate(&particles, &world, deltaTime);

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
                      !smokeDrillObserved || !smokeFireContained ||
                      world.activeChunkCount <= 0 ||
                      world.activeChunkCount >= world.chunkColumns * world.chunkRows)) {
        fprintf(stderr,
                "Smoke test failed: reaction=%d laser=%d explosion=%d collision=%d "
                "drill=%d fire_contained=%d chunks=%d/%d\n",
                smokeReactionObserved, smokeLaserHitObserved, smokeExplosionObserved,
                smokeCollisionObserved, smokeDrillObserved, smokeFireContained,
                world.activeChunkCount, world.chunkColumns * world.chunkRows);
        exitCode = 2;
    }
    WorldUnload(&world);
    GameAudioUnload(&audio);
    CloseWindow();
    return exitCode;
}
