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
    player->drillPosition = position;
    player->drillMaterial = MATERIAL_EMPTY;
    player->acceleration = 250.0f;
    player->maxSpeed = 118.0f;
    player->boostAcceleration = 540.0f;
    player->boostMaxSpeed = 235.0f;
    player->boostDrag = 0.38f;
    player->boostGrace = 0.0f;
    player->drillSpeed = 92.0f;
    player->drillResistance = 0.004f;
    player->drag = 1.1f;
    player->restitution = 0.34f;
    player->radius = 3.2f;
    player->impactStrength = 0.0f;
    player->impactTimer = 0.0f;
    player->animationTime = 0.0f;
    player->boostTrailTimer = 0.0f;
    player->drilledCells = 0;
    player->facingRight = true;
    player->thrusting = false;
    player->boosting = false;
    player->boostTrailEmitted = false;
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

/* Records what is about to be cut. Sampling before the cut is the point: after
   it the cell is empty and there is nothing left to identify. */
static void PlayerRecordDrillMaterial(Player *player, const World *world, Vector2 at)
{
    CellMaterial material = WorldGetCell(world, (int)floorf(at.x), (int)floorf(at.y));

    if (WorldMaterialIsSolid(material)) {
        player->drillMaterial = material;
    }
}

/* Clears whatever blocks the collider at `at`, widening until it is actually
   free. One pass is not enough on its own: WorldDrillCircle measures an integer
   radius from a floored centre, while the collider is a float circle measured to
   the nearest edge of a cell box, so the collider can touch cells a single cut
   of the same nominal radius leaves standing. Returns the cells removed. */
static int PlayerCutFree(Player *player, World *world, Vector2 at)
{
    int base = (int)ceilf(player->radius);
    int removed = 0;
    int attempt;

    PlayerRecordDrillMaterial(player, world, at);

    for (attempt = 0; attempt < 3; ++attempt) {
        removed += WorldDrillCircle(world, (int)floorf(at.x), (int)floorf(at.y),
                                    base + attempt);
        if (!PlayerCollidesAt(player, world, at)) {
            break;
        }
    }
    return removed;
}

static void PlayerDrillAhead(Player *player, World *world, Vector2 nextPosition)
{
    float speed = sqrtf(player->velocity.x * player->velocity.x +
                        player->velocity.y * player->velocity.y);
    Vector2 direction;
    Vector2 drillPoint;
    int destroyed;

    if (!player->boosting || speed < 1.0f) {
        return;
    }

    /* Above the drill threshold a boost cuts continuously ahead of itself. Below
       it the drill still bites, but only where the player is actually pressed
       into material, and it cuts around the collider rather than ahead of it.
       Without this a boost begun from rest against a wall could never start: the
       collision zeroes the blocked component every frame, so the speed can never
       climb to the threshold that would have cut the wall away, and a cut placed
       ahead of the collider lands inside the hole it already made while the rim
       that is actually blocking survives. */
    if (speed < player->drillSpeed) {
        if (!PlayerCollidesAt(player, world, nextPosition)) {
            return;
        }
        destroyed = PlayerCutFree(player, world, nextPosition);
        if (destroyed > 0) {
            player->drilledCells += destroyed;
            player->drillPosition = nextPosition;
        }
        return;
    }

    direction = (Vector2){player->velocity.x / speed, player->velocity.y / speed};
    drillPoint = (Vector2){nextPosition.x + direction.x * player->radius * 0.7f,
                           nextPosition.y + direction.y * player->radius * 0.7f};
    PlayerRecordDrillMaterial(player, world, drillPoint);
    destroyed = WorldDrillCircle(world, (int)floorf(drillPoint.x),
                                 (int)floorf(drillPoint.y),
                                 (int)ceilf(player->radius));
    if (destroyed > 0) {
        /* Cutting terrain costs speed, but never enough to fall under the drill
           threshold: a boost that stalls would leave the player buried. */
        float floorSpeed = player->drillSpeed * 1.05f;
        float slowed = speed * expf(-(float)destroyed * player->drillResistance);
        float scale;

        if (slowed < floorSpeed) {
            slowed = fminf(speed, floorSpeed);
        }
        scale = slowed / speed;
        player->velocity.x *= scale;
        player->velocity.y *= scale;
        player->drilledCells += destroyed;
        player->drillPosition = drillPoint;
    }
}

