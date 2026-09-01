#include "player.h"

#include "materials.h"

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
    player->drillResistance = 0.010f;
    player->drillHeat = 0.72f;
    /* A third of the steering left at top speed. Enough to pick a line through
       a cavern at six hundred cells a second, not enough to turn a corner: the
       cost of going that fast is that the world has to be read further ahead. */
    player->turnAuthorityAtHighSpeed = 0.34f;
    /* Braking beats accelerating, which is what makes committing to speed feel
       safe rather than reckless. */
    player->brakingAuthority = 2.6f;
    player->fluidDrag = 3.4f;
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

Vector2 PlayerBeamOrigin(const Player *player, Vector2 aim)
{
    Vector2 eye;
    float dx;
    float dy;
    float length;

    if (player == NULL) {
        return aim;
    }
    /* The head sits above the middle of the collider, and leans forward with
       the same posture the renderer draws: at full boost the character is laid
       out along the direction of travel, and the eyes go with them. */
    eye = (Vector2){player->position.x +
                        (player->facingRight ? 1.0f : -1.0f) * player->radius *
                            0.35f * player->leanAmount,
                    player->position.y - player->radius * 0.55f};
    dx = aim.x - eye.x;
    dy = aim.y - eye.y;
    length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        return eye;
    }
    dx /= length;
    dy /= length;
    {
        Vector2 out = {eye.x + dx * player->radius * 0.85f,
                       eye.y + dy * player->radius * 0.85f};
        float clearance = sqrtf((out.x - player->position.x) *
                                    (out.x - player->position.x) +
                                (out.y - player->position.y) *
                                    (out.y - player->position.y));

        /* Aiming straight down, the offset to the head and the push along the
           aim point opposite ways and very nearly cancel, leaving the beam
           starting inside the character. Whatever the eye happens to be, the
           muzzle has to end up clear of the collider. */
        if (clearance < player->radius * 0.9f) {
            out = (Vector2){player->position.x + dx * player->radius * 0.9f,
                            player->position.y + dy * player->radius * 0.9f};
        }
        return out;
    }
}

bool PlayerIsDrilling(const Player *player)
{
    float speed;

    if (player == NULL || !player->boosting) {
        return false;
    }
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    return speed >= player->drillSpeed;
}

float PlayerDrillRadius(const Player *player)
{
    float scale;

    switch (player->boostStage) {
    case PLAYER_BOOST_STAGE_THREE: scale = PLAYER_DRILL_WIDTH_STAGE_THREE; break;
    case PLAYER_BOOST_STAGE_TWO: scale = PLAYER_DRILL_WIDTH_STAGE_TWO; break;
    case PLAYER_BOOST_STAGE_ONE: scale = PLAYER_DRILL_WIDTH_STAGE_ONE; break;
    default: scale = PLAYER_DRILL_WIDTH_IDLE; break;
    }
    return player->radius * scale;
}

/* Leaves the tunnel wall glowing.

   The heat is a fraction of each material's *own* phase threshold rather than a
   temperature, which is what makes rock and dirt read alike: rock melts at 720
   and dirt catches at 175, so a single number either barely tints the rock or
   sets the dirt alight. A fraction glows both and pushes neither over, and the
   ordinary thermal simulation takes it from there — the wall cools on its own,
   and nothing here turns anything into lava.

   Three bands, hottest at the cut. The scan is the ring between the tunnel and
   its surroundings, so its cost is the area of one carve and not a function of
   anything else. */
static void PlayerHeatTunnel(World *world, Vector2 at, float radius,
                             float strength)
{
    int centreX = (int)floorf(at.x);
    int centreY = (int)floorf(at.y);
    int reach = (int)ceilf(radius) + 3;
    int y;

    if (strength <= 0.0f) {
        return;
    }
    for (y = centreY - reach; y <= centreY + reach; ++y) {
        int x;

        for (x = centreX - reach; x <= centreX + reach; ++x) {
            float dx = (float)x + 0.5f - at.x;
            float dy = (float)y + 0.5f - at.y;
            float distance = sqrtf(dx * dx + dy * dy);
            const MaterialInfo *info;
            CellMaterial material;
            float band;
            float target;

            if (distance > radius + 3.0f) {
                continue;
            }
            material = WorldGetCell(world, x, y);
            info = MaterialAt(material);
            if (!info->solid || !info->onHeat.enabled ||
                info->onHeat.threshold <= 60.0f) {
                continue;
            }
            /* Full strength against the cut, falling away over three cells. */
            band = 1.0f - Clamp((distance - radius) / 3.0f, 0.0f, 1.0f);
            target = info->onHeat.threshold * strength * band;
            if (target > WorldGetTemperature(world, x, y)) {
                WorldSetTemperature(world, x, y, target);
            }
        }
    }
}

/* Carves the corridor the player is about to travel down, as a line of bites
   rather than one circle at the destination.

   A single circle per substep leaves scallops on a diagonal — the collider
   moves further than the circles overlap — and those scallops are exactly what
   the collision resolver then bounces off. Sampling along the displacement at
   less than a radius apart makes the swept shape a capsule, and a capsule has
   no teeth to catch on.

   Returns the cells removed. */
