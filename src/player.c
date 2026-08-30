#include "player.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

void PlayerInit(Player *player, Vector2 position)
{
    if (player == NULL) {
        return;
    }

    player->position = position;
    player->velocity = (Vector2){0.0f, 0.0f};
    player->speed = 92.0f;
    player->radius = 4.5f;
    player->facingRight = true;
}

void PlayerUpdate(Player *player, float deltaTime, int worldWidth, int worldHeight)
{
    Vector2 input = {0.0f, 0.0f};
    Vector2 targetVelocity;
    float length;
    float response;

    if (player == NULL) {
        return;
    }

    if (IsKeyDown(KEY_A)) input.x -= 1.0f;
    if (IsKeyDown(KEY_D)) input.x += 1.0f;
    if (IsKeyDown(KEY_W)) input.y -= 1.0f;
    if (IsKeyDown(KEY_S)) input.y += 1.0f;

    length = sqrtf(input.x * input.x + input.y * input.y);
    if (length > 0.0f) {
        input.x /= length;
        input.y /= length;
    }

    targetVelocity.x = input.x * player->speed;
    targetVelocity.y = input.y * player->speed;
    response = 1.0f - expf(-11.0f * deltaTime);
    player->velocity.x += (targetVelocity.x - player->velocity.x) * response;
    player->velocity.y += (targetVelocity.y - player->velocity.y) * response;

    player->position.x += player->velocity.x * deltaTime;
    player->position.y += player->velocity.y * deltaTime;
    player->position.x = Clamp(player->position.x, player->radius,
                               (float)worldWidth - player->radius);
    player->position.y = Clamp(player->position.y, player->radius,
                               (float)worldHeight - player->radius);

    if (fabsf(player->velocity.x) > 1.0f) {
        player->facingRight = player->velocity.x > 0.0f;
    }
}

void PlayerDraw(const Player *player, Vector2 aimPosition)
{
    Vector2 aim;
    float aimLength;
    Vector2 perpendicular;
    Vector2 capeTip;
    Vector2 capeTop;
    Vector2 capeBottom;
    Vector2 eye;

    if (player == NULL) {
        return;
    }

    aim = (Vector2){aimPosition.x - player->position.x,
                    aimPosition.y - player->position.y};
    aimLength = sqrtf(aim.x * aim.x + aim.y * aim.y);
    if (aimLength < 0.001f) {
        aim = (Vector2){player->facingRight ? 1.0f : -1.0f, 0.0f};
    } else {
        aim.x /= aimLength;
        aim.y /= aimLength;
    }
    perpendicular = (Vector2){-aim.y, aim.x};

    capeTop = (Vector2){player->position.x - aim.x * 2.0f + perpendicular.x * 3.0f,
                        player->position.y - aim.y * 2.0f + perpendicular.y * 3.0f};
    capeBottom = (Vector2){player->position.x - aim.x * 2.0f - perpendicular.x * 3.0f,
                           player->position.y - aim.y * 2.0f - perpendicular.y * 3.0f};
    capeTip = (Vector2){player->position.x - aim.x * 10.0f,
                        player->position.y - aim.y * 10.0f};
    DrawTriangle(capeTop, capeTip, capeBottom, (Color){190, 35, 62, 255});

    DrawCircleV(player->position, player->radius, (Color){36, 139, 214, 255});
    DrawCircleLinesV(player->position, player->radius, (Color){173, 224, 255, 255});
    eye = (Vector2){player->position.x + aim.x * 3.0f,
                    player->position.y + aim.y * 3.0f};
    DrawCircleV(eye, 1.2f, (Color){255, 241, 126, 255});
}
