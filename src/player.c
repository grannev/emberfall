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
    player->health = 100.0f;
    player->maxHealth = 100.0f;
    player->invulnerability = 0.0f;
    player->respawnTimer = 0.0f;
    player->alive = true;
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

static void PlayerTakeDamage(Player *player, float damage)
{
    if (!player->alive || player->invulnerability > 0.0f || damage <= 0.0f) {
        return;
    }

    player->health = fmaxf(0.0f, player->health - damage);
    player->invulnerability = 0.28f;
    if (player->health <= 0.0f) {
        player->alive = false;
        player->respawnTimer = 1.15f;
        player->velocity = (Vector2){0.0f, 0.0f};
    }
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

    player->invulnerability = fmaxf(0.0f, player->invulnerability - deltaTime);
    if (!player->alive) {
        player->respawnTimer = fmaxf(0.0f, player->respawnTimer - deltaTime);
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

    if (player == NULL || world == NULL || !player->alive ||
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

void PlayerApplyWorldHazards(Player *player, const World *world)
{
    int minimumX;
    int maximumX;
    int minimumY;
    int maximumY;
    int y;
    bool touchesFire = false;
    bool touchesLava = false;

    if (player == NULL || world == NULL || !player->alive) {
        return;
    }

    minimumX = (int)floorf(player->position.x - player->radius);
    maximumX = (int)floorf(player->position.x + player->radius);
    minimumY = (int)floorf(player->position.y - player->radius);
    maximumY = (int)floorf(player->position.y + player->radius);

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            CellMaterial material = WorldGetCell(world, x, y);

            touchesLava = touchesLava || material == MATERIAL_LAVA;
            touchesFire = touchesFire || material == MATERIAL_FIRE;
        }
    }

    if (touchesLava) {
        PlayerTakeDamage(player, 24.0f);
    } else if (touchesFire) {
        PlayerTakeDamage(player, 11.0f);
    }
}

bool PlayerNeedsRespawn(const Player *player)
{
    return player != NULL && !player->alive && player->respawnTimer <= 0.0f;
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
    PlayerTakeDamage(player, 30.0f * strength);
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
    Color bodyColor;

    if (player == NULL || !player->alive) {
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

    bodyColor = player->invulnerability > 0.0f ? (Color){130, 210, 255, 255}
                                               : (Color){36, 139, 214, 255};
    DrawCircleV(player->position, player->radius, bodyColor);
    DrawCircleLinesV(player->position, player->radius, (Color){173, 224, 255, 255});
    eye = (Vector2){player->position.x + aim.x * 3.0f,
                    player->position.y + aim.y * 3.0f};
    DrawCircleV(eye, 1.2f, (Color){255, 241, 126, 255});
}
