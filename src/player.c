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


/* ---- Character rendering -------------------------------------------------
 *
 * The figure is built in a body frame rather than as a fixed sprite: `up` runs
 * from the hips to the head and `side` across the shoulders, and the whole frame
 * rotates from vertical toward the direction of travel as `leanAmount` rises.
 * Hovering, the character stands in the air with their knees drawn back; at
 * speed the same joints lay out flat with the arms thrown forward, because the
 * frame turned rather than because a different sprite was chosen.
 *
 * There is no outline. A dark rim around every limb flattens the figure into a
 * silhouette — a brick with a cape — and hides the shading that makes it read as
 * a body. Contrast comes instead from a lit tone on the side facing `up` and a
 * shadow tone opposite it, so the character is legible against terrain that the
 * lighting has already darkened.
 */

typedef struct BodyFrame {
    Vector2 origin;
    Vector2 up;
    Vector2 side;
} BodyFrame;

static Vector2 BodyPoint(const BodyFrame *frame, float alongUp, float alongSide)
{
    return (Vector2){
        frame->origin.x + frame->up.x * alongUp + frame->side.x * alongSide,
        frame->origin.y + frame->up.y * alongUp + frame->side.y * alongSide
    };
}

static void DrawBodyCell(Vector2 point, int size, Color color)
{
    DrawRectangle((int)floorf(point.x) - (size - 1) / 2,
                  (int)floorf(point.y) - (size - 1) / 2, size, size, color);
}

/* Fills a rectangle of the body frame one cell at a time, sampled at half a cell
   so a turned frame leaves no holes between the samples. Tone is chosen per
   column, which is what shades the body without an outline around it. */
static void FillBodyRect(const BodyFrame *frame, float fromUp, float toUp,
                         float halfWidth, Color shadow, Color mid, Color lit)
{
    float alongUp;

    for (alongUp = fromUp; alongUp <= toUp + 0.001f; alongUp += 0.5f) {
        float alongSide;

        for (alongSide = -halfWidth; alongSide <= halfWidth + 0.001f;
             alongSide += 0.5f) {
            Color tone = alongSide > 0.4f ? lit : (alongSide < -0.4f ? shadow : mid);

            DrawBodyCell(BodyPoint(frame, alongUp, alongSide), 1, tone);
        }
    }
}

/* A limb as a stepped run of cells. Limbs have to bend to arbitrary angles while
   the torso and head stay axis-aligned blocks; drawing them as rotated
   rectangles would shear them, and as fixed sprites they could not bend at all. */
static void DrawLimb(Vector2 from, Vector2 to, int thickness, Color color)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float span = fmaxf(fabsf(dx), fabsf(dy));
    /* Two samples per cell, so a limb at any angle stays a solid run. */
    int steps = (int)ceilf(span * 2.0f);
    int step;

    if (steps < 1) {
        steps = 1;
    }
    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;

        DrawBodyCell((Vector2){from.x + dx * amount, from.y + dy * amount},
                     thickness, color);
    }
}

#define SHOULDER_UP 3.2f

/* Where the hands reach, in body-frame coordinates: x across the shoulders, y
   from the hips toward the head. Absolute rather than relative to some offset,
   because a hand placed by an unexplained constant is a hand nobody can move
   with confidence later. */
