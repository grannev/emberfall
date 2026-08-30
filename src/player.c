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
    player->impactPosition = position;
    player->impactNormal = (Vector2){0.0f, 0.0f};
    player->acceleration = 250.0f;
    player->maxSpeed = 118.0f;
    player->drag = 1.1f;
    player->restitution = 0.34f;
    player->radius = 3.2f;
    player->impactStrength = 0.0f;
    player->impactTimer = 0.0f;
    player->facingRight = true;
    player->thrusting = false;
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

static void PlayerRecordImpact(Player *player, Vector2 normal, float strength)
{
    if (strength <= player->impactStrength) {
        return;
    }

    player->impactStrength = strength;
    player->impactNormal = normal;
    player->impactPosition = (Vector2){
        player->position.x - normal.x * player->radius,
        player->position.y - normal.y * player->radius
    };
    if (strength >= 14.0f) {
        player->impactTimer = 0.12f;
    }
}

void PlayerUpdate(Player *player, const World *world, float deltaTime)
{
    Vector2 input = {0.0f, 0.0f};
    float inputLength;
    float velocityLength;
    float damping;
    float moveX;
    float moveY;
    float stepTime;
    int moveSteps;
    int step;

    if (player == NULL || world == NULL) {
        return;
    }

    player->impactStrength = 0.0f;
    player->impactNormal = (Vector2){0.0f, 0.0f};
    player->impactTimer = fmaxf(0.0f, player->impactTimer - deltaTime);
    player->thrusting = false;

    if (IsKeyDown(KEY_A)) input.x -= 1.0f;
    if (IsKeyDown(KEY_D)) input.x += 1.0f;
    if (IsKeyDown(KEY_W)) input.y -= 1.0f;
    if (IsKeyDown(KEY_S)) input.y += 1.0f;

    inputLength = sqrtf(input.x * input.x + input.y * input.y);
    if (inputLength > 0.0f) {
        input.x /= inputLength;
        input.y /= inputLength;
        player->velocity.x += input.x * player->acceleration * deltaTime;
        player->velocity.y += input.y * player->acceleration * deltaTime;
        player->thrusting = true;
    }

    damping = expf(-player->drag * deltaTime);
    player->velocity.x *= damping;
    player->velocity.y *= damping;
    velocityLength = sqrtf(player->velocity.x * player->velocity.x +
                           player->velocity.y * player->velocity.y);
    if (velocityLength > player->maxSpeed) {
        float scale = player->maxSpeed / velocityLength;

        player->velocity.x *= scale;
        player->velocity.y *= scale;
    }

    moveX = player->velocity.x * deltaTime;
    moveY = player->velocity.y * deltaTime;
    moveSteps = (int)ceilf(fmaxf(fabsf(moveX), fabsf(moveY)) / 0.5f);
    if (moveSteps < 1) {
        moveSteps = 1;
    }
    stepTime = deltaTime / (float)moveSteps;

    for (step = 0; step < moveSteps; ++step) {
        Vector2 candidate = player->position;
        float incomingSpeed;

        candidate.x += player->velocity.x * stepTime;
        if (!PlayerCollidesAt(player, world, candidate)) {
            player->position.x = candidate.x;
        } else {
            Vector2 normal = {player->velocity.x > 0.0f ? -1.0f : 1.0f, 0.0f};

            incomingSpeed = fabsf(player->velocity.x);
            PlayerRecordImpact(player, normal, incomingSpeed);
            player->velocity.x = incomingSpeed >= 14.0f
                                     ? -player->velocity.x * player->restitution
                                     : 0.0f;
        }

        candidate = player->position;
        candidate.y += player->velocity.y * stepTime;
        if (!PlayerCollidesAt(player, world, candidate)) {
            player->position.y = candidate.y;
        } else {
            Vector2 normal = {0.0f, player->velocity.y > 0.0f ? -1.0f : 1.0f};

            incomingSpeed = fabsf(player->velocity.y);
            PlayerRecordImpact(player, normal, incomingSpeed);
            player->velocity.y = incomingSpeed >= 14.0f
                                     ? -player->velocity.y * player->restitution
                                     : 0.0f;
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
    const Color darkOutline = (Color){15, 20, 28, 255};
    const Color suit = (Color){43, 50, 61, 255};
    const Color suitLight = (Color){72, 82, 96, 255};
    const Color cape = (Color){235, 86, 31, 255};
    const Color capeShadow = (Color){143, 45, 27, 255};
    const Color skin = (Color){224, 170, 119, 255};
    const Color accent = (Color){67, 206, 218, 255};
    Color outline;
    float aimX;
    float aimY;
    bool facingRight;
    int centerX;
    int centerY;
    int tailOffsetY;
    int legFrame;
    int eyeX;
    int eyeY;

    if (player == NULL) {
        return;
    }

    aimX = aimPosition.x - player->position.x;
    aimY = aimPosition.y - player->position.y;
    facingRight = fabsf(aimX) > 0.5f ? aimX > 0.0f : player->facingRight;
    centerX = (int)floorf(player->position.x);
    centerY = (int)floorf(player->position.y);
    tailOffsetY = (int)roundf(Clamp(-player->velocity.y / 36.0f, -2.0f, 2.0f));
    legFrame = player->thrusting ? ((int)(GetTime() * 10.0) & 1) : 0;
    outline = player->impactTimer > 0.0f ? (Color){255, 179, 65, 255}
                                        : darkOutline;

    /* A small stepped cape trails the aim direction and reacts to vertical speed. */
    if (facingRight) {
        DrawRectangle(centerX - 5, centerY - 3, 4, 8, outline);
        DrawRectangle(centerX - 6, centerY + tailOffsetY, 3, 6, outline);
        DrawRectangle(centerX - 4, centerY - 2, 3, 6, cape);
        DrawRectangle(centerX - 5, centerY + 1 + tailOffsetY, 2, 4, cape);
        DrawRectangle(centerX - 5, centerY + 4 + tailOffsetY, 2, 2, capeShadow);
    } else {
        DrawRectangle(centerX + 1, centerY - 3, 4, 8, outline);
        DrawRectangle(centerX + 3, centerY + tailOffsetY, 3, 6, outline);
        DrawRectangle(centerX + 1, centerY - 2, 3, 6, cape);
        DrawRectangle(centerX + 3, centerY + 1 + tailOffsetY, 2, 4, cape);
        DrawRectangle(centerX + 3, centerY + 4 + tailOffsetY, 2, 2, capeShadow);
    }

    DrawRectangle(centerX - 2, centerY + 1, 2, 5 + legFrame, outline);
    DrawRectangle(centerX - 1, centerY + 2, 1, 3 + legFrame, suitLight);
    DrawRectangle(centerX + 1, centerY + 1 + legFrame, 2, 5 - legFrame, outline);
    DrawRectangle(centerX + 1, centerY + 2 + legFrame, 1, 3 - legFrame, suit);
    DrawRectangle(centerX - 2, centerY + 5 + legFrame, 2, 1, capeShadow);
    DrawRectangle(centerX + 1, centerY + 5, 2, 1, capeShadow);

    /* The rear arm hangs down; the front arm points toward the cursor. */
    if (facingRight) {
        DrawRectangle(centerX - 3, centerY - 2, 2, 5, outline);
        DrawRectangle(centerX - 2, centerY - 1, 1, 3, suitLight);
        DrawRectangle(centerX + 1, centerY - 2, 5, 2, outline);
        DrawRectangle(centerX + 2, centerY - 1, 3, 1, suit);
        DrawRectangle(centerX + 5, centerY - 1, 1, 1, skin);
    } else {
        DrawRectangle(centerX + 1, centerY - 2, 2, 5, outline);
        DrawRectangle(centerX + 1, centerY - 1, 1, 3, suitLight);
        DrawRectangle(centerX - 5, centerY - 2, 5, 2, outline);
        DrawRectangle(centerX - 4, centerY - 1, 3, 1, suit);
        DrawRectangle(centerX - 5, centerY - 1, 1, 1, skin);
    }

    PlayerDrawPixelBlock(centerX - 1, centerY - 3, 3, 5, suit, outline);
    DrawRectangle(centerX - 1, centerY + 1, 3, 1, capeShadow);
    DrawRectangle(centerX, centerY - 1, 1, 2, accent);

    PlayerDrawPixelBlock(centerX - 1, centerY - 7, 3, 3, suitLight, outline);
    eyeY = centerY - 6;
    if (aimY > 5.0f) {
        ++eyeY;
    } else if (aimY < -5.0f) {
        --eyeY;
    }
    eyeX = facingRight ? centerX + 1 : centerX - 1;
    DrawRectangle(eyeX, centerY - 6, 1, 2, skin);
    DrawRectangle(eyeX, eyeY, 1, 1, accent);
}