static int PlayerCarveSweep(Player *player, World *world, Vector2 from,
                            Vector2 to, float radius, float heat)
{
    float deltaX = to.x - from.x;
    float deltaY = to.y - from.y;
    float distance = sqrtf(deltaX * deltaX + deltaY * deltaY);
    float spacing = radius * 0.6f;
    int samples = 1;
    int removed = 0;
    int index;

    if (spacing > 0.001f && distance > spacing) {
        samples = (int)ceilf(distance / spacing) + 1;
        if (samples > PLAYER_MAX_MOVE_SUBSTEPS) {
            samples = PLAYER_MAX_MOVE_SUBSTEPS;
        }
    }
    for (index = 0; index < samples; ++index) {
        float amount = samples > 1 ? (float)index / (float)(samples - 1) : 1.0f;
        Vector2 at = {from.x + deltaX * amount, from.y + deltaY * amount};
        int cut = WorldDrillCircle(world, (int)floorf(at.x), (int)floorf(at.y),
                                   (int)ceilf(radius));

        removed += cut;
        PlayerHeatTunnel(world, at, radius, heat);
    }
    player->drillPosition = to;
    return removed;
}

/* The speed a cut costs, per cell of travel through material.

   Charged by distance rather than by cells removed. The tunnel is far wider
   than it was, so a cell count would make every widening of the drill a
   slowdown — and the player would feel the upgrade as a punishment. What
   actually resists is the material being pushed aside, so the weight of it is
   the multiplier, and the stage is the divisor: the harder the boost, the less
   of its speed the ground takes. */
static float PlayerDrillDrag(const Player *player, CellMaterial material)
{
    float drag = player->drillResistance * MaterialAt(material)->density;

    switch (player->boostStage) {
    case PLAYER_BOOST_STAGE_THREE: return drag * 0.18f;
    case PLAYER_BOOST_STAGE_TWO: return drag * 0.34f;
    case PLAYER_BOOST_STAGE_ONE: return drag * 0.60f;
    default: return drag;
    }
}

/* The slow half of the drill: only where the player is actually pressed into
   material, cutting around the collider rather than ahead of it.

   Without this a boost begun from rest against a wall could never start: the
   collision zeroes the blocked component every frame, so the speed can never
   climb to the threshold that would have cut the wall away, and a cut placed
   ahead of the collider lands inside the hole it already made while the rim
   that is actually blocking survives. Still per substep, because it is a
   reaction to a collision rather than a path being cleared. */
static void PlayerDrillPressed(Player *player, World *world, Vector2 nextPosition)
{
    float speed = sqrtf(player->velocity.x * player->velocity.x +
                        player->velocity.y * player->velocity.y);
    int destroyed;

    if (!player->boosting || speed < 1.0f || speed >= player->drillSpeed) {
        return;
    }
    if (!PlayerCollidesAt(player, world, nextPosition)) {
        return;
    }
    destroyed = PlayerCutFree(player, world, nextPosition);
    if (destroyed > 0) {
        player->drilledCells += destroyed;
        player->drillPosition = nextPosition;
    }
}

/* The fast half: the whole corridor this frame will travel down, cut once.

   It used to be cut once per collision substep, which at speed meant sixty-odd
   overlapping carves of very nearly the same ground — the substeps are half a
   cell apart and the drill is eight cells across, so each one re-scanned almost
   exactly what the last had already cleared. Cutting the frame's path in a
   single sweep does the same job for a fifteenth of the work, and the substeps
   that follow simply travel down a corridor that is already open. */