void PlayerUpdate(Player *player, World *world, Vector2 input, bool boostHeld,
                  float deltaTime)
{
    float inputLength;
    float velocityLength;
    float acceleration;
    float speedLimit;
    float damping;
    float moveX;
    float moveY;
    float stepTime;
    bool wasBoosting;
    int moveSteps;
    int step;

    if (player == NULL || world == NULL) {
        return;
    }

    player->impactStrength = 0.0f;
    player->impactNormal = (Vector2){0.0f, 0.0f};
    player->impactTimer = fmaxf(0.0f, player->impactTimer - deltaTime);
    player->drilledCells = 0;
    player->boostTrailEmitted = false;
    player->thrusting = false;
    wasBoosting = player->boosting;

    inputLength = sqrtf(input.x * input.x + input.y * input.y);
    if (inputLength > 0.0f) {
        input.x /= inputLength;
        input.y /= inputLength;
        player->thrusting = true;
    }
    /* Boost outlives the directional input for a moment, so letting go of WASD
       inside a tunnel coasts out instead of dropping the drill into a wall. */
    if (boostHeld && player->thrusting) {
        player->boostGrace = 0.14f;
    } else {
        player->boostGrace = fmaxf(0.0f, player->boostGrace - deltaTime);
    }
    player->boosting = boostHeld && (player->thrusting || player->boostGrace > 0.0f);
    acceleration = player->boosting ? player->boostAcceleration
                                    : player->acceleration;
    speedLimit = player->boosting ? player->boostMaxSpeed : player->maxSpeed;
    if (player->thrusting) {
        if (player->boosting && !wasBoosting) {
            player->velocity.x += input.x * 34.0f;
            player->velocity.y += input.y * 34.0f;
        }
        player->velocity.x += input.x * acceleration * deltaTime;
        player->velocity.y += input.y * acceleration * deltaTime;
    }

    damping = expf(-(player->boosting ? player->boostDrag : player->drag) *
                   deltaTime);
    player->velocity.x *= damping;
    player->velocity.y *= damping;
    velocityLength = sqrtf(player->velocity.x * player->velocity.x +
                           player->velocity.y * player->velocity.y);
    if (velocityLength > speedLimit) {
        float reducedSpeed = player->boosting
                                 ? speedLimit
                                 : fmaxf(speedLimit, velocityLength - 180.0f * deltaTime);
        float scale = reducedSpeed / velocityLength;

        player->velocity.x *= scale;
        player->velocity.y *= scale;
        velocityLength = reducedSpeed;
    }

    player->animationTime += deltaTime *
                             (player->boosting ? 2.4f
                                               : 1.0f + fminf(velocityLength / 90.0f,
                                                              0.8f));
    if (player->boosting && velocityLength >= player->drillSpeed * 0.65f) {
        player->boostTrailTimer -= deltaTime;
        if (player->boostTrailTimer <= 0.0f) {
            player->boostTrailEmitted = true;
            player->boostTrailTimer = 0.025f;
        }
    } else {
        player->boostTrailTimer = 0.0f;
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
        Vector2 nextPosition = {
            player->position.x + player->velocity.x * stepTime,
            player->position.y + player->velocity.y * stepTime
        };
        float incomingSpeed;

        PlayerDrillAhead(player, world, nextPosition);
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

void PlayerResolveWorldCollision(Player *player, World *world)
{
    Vector2 origin;
    int distance;

    if (player == NULL || world == NULL ||
        !PlayerCollidesAt(player, world, player->position)) {
        return;
    }

    /* Sand closing over a boosting player is not an impact to be pushed out of:
       the drill is already running, so cut the way clear and keep the momentum.
       Relocating would zero the very velocity that was about to free them, and
       because sand refills the tunnel every tick that happens on every frame —
       a boost could never cross a sand body at all. */
    if (player->boosting) {
        int destroyed = PlayerCutFree(player, world, player->position);

        if (destroyed > 0) {
            player->drilledCells += destroyed;
            player->drillPosition = player->position;
        }
        if (!PlayerCollidesAt(player, world, player->position)) {
            return;
        }
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
    static const int capeWave[4] = {0, -1, 0, 1};
    const Color outline = (Color){15, 20, 28, 255};
    const Color cape = (Color){235, 86, 31, 255};
    const Color capeShadow = (Color){143, 45, 27, 255};
    const Color skin = (Color){224, 170, 119, 255};
    Color suit = (Color){43, 50, 61, 255};
    Color suitLight = (Color){72, 82, 96, 255};
    Color accent = (Color){67, 206, 218, 255};
    float aimX;
    float aimY;
    float speed;
    bool facingRight;
    bool capeOnLeft;
    int centerX;
    int centerY;
    int animationFrame;
    int idleFrame;
    int tailOffsetY;
    int legFrame;
    int armOffsetY;
    int boostExtension;
    int eyeX;
    int eyeY;

    if (player == NULL) {
        return;
    }

    aimX = aimPosition.x - player->position.x;
    aimY = aimPosition.y - player->position.y;
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    facingRight = fabsf(aimX) > 0.5f ? aimX > 0.0f : player->facingRight;
    capeOnLeft = fabsf(player->velocity.x) > 20.0f
                     ? player->velocity.x > 0.0f
                     : facingRight;
    centerX = (int)floorf(player->position.x);
    centerY = (int)floorf(player->position.y);
    animationFrame = (int)(player->animationTime * 7.0f) & 3;
    idleFrame = (int)(player->animationTime * 3.0f) & 3;
    if (!player->thrusting && speed < 5.0f && idleFrame == 2) {
        ++centerY;
    }
    tailOffsetY = (int)roundf(Clamp(-player->velocity.y / 40.0f +
                                        (float)capeWave[animationFrame],
                                    -2.0f, 2.0f));
    legFrame = player->thrusting && !player->boosting ? animationFrame & 1 : 0;
    armOffsetY = aimY > 5.0f ? 1 : (aimY < -5.0f ? -1 : 0);
    boostExtension = player->boosting ? 2 : 0;
    if (player->impactTimer > 0.0f) {
        /* Flash the lit fills and leave the rim dark: recolouring the outline
           floods most of the model and erases the silhouette. */
        suit = (Color){186, 104, 34, 255};
        suitLight = (Color){255, 190, 88, 255};
        accent = (Color){255, 240, 190, 255};
    }

    if (player->boosting && speed > 40.0f) {
        Vector2 direction = {player->velocity.x / speed, player->velocity.y / speed};
        Vector2 side = {-direction.y, direction.x};
        int streak;

        /* Three tapering dashes read as speed lines instead of lone pixels. */
        for (streak = 0; streak < 3; ++streak) {
            float distance = 7.0f + (float)streak * 4.0f;
            float sideOffset = (float)capeWave[(animationFrame + streak) & 3] * 2.0f;
            Color streakColor = streak == 0 ? (Color){91, 224, 231, 210}
                                            : (Color){179, 239, 237, 130};
            int length = 3 - streak;
            int segment;

            for (segment = 0; segment < length; ++segment) {
                float back = distance + (float)segment;
                int x = (int)floorf(player->position.x - direction.x * back +
                                    side.x * sideOffset);
                int y = (int)floorf(player->position.y - direction.y * back +
                                    side.y * sideOffset);

                DrawRectangle(x, y, 1, 1, streakColor);
            }
        }
    }

    /* Shoulder root and bending tail share three cells, so the cape stays one
       connected shape at every wave offset instead of splitting into a blob. */
    if (capeOnLeft) {
        DrawRectangle(centerX - 5, centerY - 4, 5, 9, outline);
        DrawRectangle(centerX - 6 - boostExtension, centerY - 2 + tailOffsetY,
                      4 + boostExtension, 7, outline);
        DrawRectangle(centerX - 4, centerY - 3, 3, 7, cape);
        DrawRectangle(centerX - 5 - boostExtension, centerY - 1 + tailOffsetY,
                      2 + boostExtension, 5, cape);
        DrawRectangle(centerX - 5 - boostExtension,
                      centerY + 2 + tailOffsetY + capeWave[animationFrame],
                      2 + boostExtension, 2, capeShadow);
    } else {
        DrawRectangle(centerX + 1, centerY - 4, 5, 9, outline);
        DrawRectangle(centerX + 3, centerY - 2 + tailOffsetY,
                      4 + boostExtension, 7, outline);
        DrawRectangle(centerX + 2, centerY - 3, 3, 7, cape);
        DrawRectangle(centerX + 4, centerY - 1 + tailOffsetY,
                      2 + boostExtension, 5, cape);
        DrawRectangle(centerX + 4,
                      centerY + 2 + tailOffsetY + capeWave[animationFrame],
                      2 + boostExtension, 2, capeShadow);
    }

    DrawRectangle(centerX - 2, centerY + 1, 2, 5 + legFrame, outline);
    DrawRectangle(centerX - 1, centerY + 2, 1, 3 + legFrame, suitLight);
    DrawRectangle(centerX + 1, centerY + 1 + legFrame, 2, 5 - legFrame, outline);
    DrawRectangle(centerX + 1, centerY + 2 + legFrame, 1, 3 - legFrame, suit);
    DrawRectangle(centerX - 2, centerY + 5 + legFrame, 2, 1, capeShadow);
    DrawRectangle(centerX + 1, centerY + 5, 2, 1, capeShadow);

    /* The rear arm hangs down in the darker tone; the lit front arm points at
       the cursor, so the aim pose stays readable against the world. */
    if (facingRight) {
        DrawRectangle(centerX - 3, centerY - 2 + legFrame, 2, 5, outline);
        DrawRectangle(centerX - 3, centerY - 1 + legFrame, 1, 3, suit);
        DrawRectangle(centerX + 1, centerY - 2 + armOffsetY, 5, 2, outline);
        DrawRectangle(centerX + 2, centerY - 1 + armOffsetY, 3, 1, suitLight);
        DrawRectangle(centerX + 5, centerY - 1 + armOffsetY, 1, 1, skin);
    } else {
        DrawRectangle(centerX + 2, centerY - 2 + legFrame, 2, 5, outline);
        DrawRectangle(centerX + 3, centerY - 1 + legFrame, 1, 3, suit);
        DrawRectangle(centerX - 5, centerY - 2 + armOffsetY, 5, 2, outline);
        DrawRectangle(centerX - 4, centerY - 1 + armOffsetY, 3, 1, suitLight);
        DrawRectangle(centerX - 5, centerY - 1 + armOffsetY, 1, 1, skin);
    }

    PlayerDrawPixelBlock(centerX - 1, centerY - 3, 3, 5, suit, outline);
    DrawRectangle(centerX - 1, centerY + 1, 3, 1, capeShadow);
    DrawRectangle(centerX, centerY - 1, 1, 2,
                  player->boosting && (animationFrame & 1) != 0
                      ? (Color){194, 250, 239, 255}
                      : accent);

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

    if (player->drilledCells > 0 && speed > 0.001f) {
        Vector2 direction = {player->velocity.x / speed, player->velocity.y / speed};
        Vector2 side = {-direction.y, direction.x};
        int spark;

        for (spark = -1; spark <= 1; ++spark) {
            int x = (int)floorf(player->drillPosition.x +
                                side.x * (float)spark * 2.5f);
            int y = (int)floorf(player->drillPosition.y +
                                side.y * (float)spark * 2.5f);

            DrawRectangle(x, y, spark == 0 ? 2 : 1, spark == 0 ? 2 : 1,
                          spark == 0 ? (Color){255, 206, 75, 245} : accent);
        }
    }
}