static void PlayerHandTargets(const Player *player, Vector2 aimLocal, float lean,
                              float wave, Vector2 *lead, Vector2 *trail)
{
    /* Hanging at rest — the hands sit below the shoulders — and thrown out past
       the head at speed, along the body axis, which at full lean is the
       direction of travel. */
    Vector2 restLead = {2.2f - 1.2f * lean, 0.4f + 8.2f * lean};
    Vector2 restTrail = {-2.2f + 1.2f * lean, 0.0f + 7.2f * lean};
    float reach;

    restLead.y += wave * 0.35f;
    restTrail.y += wave * 0.3f;

    switch (player->pose) {
    case PLAYER_POSE_LASER:
        /* One arm snaps straight at the cursor; the other stays braced. */
        reach = 5.4f;
        lead->x = aimLocal.x * reach;
        lead->y = SHOULDER_UP + aimLocal.y * reach;
        *trail = (Vector2){-1.6f, SHOULDER_UP - 2.2f};
        return;
    case PLAYER_POSE_CHILL:
        /* Both palms out: a wide, two-handed gesture, so the cryo beam does not
           look like the laser with a different colour. */
        reach = 4.6f;
        lead->x = aimLocal.x * reach + 1.1f;
        lead->y = SHOULDER_UP + aimLocal.y * reach - 0.6f;
        trail->x = aimLocal.x * (reach - 0.8f) - 0.8f;
        trail->y = SHOULDER_UP + aimLocal.y * (reach - 0.8f) - 2.2f;
        return;
    case PLAYER_POSE_BLAST: {
        /* A punch: both arms drive out along the aim and recover. */
        float punch = Clamp(player->poseTimer / 0.28f, 0.0f, 1.0f);
        float thrust = 3.2f + 5.2f * sinf(punch * PI);

        lead->x = aimLocal.x * thrust + 1.0f;
        lead->y = SHOULDER_UP + aimLocal.y * thrust - 0.4f;
        trail->x = aimLocal.x * thrust - 1.0f;
        trail->y = SHOULDER_UP + aimLocal.y * thrust - 1.8f;
        return;
    }
    default:
        break;
    }

    /* Free flight: the leading arm still tracks the cursor, so aim stays
       readable, but only part of the way — the whole arm swinging to the cursor
       while hovering looks like pointing, not like flying. */
    lead->x = restLead.x + aimLocal.x * 2.0f * (1.0f - lean);
    lead->y = restLead.y + aimLocal.y * 2.0f * (1.0f - lean);
    *trail = restTrail;
}