static void PlayerDrillPath(Player *player, World *world, float deltaTime)
{
    float speed = sqrtf(player->velocity.x * player->velocity.x +
                        player->velocity.y * player->velocity.y);
    Vector2 direction;
    Vector2 lead;
    float radius;
    float travelled;
    int destroyed;

    if (!player->boosting || speed < player->drillSpeed) {
        return;
    }

    direction = (Vector2){player->velocity.x / speed, player->velocity.y / speed};
    radius = PlayerDrillRadius(player);
    /* Reaching past where the frame ends, so the corridor is already open when
       the collision resolver looks at it. Clearing only as far as the collider
       will reach leaves its leading edge against fresh material every single
       substep, and every one of those is a bounce off a wall that was about to
       be cut anyway. */
    lead = (Vector2){player->position.x + player->velocity.x * deltaTime +
                         direction.x * radius,
                     player->position.y + player->velocity.y * deltaTime +
                         direction.y * radius};
    PlayerRecordDrillMaterial(player, world, lead);
    destroyed = PlayerCarveSweep(player, world, player->position, lead, radius,
                                 player->drillHeat);
    if (destroyed > 0) {
        /* Cutting terrain costs speed, but never enough to fall under the drill
           threshold: a boost that stalls would leave the player buried. */
        float floorSpeed = player->drillSpeed * 1.05f;
        float slowed;
        float scale;

        if (player->boostStage == PLAYER_BOOST_STAGE_TWO) {
            floorSpeed = fmaxf(floorSpeed, player->boostStageOneSpeed * 0.72f);
        } else if (player->boostStage == PLAYER_BOOST_STAGE_THREE) {
            floorSpeed = fmaxf(floorSpeed, player->sonicSpeed * 0.70f);
        }
        travelled = sqrtf((lead.x - player->position.x) *
                              (lead.x - player->position.x) +
                          (lead.y - player->position.y) *
                              (lead.y - player->position.y));
        slowed = speed * expf(-PlayerDrillDrag(player, player->drillMaterial) *
                              travelled);

        if (slowed < floorSpeed) {
            slowed = fminf(speed, floorSpeed);
        }
        scale = slowed / speed;
        player->velocity.x *= scale;
        player->velocity.y *= scale;
        player->drilledCells += destroyed;
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

/* Thrust, split into the part that acts along the direction of travel and the
   part that acts across it, because those two are not the same request.

   Pushing forward is acceleration. Pushing back is braking, and it is worth
   more than acceleration is: committing to six hundred cells a second has to
   feel survivable, and it only does if the player knows they can shed that
   speed faster than they gained it. Pushing sideways is steering, and steering
   is what speed takes away — the faster the flight, the wider the arc, down to
   a fraction that is deliberately never zero.

   At a standstill there is no direction of travel to decompose against, and the
   input is simply thrust. */
static void PlayerApplyThrust(Player *player, Vector2 input, float acceleration,
                              float speed, float deltaTime)
{
    Vector2 forward;
    float along;
    Vector2 across;
    float authority;
    float alongDelta;

    if (speed < 1.0f) {
        player->velocity.x += input.x * acceleration * deltaTime;
        player->velocity.y += input.y * acceleration * deltaTime;
        return;
    }

    forward = (Vector2){player->velocity.x / speed, player->velocity.y / speed};
    along = input.x * forward.x + input.y * forward.y;
    across = (Vector2){input.x - forward.x * along, input.y - forward.y * along};

    /* Full steering at rest, `turnAuthorityAtHighSpeed` of it at the top of the
       boost range, straight line between. */
    authority = player->boostMaxSpeed > 0.001f
                    ? Clamp(speed / player->boostMaxSpeed, 0.0f, 1.0f)
                    : 0.0f;
    authority = 1.0f + (player->turnAuthorityAtHighSpeed - 1.0f) * authority;

    alongDelta = along * acceleration * deltaTime;
    if (along < 0.0f) {
        alongDelta *= player->brakingAuthority;
        /* Braking stops at a standstill. Without this a hard enough brake in a
           long frame reads as an instant reversal, which is the one thing
           momentum is supposed to make impossible. */
        if (alongDelta < -speed) {
            alongDelta = -speed;
        }
    }

    player->velocity.x += forward.x * alongDelta +
                          across.x * acceleration * authority * deltaTime;
    player->velocity.y += forward.y * alongDelta +
                          across.y * acceleration * authority * deltaTime;
}

/* Moving through something costs speed in proportion to how heavy it is. The
   rule is the material table's, not a list of names: anything the player can
   pass through slows them by its own density, so water slows, lava slows
   harder, and a gas barely registers. One cell read per frame. */
static void PlayerApplyFluidDrag(Player *player, const World *world,
                                 float deltaTime)
{
    CellMaterial material = WorldGetCell(world, (int)floorf(player->position.x),
                                         (int)floorf(player->position.y));
    float density = MaterialAt(material)->density;
    float damping;

    if (WorldMaterialIsSolid(material) || density <= 0.0f) {
        return;
    }
    damping = expf(-player->fluidDrag * density * deltaTime);
    player->velocity.x *= damping;
    player->velocity.y *= damping;
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
        PlayerApplyThrust(player, input, acceleration, velocityLength, deltaTime);
    }
    PlayerApplyFluidDrag(player, world, deltaTime);

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

    /* The corridor first, once, for the whole frame. Everything after this is
       movement through space that is already clear. */
    PlayerDrillPath(player, world, deltaTime);

    moveX = player->velocity.x * deltaTime;
    moveY = player->velocity.y * deltaTime;
    /* Half a cell per step, so the collider — more than three cells across —
       can never straddle a wall between two tests. The cap bounds the work a
       single frame can ask for; at the speeds the boost can reach it is not
       met, and if it ever were the step would still be well under the
       collider's own size. */
    moveSteps = (int)ceilf(fmaxf(fabsf(moveX), fabsf(moveY)) / 0.5f);
    if (moveSteps < 1) {
        moveSteps = 1;
    }
    if (moveSteps > PLAYER_MAX_MOVE_SUBSTEPS) {
        moveSteps = PLAYER_MAX_MOVE_SUBSTEPS;
    }
    stepTime = deltaTime / (float)moveSteps;

    for (step = 0; step < moveSteps; ++step) {
        Vector2 candidate = player->position;
        Vector2 nextPosition = {
            player->position.x + player->velocity.x * stepTime,
            player->position.y + player->velocity.y * stepTime
        };
        float incomingSpeed;

        PlayerDrillPressed(player, world, nextPosition);
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
