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
    player->boostStageTwoAcceleration = 720.0f;
    player->boostStageThreeAcceleration = 980.0f;
    player->boostStageOneSpeed = 235.0f;
    player->boostStageTwoSpeed = 380.0f;
    player->boostMaxSpeed = 620.0f;
    player->boostDrag = 0.38f;
    player->boostStageTwoDrag = 0.24f;
    player->boostStageThreeDrag = 0.12f;
    player->sonicSpeed = 520.0f;
    player->boostStageTwoDelay = 1.0f;
    player->boostStageThreeDelay = 1.4f;
    player->boostStageTime = 0.0f;
    player->boostGrace = 0.0f;
    player->drillSpeed = 92.0f;
    player->drillResistance = 0.004f;
    player->drag = 1.1f;
    player->restitution = 0.34f;
    player->radius = 3.2f;
    player->impactStrength = 0.0f;
    player->impactTimer = 0.0f;
    player->animationTime = 0.0f;
    player->leanAmount = 0.0f;
    player->pose = PLAYER_POSE_FLY;
    player->poseTimer = 0.0f;
    player->boostTrailTimer = 0.0f;
    player->boostBurstTimer = 0.0f;
    player->drilledCells = 0;
    player->boostStage = PLAYER_BOOST_NONE;
    player->boostStageChanged = PLAYER_BOOST_NONE;
    player->boostBurstStage = PLAYER_BOOST_NONE;
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
        float resistance = player->drillResistance;
        float floorSpeed = player->drillSpeed * 1.05f;
        float slowed;
        float scale;

        if (player->boostStage == PLAYER_BOOST_STAGE_TWO) {
            resistance /= 1.75f;
            floorSpeed = fmaxf(floorSpeed, player->boostStageOneSpeed * 0.72f);
        } else if (player->boostStage == PLAYER_BOOST_STAGE_THREE) {
            resistance /= 3.25f;
            floorSpeed = fmaxf(floorSpeed, player->sonicSpeed * 0.70f);
        }
        slowed = speed * expf(-(float)destroyed * resistance);

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

static float PlayerBoostStageSpeed(const Player *player)
{
    switch (player->boostStage) {
    case PLAYER_BOOST_STAGE_TWO:
        return player->boostStageTwoSpeed;
    case PLAYER_BOOST_STAGE_THREE:
        return player->boostMaxSpeed;
    default:
        return player->boostStageOneSpeed;
    }
}