void PlayerDraw(const Player *player, Vector2 aimPosition)
{
    /* Limbs get their own darker tone and the boots and gloves a bright one.
       Without that separation every part is the same blue and the figure reads
       as one shape however carefully the joints are placed. */
    const Color suitDark = (Color){40, 52, 86, 255};
    const Color suitMid = (Color){84, 108, 162, 255};
    const Color suitLit = (Color){152, 184, 236, 255};
    const Color capeCore = (Color){228, 88, 38, 255};
    const Color capeEdge = (Color){255, 156, 72, 255};
    const Color capeShade = (Color){140, 44, 28, 255};
    const Color skin = (Color){236, 190, 146, 255};
    /* The far-side limbs sit in a much darker tone than the torso. That
       separation, not an outline, is what puts them behind the body. */
    /* Dark enough to sit behind the body, light enough to still be a limb: at
       the value of the background the far leg disappears and only its boot
       remains, reading as a square floating beside the character. */
    Color limbDark = (Color){50, 64, 104, 255};
    Color limbMid = (Color){70, 92, 142, 255};
    Color trim = (Color){206, 146, 58, 255};
    Color accent = (Color){104, 232, 236, 255};
    Color lit = suitLit;
    Color mid = suitMid;
    Color dark = suitDark;
    BodyFrame frame;
    Vector2 aimLocal;
    Vector2 travel = {0.0f, -1.0f};
    Vector2 leadHand;
    Vector2 trailHand;
    Vector2 shoulderLead;
    Vector2 shoulderTrail;
    Vector2 hipLead;
    Vector2 hipTrail;
    Vector2 kneeLead;
    Vector2 kneeTrail;
    Vector2 head;
    float aimX;
    float aimY;
    float aimLength;
    float speed;
    float lean;
    float wave;
    float bob;
    float kneeDrop;
    float footBack;
    Vector2 back;
    float sideSign;
    int segment;

    if (player == NULL) {
        return;
    }

    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    lean = Clamp(player->leanAmount, 0.0f, 1.0f);
    if (speed > 0.001f) {
        travel = (Vector2){player->velocity.x / speed, player->velocity.y / speed};
    }

    /* The body axis turns from straight up toward the direction of travel. `up`
       runs from the hips to the head, so at full lean it *is* the direction of
       travel: the head leads and the feet trail. Pointing it away from travel
       instead flies the character feet first and tips them backwards out of
       every turn. */
    {
        float uprightAngle = -PI * 0.5f;
        float travelAngle = atan2f(travel.y, travel.x);
        float turn = travelAngle - uprightAngle;

        while (turn > PI) turn -= 2.0f * PI;
        while (turn < -PI) turn += 2.0f * PI;
        /* Straight down has two equally short rotations. Keep the side chosen
           by the last horizontal motion instead of allowing tiny float noise to
           flip the body between left and right. */
        if (fabsf(fabsf(turn) - PI) < 0.0001f) {
            turn = player->facingRight ? PI : -PI;
        }
        frame.up = (Vector2){cosf(uprightAngle + turn * lean),
                             sinf(uprightAngle + turn * lean)};
    }
    frame.side = (Vector2){-frame.up.y, frame.up.x};

    aimX = aimPosition.x - player->position.x;
    aimY = aimPosition.y - player->position.y;
    aimLength = sqrtf(aimX * aimX + aimY * aimY);
    if (aimLength > 0.001f) {
        aimX /= aimLength;
        aimY /= aimLength;
    } else {
        aimX = player->facingRight ? 1.0f : -1.0f;
        aimY = 0.0f;
    }
    /* The cursor expressed in the body frame, so arm poses can be written once
       and follow the body however it is turned. */
    aimLocal = (Vector2){aimX * frame.side.x + aimY * frame.side.y,
                         aimX * frame.up.x + aimY * frame.up.y};

    /* Which way the shoulders face. Aim decides it while hovering; at speed the
       body follows the travel, or the character would fly sideways. */
    sideSign = aimLocal.x >= 0.0f ? 1.0f : -1.0f;
    frame.side.x *= sideSign;
    frame.side.y *= sideSign;
    aimLocal.x *= sideSign;

    /* Which way is "behind the character". Standing still there is no direction
       of travel to use, and taking one anyway tucks the feet upward; behind is
       then simply the far side of the shoulders. */
    back.x = -frame.side.x * (1.0f - lean) - travel.x * lean;
    back.y = -frame.side.y * (1.0f - lean) - travel.y * lean;
    {
        float length = sqrtf(back.x * back.x + back.y * back.y);

        if (length < 0.001f) {
            back = (Vector2){-frame.side.x, -frame.side.y};
        } else {
            back.x /= length;
            back.y /= length;
        }
    }

    wave = sinf(player->animationTime *
                (player->boosting ? 11.0f + (float)player->boostStage * 2.0f
                                  : 5.0f));
    bob = (1.0f - lean) * sinf(player->animationTime * 2.4f) * 0.7f;
    frame.origin = (Vector2){player->position.x + frame.up.x * bob,
                             player->position.y + frame.up.y * bob};

    if (player->impactTimer > 0.0f) {
        /* Flash the fills, not a rim: the model has no outline to recolour, and
           brightening the whole body is what sells the hit. */
        /* Pale gold rather than orange: an orange flash is the colour of the
           cape, and the two merge into one blob at the moment of the hit. */
        dark = (Color){186, 154, 96, 255};
        mid = (Color){245, 226, 168, 255};
        lit = (Color){255, 252, 232, 255};
        limbDark = (Color){170, 138, 84, 255};
        limbMid = (Color){228, 202, 142, 255};
        trim = (Color){255, 250, 226, 255};
        accent = (Color){255, 255, 255, 255};
    }

    /* ---- acceleration burst, behind the body ---- */
    if (player->boostBurstTimer > 0.0f &&
        player->boostBurstStage != PLAYER_BOOST_NONE) {
        float duration = player->boostBurstStage == PLAYER_BOOST_STAGE_THREE
                             ? 0.52f
                             : 0.30f;
        float progress = 1.0f - player->boostBurstTimer / duration;
        float radius = 3.0f + progress *
                                  (8.0f + (float)player->boostBurstStage * 5.0f);
        float alpha = (1.0f - progress) * 0.8f;
        Color ring = player->boostBurstStage == PLAYER_BOOST_STAGE_THREE
                         ? (Color){255, 239, 190, 255}
                         : (Color){137, 224, 255, 255};
        int ringIndex;

        for (ringIndex = 0; ringIndex < (int)player->boostBurstStage; ++ringIndex) {
            float ringRadius = radius - (float)ringIndex * 3.0f;

            if (ringRadius > 1.0f) {
                DrawCircleLinesV(player->position, ringRadius,
                                 Fade(ring, alpha / (1.0f + ringIndex * 0.35f)));
            }
        }
    }

    /* ---- cape ---- */
    {
        /* The cape streams opposite the travel and sags when hovering, so it
           says which way the character is moving before the body does. */
        /* Anchored clear of the torso, or the cape reads as orange noise on the
           chest instead of as cloth behind the shoulders. */
        /* On the shoulder, not floating beside it: a cape that starts clear of
           the body reads as a separate ribbon following the character around. */
        Vector2 anchor = BodyPoint(&frame, 3.2f, -1.0f);
        /* Hanging behind and below at rest, streaming straight back at speed. */
        /* Nearly vertical at rest, straight back at speed. Hanging at an angle
           puts most of the cloth behind the torso, which covers it and leaves
           only a ragged diagonal edge showing. */
        Vector2 flow = {back.x * (0.3f + 0.7f * lean) -
                            frame.up.x * (1.0f - lean) * 0.95f,
                        back.y * (0.3f + 0.7f * lean) -
                            frame.up.y * (1.0f - lean) * 0.95f};
        float flowLength = sqrtf(flow.x * flow.x + flow.y * flow.y);
        float length = 8.0f + 4.0f * lean +
                       (player->boosting ? 1.2f + (float)player->boostStage * 1.4f
                                         : 0.0f);
        int steps = 24;

        if (flowLength > 0.001f) {
            flow.x /= flowLength;
            flow.y /= flowLength;
        }
        for (segment = 0; segment <= steps; ++segment) {
            float amount = (float)segment / (float)steps;
            float along = length * amount;
            /* Barely a stir while hovering — cloth hangs — and a real wave at
               speed. */
            float ripple = sinf(player->animationTime * 9.0f - amount * 4.2f) *
                           (0.3f + 1.9f * amount) * (0.18f + 0.82f * lean);
            Vector2 spine = {anchor.x + flow.x * along - flow.y * ripple,
                             anchor.y + flow.y * along + flow.x * ripple};
            /* Narrow enough to stay cloth behind the shoulders rather than a
               slab covering the character. */
            float width = 1.4f - 1.1f * amount;
            float across;

            /* Sampled finer than half a cell: at this width half-cell steps
               leave the cloth as a dotted line rather than a sheet. */
            for (across = -width; across <= width + 0.001f; across += 0.34f) {
                Color tone = across < -width * 0.45f
                                 ? capeShade
                                 : (across > width * 0.45f ? capeEdge : capeCore);

                DrawBodyCell((Vector2){spine.x - flow.y * across,
                                       spine.y + flow.x * across},
                             1, tone);
            }
        }
    }

    /* ---- legs ---- */
    /* Knees stay drawn back while hovering, the way someone hangs in the air in
       every superhero film, and straighten out as the body lays down. */
    kneeDrop = 3.6f - 0.9f * lean;
    /* Tucked back, not thrown out sideways: too much and the far foot leaves
       the silhouette entirely and reads as a loose block beside the body. */
    footBack = (1.3f - 1.0f * lean) + wave * 0.35f * (1.0f - lean);
    /* A wide enough stance that the two legs stay two legs. Placed closer
       together they overlap into one block and the character loses its legs
       entirely below the belt. */
    hipLead = BodyPoint(&frame, -0.6f, 1.0f);
    hipTrail = BodyPoint(&frame, -0.6f, -1.0f);
    kneeLead = BodyPoint(&frame, -0.6f - kneeDrop, 1.3f + 0.3f * (1.0f - lean));
    kneeTrail = BodyPoint(&frame, -0.6f - kneeDrop, -1.3f - 0.3f * (1.0f - lean));
    {
        Vector2 footLead = {kneeLead.x - frame.up.x * 2.2f + back.x * footBack,
                            kneeLead.y - frame.up.y * 2.2f + back.y * footBack};
        Vector2 footTrail = {kneeTrail.x - frame.up.x * 2.2f + back.x * footBack,
                             kneeTrail.y - frame.up.y * 2.2f + back.y * footBack};

        /* Thigh thicker than shin, and a boot at the end, so a leg reads as a
           leg rather than as a drawn line. */
        /* Thigh thicker than shin, and a boot at the end, so a leg reads as a
           leg rather than as a drawn line. */
        DrawLimb(hipTrail, kneeTrail, 2, limbDark);
        DrawLimb(kneeTrail, footTrail, 1, limbDark);
        DrawBodyCell(footTrail, 2, limbDark);
        DrawLimb(hipLead, kneeLead, 2, limbMid);
        DrawLimb(kneeLead, footLead, 1, limbMid);
        DrawBodyCell(footLead, 2, trim);
    }

    /* ---- arms ---- */
    shoulderLead = BodyPoint(&frame, SHOULDER_UP, 1.2f);
    shoulderTrail = BodyPoint(&frame, SHOULDER_UP, -1.2f);
    PlayerHandTargets(player, aimLocal, lean, wave, &leadHand, &trailHand);
    {
        Vector2 leadPoint = BodyPoint(&frame, leadHand.y, leadHand.x);
        Vector2 trailPoint = BodyPoint(&frame, trailHand.y, trailHand.x);
        Vector2 leadElbow = {(shoulderLead.x + leadPoint.x) * 0.5f +
                                 frame.side.x * 0.6f,
                             (shoulderLead.y + leadPoint.y) * 0.5f +
                                 frame.side.y * 0.6f};
        Vector2 trailElbow = {(shoulderTrail.x + trailPoint.x) * 0.5f -
                                  frame.side.x * 0.6f,
                              (shoulderTrail.y + trailPoint.y) * 0.5f -
                                  frame.side.y * 0.6f};

        DrawLimb(shoulderTrail, trailElbow, 2, limbDark);
        DrawLimb(trailElbow, trailPoint, 1, limbDark);
        DrawBodyCell(trailPoint, 1, limbDark);

        DrawLimb(shoulderLead, leadElbow, 2, limbMid);
        DrawLimb(leadElbow, leadPoint, 1, lit);
        DrawBodyCell(leadPoint, 1, trim);
        DrawBodyCell((Vector2){leadPoint.x + aimX * 0.9f,
                               leadPoint.y + aimY * 0.9f},
                     1, skin);

        if (player->pose == PLAYER_POSE_LASER ||
            player->pose == PLAYER_POSE_CHILL ||
            player->pose == PLAYER_POSE_BLAST) {
            Color glow = player->pose == PLAYER_POSE_CHILL
                             ? (Color){206, 244, 255, 235}
                             : (player->pose == PLAYER_POSE_BLAST
                                    ? (Color){196, 222, 255, 235}
                                    : (Color){255, 224, 168, 235});

            DrawBodyCell(leadPoint, 2, glow);
            if (player->pose != PLAYER_POSE_LASER) {
                DrawBodyCell(trailPoint, 2, glow);
            }
        }
    }

    /* ---- torso, neck, head ---- */
    FillBodyRect(&frame, -0.5f, 3.5f, 1.0f, dark, mid, lit);
    /* A belt breaks the torso into a chest and a waist; without it the body is
       one undifferentiated block whatever tones it carries. */
    FillBodyRect(&frame, -0.3f, -0.1f, 1.0f, capeShade, capeCore, capeEdge);
    /* One cyan mark on the chest. Any more and the eye has nowhere to settle:
       the visor stops being the face and becomes another light. */
    DrawBodyCell(BodyPoint(&frame, 2.0f, 0.35f), 1, accent);

    /* One cell of neck. Without the gap the head merges into the shoulders and
       the whole figure reads as a single block. */
    DrawBodyCell(BodyPoint(&frame, 4.2f, 0.0f), 1, dark);

    head = BodyPoint(&frame, 6.0f, 0.25f * aimLocal.x);
    FillBodyRect(&frame, 5.2f, 6.9f, 1.0f, dark, mid, lit);
    {
        /* The visor looks where the cursor is, independently of the body. */
        Vector2 visor = {head.x + frame.side.x * 0.9f + aimX * 0.5f,
                         head.y + frame.side.y * 0.9f + aimY * 0.5f};

        DrawBodyCell(visor, 1, accent);
        DrawBodyCell((Vector2){visor.x + frame.up.x * 0.6f,
                               visor.y + frame.up.y * 0.6f},
                     1, accent);
        DrawBodyCell(BodyPoint(&frame, 5.4f, -0.9f), 1, skin);
    }

    /* ---- speed streaks ---- */
    if (speed > player->maxSpeed * 0.8f) {
        float intensity = Clamp((speed - player->maxSpeed * 0.8f) /
                                    (player->boostMaxSpeed - player->maxSpeed * 0.8f),
                                0.0f, 1.0f);
        Vector2 across = {-travel.y, travel.x};
        int streakCount = 4 + (int)player->boostStage * 2;
        int streak;

        if (player->boosting) {
            intensity = fmaxf(intensity, 0.18f + (float)player->boostStage * 0.18f);
        }

        for (streak = 0; streak < streakCount; ++streak) {
            float streakBack = 8.0f + (float)streak * 4.0f;
            float offset = sinf(player->animationTime * 11.0f + (float)streak) * 3.4f;
            int length = 3 + (int)player->boostStage - streak / 2;
            int cell;

            if (length < 1) length = 1;

            for (cell = 0; cell < length; ++cell) {
                Vector2 point = {
                    player->position.x - travel.x * (streakBack + (float)cell) +
                        across.x * offset,
                    player->position.y - travel.y * (streakBack + (float)cell) +
                        across.y * offset
                };

                DrawBodyCell(point, 1,
                             Fade(streak == 0 ? accent : (Color){186, 226, 255, 255},
                                  intensity * (0.88f - 0.065f * (float)streak)));
            }
        }

        if (player->boostStage == PLAYER_BOOST_STAGE_THREE &&
            speed >= player->sonicSpeed) {
            float pulse = 10.0f + sinf(player->animationTime * 18.0f) * 1.8f;
            Vector2 coneStart = {player->position.x - travel.x * 3.5f,
                                 player->position.y - travel.y * 3.5f};
            Vector2 coneBack = {player->position.x - travel.x * 28.0f,
                                player->position.y - travel.y * 28.0f};

            DrawLineEx(coneStart,
                       (Vector2){coneBack.x + across.x * pulse,
                                 coneBack.y + across.y * pulse},
                       0.7f, (Color){203, 235, 255, 150});
            DrawLineEx(coneStart,
                       (Vector2){coneBack.x - across.x * pulse,
                                 coneBack.y - across.y * pulse},
                       0.7f, (Color){203, 235, 255, 150});
        }
    }

    /* ---- drill contact ---- */
    if (player->drilledCells > 0 && speed > 0.001f) {
        Vector2 across = {-travel.y, travel.x};
        int spark;

        for (spark = -1; spark <= 1; ++spark) {
            Vector2 point = {player->drillPosition.x + across.x * (float)spark * 2.5f,
                             player->drillPosition.y + across.y * (float)spark * 2.5f};

            DrawBodyCell(point, spark == 0 ? 3 : 1,
                         spark == 0 ? (Color){255, 214, 96, 245} : accent);
        }
    }
}
