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

static Vector2 PlayerLocalPoint(Vector2 origin, Vector2 forward, Vector2 side,
                                float forwardOffset, float sideOffset)
{
    return (Vector2){origin.x + forward.x * forwardOffset + side.x * sideOffset,
                     origin.y + forward.y * forwardOffset + side.y * sideOffset};
}

static void PlayerDrawLimb(Vector2 start, Vector2 end, float width, Color color,
                           Color outline)
{
    DrawLineEx(start, end, width + 1.4f, outline);
    DrawLineEx(start, end, width, color);
}

static void PlayerDrawQuad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color color,
                           Color outline)
{
    DrawTriangle(a, b, c, color);
    DrawTriangle(a, c, d, color);
    DrawLineEx(a, b, 0.8f, outline);
    DrawLineEx(b, c, 0.8f, outline);
    DrawLineEx(c, d, 0.8f, outline);
    DrawLineEx(d, a, 0.8f, outline);
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
    Vector2 forward;
    Vector2 side;
    Vector2 capeUpper;
    Vector2 capeLower;
    Vector2 capeTailUpper;
    Vector2 capeTailMiddle;
    Vector2 capeTailLower;
    Vector2 rearFoot;
    Vector2 frontFoot;
    Vector2 rearHip;
    Vector2 frontHip;
    Vector2 rearShoulder;
    Vector2 frontShoulder;
    Vector2 rearHand;
    Vector2 frontHand;
    Vector2 torsoUpperRear;
    Vector2 torsoUpperFront;
    Vector2 torsoLowerFront;
    Vector2 torsoLowerRear;
    Vector2 head;
    Vector2 face;
    Vector2 visorStart;
    Vector2 visorEnd;
    Vector2 chest;
    float forwardLength;
    float flutter;

    if (player == NULL) {
        return;
    }

    forward = (Vector2){aimPosition.x - player->position.x,
                        aimPosition.y - player->position.y};
    forwardLength = sqrtf(forward.x * forward.x + forward.y * forward.y);
    if (forwardLength < 0.001f) {
        forward = (Vector2){player->facingRight ? 1.0f : -1.0f, 0.0f};
    } else {
        forward.x /= forwardLength;
        forward.y /= forwardLength;
    }
    side = (Vector2){-forward.y, forward.x};
    flutter = sinf((float)GetTime() * 7.0f + player->position.x * 0.08f) * 1.1f;

    capeUpper = PlayerLocalPoint(player->position, forward, side, 0.2f, 2.8f);
    capeLower = PlayerLocalPoint(player->position, forward, side, 0.2f, -2.8f);
    capeTailUpper = PlayerLocalPoint(player->position, forward, side, -9.8f,
                                     4.3f + flutter);
    capeTailMiddle = PlayerLocalPoint(player->position, forward, side, -11.5f,
                                      0.4f + flutter * 0.7f);
    capeTailLower = PlayerLocalPoint(player->position, forward, side, -9.2f,
                                     -4.1f + flutter * 0.35f);
    DrawTriangle(capeUpper, capeTailUpper, capeTailMiddle, cape);
    DrawTriangle(capeUpper, capeTailMiddle, capeLower, cape);
    DrawTriangle(capeLower, capeTailMiddle, capeTailLower, capeShadow);
    DrawLineEx(capeUpper, capeTailUpper, 0.9f, outline);
    DrawLineEx(capeTailUpper, capeTailMiddle, 0.9f, outline);
    DrawLineEx(capeTailMiddle, capeTailLower, 0.9f, outline);
    DrawLineEx(capeTailLower, capeLower, 0.9f, outline);

    rearHip = PlayerLocalPoint(player->position, forward, side, -3.0f, 1.1f);
    frontHip = PlayerLocalPoint(player->position, forward, side, -3.0f, -1.1f);
    rearFoot = PlayerLocalPoint(player->position, forward, side, -8.5f,
                                2.0f + flutter * 0.2f);
    frontFoot = PlayerLocalPoint(player->position, forward, side, -9.0f,
                                 -2.0f + flutter * 0.12f);
    PlayerDrawLimb(rearHip, rearFoot, 2.1f, suitLight, outline);
    PlayerDrawLimb(frontHip, frontFoot, 2.3f, suit, outline);
    DrawCircleV(rearFoot, 1.25f, outline);
    DrawCircleV(frontFoot, 1.35f, outline);
    DrawLineEx(rearFoot,
               PlayerLocalPoint(rearFoot, forward, side, 1.2f, 0.0f), 1.3f,
               capeShadow);
    DrawLineEx(frontFoot,
               PlayerLocalPoint(frontFoot, forward, side, 1.3f, 0.0f), 1.4f,
               capeShadow);

    rearShoulder = PlayerLocalPoint(player->position, forward, side, 0.8f, 2.4f);
    frontShoulder = PlayerLocalPoint(player->position, forward, side, 0.8f, -2.4f);
    rearHand = PlayerLocalPoint(player->position, forward, side, 7.2f, 2.0f);
    frontHand = PlayerLocalPoint(player->position, forward, side, 8.6f, -1.2f);
    PlayerDrawLimb(rearShoulder, rearHand, 1.8f, suitLight, outline);
    PlayerDrawLimb(frontShoulder, frontHand, 2.0f, suit, outline);

    torsoUpperRear = PlayerLocalPoint(player->position, forward, side, 1.1f, 2.5f);
    torsoUpperFront = PlayerLocalPoint(player->position, forward, side, 1.1f, -2.5f);
    torsoLowerFront = PlayerLocalPoint(player->position, forward, side, -3.2f, -1.5f);
    torsoLowerRear = PlayerLocalPoint(player->position, forward, side, -3.2f, 1.5f);
    PlayerDrawQuad(torsoUpperRear, torsoUpperFront, torsoLowerFront,
                   torsoLowerRear, suit, outline);

    DrawCircleV(rearHand, 1.15f, outline);
    DrawCircleV(rearHand, 0.75f, skin);
    DrawCircleV(frontHand, 1.25f, outline);
    DrawCircleV(frontHand, 0.85f, skin);

    head = PlayerLocalPoint(player->position, forward, side, 4.0f, 0.0f);
    face = PlayerLocalPoint(head, forward, side, 0.45f, 0.0f);
    DrawCircleV(PlayerLocalPoint(head, forward, side, -0.35f, 0.0f), 2.7f,
                outline);
    DrawCircleV(face, 2.35f, skin);
    DrawLineEx(PlayerLocalPoint(head, forward, side, -1.1f, 2.0f),
               PlayerLocalPoint(head, forward, side, 0.3f, 2.3f), 1.0f,
               (Color){72, 43, 34, 255});

    visorStart = PlayerLocalPoint(head, forward, side, 1.25f, -1.35f);
    visorEnd = PlayerLocalPoint(head, forward, side, 1.25f, 1.35f);
    DrawLineEx(visorStart, visorEnd, 1.3f, outline);
    DrawLineEx(visorStart, visorEnd, 0.65f, emblem);

    chest = PlayerLocalPoint(player->position, forward, side, 0.0f, 0.0f);
    DrawCircleV(chest, 1.15f, outline);
    DrawTriangle(PlayerLocalPoint(chest, forward, side, 0.85f, 0.0f),
                 PlayerLocalPoint(chest, forward, side, -0.55f, 0.75f),
                 PlayerLocalPoint(chest, forward, side, -0.55f, -0.75f), emblem);
}