static void PlayerUpdateBoostStage(Player *player, Vector2 input, float speed,
                                   float deltaTime)
{
    float alignment = 0.0f;
    float requiredSpeed;
    float delay;

    if (!player->boosting) {
        player->boostStage = PLAYER_BOOST_NONE;
        player->boostStageTime = 0.0f;
        return;
    }

    if (player->boostStage == PLAYER_BOOST_NONE) {
        player->boostStage = PLAYER_BOOST_STAGE_ONE;
        player->boostStageChanged = PLAYER_BOOST_STAGE_ONE;
        player->boostStageTime = 0.0f;
    }

    if (player->thrusting && speed > 0.001f) {
        alignment = (player->velocity.x * input.x + player->velocity.y * input.y) /
                    speed;
    }
    requiredSpeed = PlayerBoostStageSpeed(player) * 0.86f;
    if (player->thrusting && alignment >= 0.88f && speed >= requiredSpeed) {
        player->boostStageTime += deltaTime;
    } else {
        /* A brief correction does not erase a long run, but turning around or
           grinding through a wall cannot charge the next stage. */
        player->boostStageTime =
            fmaxf(0.0f, player->boostStageTime - deltaTime * 1.5f);
    }

    delay = player->boostStage == PLAYER_BOOST_STAGE_ONE
                ? player->boostStageTwoDelay
                : player->boostStageThreeDelay;
    if ((player->boostStage == PLAYER_BOOST_STAGE_ONE ||
         player->boostStage == PLAYER_BOOST_STAGE_TWO) &&
        player->boostStageTime >= delay) {
        player->boostStage = (PlayerBoostStage)((int)player->boostStage + 1);
        player->boostStageChanged = player->boostStage;
        player->boostStageTime = 0.0f;
    }

    if (player->boostStageChanged != PLAYER_BOOST_NONE) {
        float impulse = player->boostStageChanged == PLAYER_BOOST_STAGE_ONE
                            ? 34.0f
                            : (player->boostStageChanged == PLAYER_BOOST_STAGE_TWO
                                   ? 86.0f
                                   : 180.0f);

        player->velocity.x += input.x * impulse;
        player->velocity.y += input.y * impulse;
        player->boostBurstStage = player->boostStageChanged;
        player->boostBurstTimer = player->boostStageChanged == PLAYER_BOOST_STAGE_THREE
                                      ? 0.52f
                                      : 0.30f;
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
    player->boostStageChanged = PLAYER_BOOST_NONE;
    player->boostBurstTimer = fmaxf(0.0f, player->boostBurstTimer - deltaTime);
    player->thrusting = false;
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
    velocityLength = sqrtf(player->velocity.x * player->velocity.x +
                           player->velocity.y * player->velocity.y);
    PlayerUpdateBoostStage(player, input, velocityLength, deltaTime);
    acceleration = player->acceleration;
    speedLimit = player->maxSpeed;
    damping = player->drag;
    if (player->boosting) {
        switch (player->boostStage) {
        case PLAYER_BOOST_STAGE_TWO:
            acceleration = player->boostStageTwoAcceleration;
            speedLimit = player->boostStageTwoSpeed;
            damping = player->boostStageTwoDrag;
            break;
        case PLAYER_BOOST_STAGE_THREE:
            acceleration = player->boostStageThreeAcceleration;
            speedLimit = player->boostMaxSpeed;
            damping = player->boostStageThreeDrag;
            break;
        default:
            acceleration = player->boostAcceleration;
            speedLimit = player->boostStageOneSpeed;
            damping = player->boostDrag;
            break;
        }
    }
    if (player->thrusting) {
        player->velocity.x += input.x * acceleration * deltaTime;
        player->velocity.y += input.y * acceleration * deltaTime;
    }

    damping = expf(-damping * deltaTime);
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

    /* Normal flight is a hover, not a slow version of the boost pose. Speed only
       adds a small forward weight shift until boost is actually engaged; boost
       then opens the rest of the range toward a horizontal superhero pose.
       Smoothed, because snapping between the two reads as a sprite swap. */
    {
        const float maximumHoverLean = 0.12f;
        float cruise = player->maxSpeed > 0.001f
                           ? Clamp(velocityLength / player->maxSpeed, 0.0f, 1.0f)
                           : 0.0f;
        float target = cruise * maximumHoverLean;

        if (player->boosting && velocityLength > player->drillSpeed * 0.5f) {
            float boostSpan = player->boostMaxSpeed - player->drillSpeed * 0.5f;
            float boostProgress = boostSpan > 0.001f
                                      ? Clamp((velocityLength -
                                               player->drillSpeed * 0.5f) /
                                                  boostSpan,
                                              0.0f, 1.0f)
                                      : 1.0f;

            target = 0.82f + 0.18f * boostProgress;
        }
        player->leanAmount += (target - player->leanAmount) *
                              (1.0f - expf(-9.0f * deltaTime));
    }
    player->poseTimer = fmaxf(0.0f, player->poseTimer - deltaTime);
    if (player->poseTimer <= 0.0f) {
        player->pose = PLAYER_POSE_FLY;
    }
    if (player->boosting && velocityLength >= player->drillSpeed * 0.65f) {
        player->boostTrailTimer -= deltaTime;
        if (player->boostTrailTimer <= 0.0f) {
            player->boostTrailEmitted = true;
            player->boostTrailTimer = player->boostStage == PLAYER_BOOST_STAGE_THREE
                                          ? 0.009f
                                          : (player->boostStage == PLAYER_BOOST_STAGE_TWO
                                                 ? 0.016f
                                                 : 0.025f);
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

void PlayerSetPose(Player *player, PlayerPose pose, float holdTime)
{
    if (player == NULL || holdTime <= 0.0f) {
        return;
    }
    /* A one-shot already running outlasts a held pose asking for a shorter
       time, so firing the laser mid-punch does not cut the punch short. */
    if (pose != player->pose && holdTime < player->poseTimer) {
        return;
    }
    player->pose = pose;
    player->poseTimer = holdTime;
}

void PlayerApplyImpulse(Player *player, Vector2 impulse)
{
    if (player == NULL) {
        return;
    }
    player->velocity.x += impulse.x;
    player->velocity.y += impulse.y;
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
