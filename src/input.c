#include "input.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

AppInput InputPoll(const World *world, Camera2D camera)
{
    AppInput input = {0};
    Vector2 point = GetScreenToWorld2D(GetMousePosition(), camera);

    if (world != NULL && world->width > 0 && world->height > 0) {
        point.x = Clamp(floorf(point.x), 0.0f, (float)(world->width - 1));
        point.y = Clamp(floorf(point.y), 0.0f, (float)(world->height - 1));
    }
    input.cursorCell = point;
    input.game.aimWorld = (Vector2){point.x + 0.5f, point.y + 0.5f};

    if (IsKeyDown(KEY_A)) input.game.move.x -= 1.0f;
    if (IsKeyDown(KEY_D)) input.game.move.x += 1.0f;
    if (IsKeyDown(KEY_W)) input.game.move.y -= 1.0f;
    if (IsKeyDown(KEY_S)) input.game.move.y += 1.0f;
    input.game.boostHeld = IsKeyDown(KEY_LEFT_SHIFT) ||
                           IsKeyDown(KEY_RIGHT_SHIFT);
    input.game.laserHeld = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    input.game.explosionPressed = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    input.game.forcePressed = IsKeyPressed(KEY_Q);
    input.game.chillHeld = IsKeyDown(KEY_E);
    input.game.regeneratePressed = IsKeyPressed(KEY_R);
    input.toggleDebugPressed = IsKeyPressed(KEY_F1);
    return input;
}
