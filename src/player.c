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

static bool PlayerCollidesAt(const Player *player, const World *world, Vector2 position)
{
    int minimumX = (int)floorf(position.x - player->radius);
    int maximumX = (int)floorf(position.x + player->radius);
    int minimumY = (int)floorf(position.y - player->radius);
    int maximumY = (int)floorf(position.y + player->radius);
    int y;

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            float nearestX;
            float nearestY;
            float dx;
            float dy;

            if (!WorldMaterialIsSolid(WorldGetCell(world, x, y))) {
                continue;
            }

            nearestX = Clamp(position.x, (float)x, (float)x + 1.0f);
            nearestY = Clamp(position.y, (float)y, (float)y + 1.0f);
            dx = position.x - nearestX;
            dy = position.y - nearestY;
            if (dx * dx + dy * dy < player->radius * player->radius) {
                return true;
            }
        }
    }

    return false;
}

void PlayerUpdate(Player *player, const World *world, float deltaTime)
{
    Vector2 input = {0.0f, 0.0f};
    Vector2 targetVelocity;
    float length;
    float response;
    float moveX;
    float moveY;
    int moveSteps;
    int step;

    if (player == NULL || world == NULL) {
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

    moveX = player->velocity.x * deltaTime;
    moveY = player->velocity.y * deltaTime;
    moveSteps = (int)ceilf(fmaxf(fabsf(moveX), fabsf(moveY)) / 0.75f);
    if (moveSteps < 1) {
        moveSteps = 1;
    }

    for (step = 0; step < moveSteps; ++step) {
        Vector2 candidate = player->position;

        candidate.x += moveX / (float)moveSteps;
        if (!PlayerCollidesAt(player, world, candidate)) {
            player->position.x = candidate.x;
        } else {
            player->velocity.x = 0.0f;
        }

        candidate = player->position;
        candidate.y += moveY / (float)moveSteps;
        if (!PlayerCollidesAt(player, world, candidate)) {
            player->position.y = candidate.y;
        } else {
            player->velocity.y = 0.0f;
        }
    }

    player->position.x = Clamp(player->position.x, player->radius,
                               (float)world->width - player->radius);
    player->position.y = Clamp(player->position.y, player->radius,
                               (float)world->height - player->radius);

    if (fabsf(player->velocity.x) > 1.0f) {
        player->facingRight = player->velocity.x > 0.0f;
    }
}

void PlayerResolveWorldCollision(Player *player, const World *world)
{
    Vector2 origin;
    int distance;

    if (player == NULL || world == NULL ||
        !PlayerCollidesAt(player, world, player->position)) {
        return;
    }

    origin = player->position;
    for (distance = 1; distance <= 18; ++distance) {
        int direction;

        for (direction = 0; direction < 16; ++direction) {
            float angle = (float)direction / 16.0f * 2.0f * PI;
            Vector2 candidate = {
                origin.x + cosf(angle) * (float)distance,
                origin.y + sinf(angle) * (float)distance
            };

            if (!PlayerCollidesAt(player, world, candidate)) {
                player->position = candidate;
                player->velocity = (Vector2){0.0f, 0.0f};
                return;
            }
        }
    }
}

void PlayerApplyExplosionImpulse(Player *player, Vector2 center, float radius, float force)
{
    Vector2 direction;
    float distance;
    float strength;

    if (player == NULL || radius <= 0.0f) {
        return;
    }

    direction = (Vector2){player->position.x - center.x, player->position.y - center.y};
    distance = sqrtf(direction.x * direction.x + direction.y * direction.y);
    if (distance >= radius) {
        return;
    }

    if (distance < 0.001f) {
        direction = (Vector2){0.0f, -1.0f};
        distance = 0.0f;
    } else {
        direction.x /= distance;
        direction.y /= distance;
    }

    strength = 1.0f - distance / radius;
    player->velocity.x += direction.x * force * strength;
    player->velocity.y += direction.y * force * strength;
}

static void PlayerDrawPixelBlock(int x, int y, int width, int height, Color color,
                                 Color outline)
{
    DrawRectangle(x - 1, y - 1, width + 2, height + 2, outline);
    DrawRectangle(x, y, width, height, color);
}

void PlayerDraw(const Player *player, Vector2 aimPosition)
{
    const Color outline = (Color){18, 30, 49, 255};
    const Color suit = (Color){35, 126, 203, 255};
    const Color suitLight = (Color){74, 180, 235, 255};
    const Color cape = (Color){190, 35, 62, 255};
    const Color capeShadow = (Color){126, 24, 48, 255};
    const Color skin = (Color){242, 187, 139, 255};
    const Color emblem = (Color){255, 221, 76, 255};
    const Color hair = (Color){72, 43, 34, 255};
    float aimX;
    float aimY;
    bool facingRight;
    int centerX;
    int centerY;
    int eyeX;
    int eyeY;
    int flutter;

    if (player == NULL) {
        return;
    }

    aimX = aimPosition.x - player->position.x;
    aimY = aimPosition.y - player->position.y;
    facingRight = fabsf(aimX) > 0.5f ? aimX > 0.0f : player->facingRight;
    centerX = (int)floorf(player->position.x);
    centerY = (int)floorf(player->position.y);
    flutter = ((int)(GetTime() * 6.0) & 1);

    /* The stepped cape is drawn first so the square body stays readable. */
    if (facingRight) {
        DrawRectangle(centerX - 8, centerY - 3, 6, 10, outline);
        DrawRectangle(centerX - 9, centerY + 1 + flutter, 5, 7, outline);
        DrawRectangle(centerX - 7, centerY - 2, 5, 7, cape);
        DrawRectangle(centerX - 8, centerY + 1 + flutter, 5, 5, cape);
        DrawRectangle(centerX - 7, centerY + 5 + flutter, 3, 2, capeShadow);
    } else {
        DrawRectangle(centerX + 2, centerY - 3, 6, 10, outline);
        DrawRectangle(centerX + 4, centerY + 1 + flutter, 5, 7, outline);
        DrawRectangle(centerX + 2, centerY - 2, 5, 7, cape);
        DrawRectangle(centerX + 3, centerY + 1 + flutter, 5, 5, cape);
        DrawRectangle(centerX + 4, centerY + 5 + flutter, 3, 2, capeShadow);
    }

    /* Legs and boots use whole-cell rectangles for a deliberately chunky pose. */
    PlayerDrawPixelBlock(centerX - 3, centerY + 3, 3, 5, suitLight, outline);
    PlayerDrawPixelBlock(centerX + 1, centerY + 3, 3, 5, suit, outline);
    DrawRectangle(centerX - 3, centerY + 7, 3, 2, capeShadow);
    DrawRectangle(centerX + 1, centerY + 7, 3, 2, capeShadow);

    if (facingRight) {
        PlayerDrawPixelBlock(centerX - 5, centerY - 1, 3, 6, suitLight, outline);
        PlayerDrawPixelBlock(centerX + 2, centerY - 1, 5, 3, suit, outline);
        DrawRectangle(centerX + 7, centerY, 2, 2, skin);
    } else {
        PlayerDrawPixelBlock(centerX + 2, centerY - 1, 3, 6, suitLight, outline);
        PlayerDrawPixelBlock(centerX - 7, centerY - 1, 5, 3, suit, outline);
        DrawRectangle(centerX - 9, centerY, 2, 2, skin);
    }

    PlayerDrawPixelBlock(centerX - 3, centerY - 2, 6, 7, suit, outline);
    DrawRectangle(centerX - 2, centerY + 3, 4, 1, capeShadow);
    DrawRectangle(centerX - 1, centerY, 2, 2, emblem);

    PlayerDrawPixelBlock(centerX - 3, centerY - 8, 6, 6, skin, outline);
    DrawRectangle(centerX - 3, centerY - 8, 6, 2, hair);
    eyeY = centerY - 5;
    if (aimY > 6.0f) {
        ++eyeY;
    } else if (aimY < -6.0f) {
        --eyeY;
    }
    eyeX = facingRight ? centerX + 1 : centerX - 2;
    DrawRectangle(eyeX, eyeY, 1, 1, outline);
    DrawRectangle(eyeX, eyeY - 1, 1, 1, emblem);
}
