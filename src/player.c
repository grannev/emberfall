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
    player->thrust = (Vector2){0.0f, 0.0f};
    player->impactPosition = position;
    player->impactNormal = (Vector2){0.0f, 0.0f};
    player->drillPosition = position;
    player->drillMaterial = MATERIAL_EMPTY;
    /* Half as fast again as when the view was 320 cells across and the
       character eight tall: the view is now 426 across and the character
       sixteen, and the flight has to cross the screen as it did. */
    player->acceleration = 360.0f;
    player->maxSpeed = 170.0f;
    player->boostAcceleration = 1060.0f;
    player->boostSpeed = 560.0f;
    player->boostDrag = 0.24f;
    player->sonicSpeed = 520.0f;
    player->boostGrace = 0.0f;
    player->drillSpeed = 135.0f;
    player->drillHeat = 0.72f;
    /* A third of the steering left at top speed. Enough to pick a line through
       a cavern at the boost ceiling, not enough to turn a corner: the cost of
       going that fast is that the world has to be read further ahead. */
    player->turnAuthorityAtHighSpeed = 0.34f;
    /* Braking beats accelerating, which is what makes committing to speed feel
       safe rather than reckless. */
    player->brakingAuthority = 2.6f;
    player->drag = 1.1f;
    player->restitution = 0.34f;
    /* A capsule round the drawn figure, from the soles at seven body units
       under the hips to the helmet at seven over them. The collider and the
       body have to agree or the character stands with his shins in the
       ground. */
    player->radius = 2.3f * PLAYER_BODY_SCALE;
    player->halfHeight = 4.7f * PLAYER_BODY_SCALE;
    player->mode = PLAYER_MODE_FLY;
    player->jumpPressed = false;
    player->jumpHeld = false;
    player->grounded = false;
    player->airTime = 0.0f;
    player->jumped = false;
    player->flightTapTimer = -1.0f;
    player->runHeld = false;
    player->walkPhase = 0.0f;
    player->impactStrength = 0.0f;
    player->impactTimer = 0.0f;
    player->landingSpeed = 0.0f;
    player->landingTimer = 0.0f;
    player->landingPosition = position;
    player->landingOnBody = false;
    player->animationTime = 0.0f;
    player->leanAmount = 0.0f;
    player->pose = PLAYER_POSE_FLY;
    player->poseTimer = 0.0f;
    player->boostBurstTimer = 0.0f;
    player->drilledCells = 0;
    player->boostEngaged = false;
    player->sonicBreakArmed = true;
    player->facingRight = true;
    player->thrusting = false;
    player->boosting = false;
}

float PlayerExtent(const Player *player)
{
    return player != NULL ? player->halfHeight + player->radius : 0.0f;
}

Vector2 PlayerFeet(const Player *player)
{
    if (player == NULL) {
        return (Vector2){0.0f, 0.0f};
    }
    return (Vector2){player->position.x, player->position.y + PlayerExtent(player)};
}

/* The capsule against the cells: each solid cell's box is measured against
   the collider's vertical segment, and it overlaps when that distance is under
   the radius. Two clamps and a compare a cell, like the circle before it. */
bool PlayerCollidesAt(const Player *player, const World *world, Vector2 position)
{
    float extent = player->halfHeight + player->radius;
    int minimumX = (int)floorf(position.x - player->radius);
    int maximumX = (int)floorf(position.x + player->radius);
    int minimumY = (int)floorf(position.y - extent);
    int maximumY = (int)floorf(position.y + extent);
    float top = position.y - player->halfHeight;
    float bottom = position.y + player->halfHeight;
    int y;

    for (y = minimumY; y <= maximumY; ++y) {
        int x;

        for (x = minimumX; x <= maximumX; ++x) {
            float nearestX;
            float segmentY;
            float nearestY;
            float dx;
            float dy;

            CellMaterial material = WorldGetCell(world, x, y);

            /* Plants stand behind the character, like the back wall: he
               walks and flies through a tree, and it is the tree that pays
               for it (PlayerBrushFlora), never him. */
            if (!WorldMaterialIsSolid(material) || MaterialIsFlora(material)) {
                continue;
            }

            /* The point of the segment nearest the box, then the point of the
               box nearest that. */
            segmentY = Clamp((float)y + 0.5f, top, bottom);
            nearestX = Clamp(position.x, (float)x, (float)x + 1.0f);
            nearestY = Clamp(segmentY, (float)y, (float)y + 1.0f);
            dx = position.x - nearestX;
            dy = segmentY - nearestY;
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
    int base = (int)ceilf(PlayerExtent(player));
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

Vector2 PlayerBodyUp(const Player *player)
{
    float uprightAngle = -PI * 0.5f;
    float speed;
    Vector2 travel = {0.0f, -1.0f};
    float lean;
    float travelAngle;
    float turn;

    if (player == NULL) {
        return (Vector2){0.0f, -1.0f};
    }
    if (player->mode == PLAYER_MODE_WALK) {
        float tilt = Clamp(player->velocity.x / PLAYER_RUN_SPEED, -1.0f, 1.0f) * 0.14f;
        return (Vector2){sinf(tilt), -cosf(tilt)};
    }
    lean = Clamp(player->leanAmount, 0.0f, 1.0f);
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    if (speed > 0.001f) {
        travel = (Vector2){player->velocity.x / speed, player->velocity.y / speed};
    }
    travelAngle = atan2f(travel.y, travel.x);
    turn = travelAngle - uprightAngle;
    while (turn > PI) turn -= 2.0f * PI;
    while (turn < -PI) turn += 2.0f * PI;
    /* Straight down has two equally short rotations. Keep the side chosen by
       the last horizontal motion instead of letting float noise flip the body
       between left and right. */
    if (fabsf(fabsf(turn) - PI) < 0.0001f) {
        turn = player->facingRight ? PI : -PI;
    }
    return (Vector2){cosf(uprightAngle + turn * lean),
                     sinf(uprightAngle + turn * lean)};
}

float PlayerStride(const Player *player)
{
    float pace;

    if (player == NULL) {
        return 14.0f;
    }
    pace = (fabsf(player->velocity.x) - PLAYER_WALK_SPEED) /
           (PLAYER_RUN_SPEED - PLAYER_WALK_SPEED);
    return 14.0f + 9.0f * Clamp(pace, 0.0f, 1.0f);
}

Vector2 PlayerBodyOrigin(const Player *player)
{
    Vector2 up = PlayerBodyUp(player);
    float bob;
    if (player == NULL) return (Vector2){0.0f, 0.0f};
    if (player->mode == PLAYER_MODE_WALK) {
        float moving = player->grounded ? Clamp(fabsf(player->velocity.x) / 20.0f, 0.0f, 1.0f) : 0.0f;
        float run = Clamp((fabsf(player->velocity.x) - PLAYER_WALK_SPEED) /
                          (PLAYER_RUN_SPEED - PLAYER_WALK_SPEED), 0.0f, 1.0f);
        bob = -fabsf(sinf(player->walkPhase * 2.0f * PI)) * (0.35f + run * 0.4f) * moving +
              sinf(player->animationTime * 1.8f) * 0.10f * (1.0f - moving);
        if (player->grounded && player->landingTimer > 0.0f) {
            float recovery = player->landingTimer / PLAYER_LANDING_RECOVERY;
            bob -= 3.2f * recovery * recovery;
        }
    } else {
        bob = (1.0f - player->leanAmount) * sinf(player->animationTime * 2.4f) * 0.3f;
    }
    return Vector2Add(player->position, Vector2Scale(up, bob));
}

/* Distance up the body axis from the hips to the visor. The renderer builds the
   head at 6.0 and puts the visor a little to the side of it; this is that same
   point, and both go through this one number. */
/* In body units, like everything else on the figure. */
#define PLAYER_VISOR_ALONG_UP 6.0f
#define PLAYER_VISOR_ALONG_SIDE 0.9f

/* Both palms out, the way the two-handed poses are drawn: the hands sit at the
   shoulders and reach toward the cursor, one a little ahead of the other. */
Vector2 PlayerHandOrigin(const Player *player, Vector2 aim, bool trailing)
{
    Vector2 up;
    Vector2 side;
    Vector2 shoulder;
    float dx;
    float dy;
    float length;
    /* Body units into cells, once, here: every number below is a place on the
       figure, and the figure has a scale. */
    float reach = (trailing ? PLAYER_TWO_HAND_REACH - 0.8f
                            : PLAYER_TWO_HAND_REACH) * PLAYER_BODY_SCALE;
    float across = (trailing ? -0.8f : 1.1f) * PLAYER_BODY_SCALE;
    float along = (trailing ? PLAYER_SHOULDER_UP - 2.2f
                            : PLAYER_SHOULDER_UP - 0.6f) * PLAYER_BODY_SCALE;

    if (player == NULL) {
        return aim;
    }
    up = PlayerBodyUp(player);
    side = (Vector2){-up.y, up.x};

    dx = aim.x - player->position.x;
    dy = aim.y - player->position.y;
    length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        dx = side.x;
        dy = side.y;
        length = 1.0f;
    }
    dx /= length;
    dy /= length;

    if (dx * side.x + dy * side.y < 0.0f) side = Vector2Negate(side);
    {
        Vector2 anchor = PlayerBodyOrigin(player);
        shoulder = (Vector2){anchor.x + up.x * along + side.x * across,
                             anchor.y + up.y * along + side.y * across};
    }
    return (Vector2){shoulder.x + dx * reach, shoulder.y + dy * reach};
}

Vector2 PlayerForceOrigin(const Player *player, Vector2 aim)
{
    Vector2 up;
    Vector2 side;
    Vector2 anchor;
    Vector2 direction;
    float length;

    if (player == NULL) return aim;
    up = PlayerBodyUp(player);
    side = (Vector2){-up.y, up.x};
    direction = Vector2Subtract(aim, player->position);
    length = Vector2Length(direction);
    if (length <= 0.001f) direction = side;
    else direction = Vector2Scale(direction, 1.0f / length);
    if (Vector2DotProduct(direction, side) < 0.0f) side = Vector2Negate(side);
    anchor = PlayerBodyOrigin(player);
    return (Vector2){anchor.x + up.x * (PLAYER_SHOULDER_UP - 0.4f) * PLAYER_BODY_SCALE +
                         side.x * 1.0f * PLAYER_BODY_SCALE +
                         direction.x * 7.4f * PLAYER_BODY_SCALE,
                     anchor.y + up.y * (PLAYER_SHOULDER_UP - 0.4f) * PLAYER_BODY_SCALE +
                         side.y * 1.0f * PLAYER_BODY_SCALE +
                         direction.y * 7.4f * PLAYER_BODY_SCALE};
}

Vector2 PlayerVisorOrigin(const Player *player, Vector2 aim)
{
    Vector2 up;
    Vector2 side;
    Vector2 visor;
    float dx;
    float dy;
    float length;
    float aimAcross;

    if (player == NULL) {
        return aim;
    }
    /* The eye, in the body's own frame — the same frame the character is drawn
       in. Measuring it straight up in world space put the muzzle in the chest
       the moment the character leaned, which at boost speed is always. */
    up = PlayerBodyUp(player);
    side = (Vector2){-up.y, up.x};

    dx = aim.x - player->position.x;
    dy = aim.y - player->position.y;
    length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        dx = side.x;
        dy = side.y;
        length = 1.0f;
    }
    dx /= length;
    dy /= length;
    /* The head turns a little toward the cursor, exactly as it is drawn. */
    aimAcross = dx * side.x + dy * side.y;
    if (aimAcross < 0.0f) side = Vector2Negate(side);

    {
        float alongUp = PLAYER_VISOR_ALONG_UP * PLAYER_BODY_SCALE;
        float alongSide = (PLAYER_VISOR_ALONG_SIDE + 0.15f * fabsf(aimAcross)) *
                          PLAYER_BODY_SCALE;
        Vector2 anchor = PlayerBodyOrigin(player);

        visor = (Vector2){
            anchor.x + up.x * alongUp + side.x * alongSide,
            anchor.y + up.y * alongUp + side.y * alongSide};
    }
    return visor;
}

Vector2 PlayerBeamOrigin(const Player *player, Vector2 aim)
{
    Vector2 visor = PlayerVisorOrigin(player, aim);
    float dx;
    float dy;
    float length;

    if (player == NULL) {
        return aim;
    }
    dx = aim.x - player->position.x;
    dy = aim.y - player->position.y;
    length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        return visor;
    }
    /* Just clear of the visor itself, so the cast ray starts in front of the
       face rather than inside it. */
    {
        float clearance = 1.4f * PLAYER_BODY_SCALE;

        return (Vector2){visor.x + dx / length * clearance,
                         visor.y + dy / length * clearance};
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
    /* Round the whole figure: the collider is a capsule from boots to
       helmet, and a tunnel cut round the waist alone would be one the
       character could not fit down. */
    return PlayerExtent(player) * (player->boosting ? PLAYER_DRILL_WIDTH_BOOST
                                                    : PLAYER_DRILL_WIDTH_IDLE);
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
    /* Rock costs nothing.
     *
     * The cut used to shed speed in proportion to the weight of what it went
     * through, and the effect of it was that the one thing the boost exists for
     * — going through the world rather than around it — was also the one thing
     * that took the boost away. Flight through open air and flight through
     * bedrock are now the same flight, which is what the drill was always
     * meant to promise. */
    if (destroyed > 0) {
        player->drilledCells += destroyed;
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
    authority = player->boostSpeed > 0.001f
                    ? Clamp(speed / player->boostSpeed, 0.0f, 1.0f)
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

/* The one moment the flight still has: engaging the boost.
 *
 * What used to live here was the tier machine — a timer that had to be fed a
 * straight line for a second before the next ceiling unlocked, and that a
 * single corner emptied again. With one speed there is nothing to climb, so all
 * that is left is the kick the engine gives when it lights, which is what the
 * particles, the camera and the sound all key off. */
static void PlayerUpdateBoostEngagement(Player *player, Vector2 input,
                                        bool wasBoosting)
{
    if (!player->boosting || wasBoosting) {
        return;
    }
    player->boostEngaged = true;
    player->velocity.x += input.x * PLAYER_BOOST_ENGAGE_IMPULSE;
    player->velocity.y += input.y * PLAYER_BOOST_ENGAGE_IMPULSE;
    player->boostBurstTimer = PLAYER_BOOST_BURST_TIME;
}

/* ---- on foot ---------------------------------------------------------------- */

/* Which way the character moves this frame: takes off on the second jump in
   the air, lands a flight on a double tap of jump, and flies wherever there
   is nothing to stand on because nothing pulls. Consumes the jump press when
   it is spent on a change of mode. */
static void PlayerUpdateMode(Player *player, float gravityScale, float deltaTime)
{
    if (player->flightTapTimer >= 0.0f) {
        player->flightTapTimer += deltaTime;
        if (player->flightTapTimer > PLAYER_DOUBLE_TAP_TIME) {
            player->flightTapTimer = -1.0f;
        }
    }
    if (gravityScale < 0.05f) {
        player->mode = PLAYER_MODE_FLY;
        return;
    }
    if (player->mode == PLAYER_MODE_WALK) {
        /* In the air past the grace of a ledge: the second jump takes off. */
        if (player->jumpPressed && !player->grounded &&
            player->airTime > PLAYER_COYOTE_TIME) {
            player->mode = PLAYER_MODE_FLY;
            player->jumpPressed = false;
            /* A lift on take-off, so the flight starts rather than the fall
               simply stopping. */
            if (player->velocity.y > -60.0f) {
                player->velocity.y = -60.0f;
            }
        }
        return;
    }
    if (player->jumpPressed) {
        if (player->flightTapTimer >= 0.0f) {
            /* The second tap: back on foot, falling. */
            player->mode = PLAYER_MODE_WALK;
            player->flightTapTimer = -1.0f;
            player->grounded = false;
            player->airTime = PLAYER_COYOTE_TIME + 0.001f;
            player->jumped = true;
        } else {
            player->flightTapTimer = 0.0f;
        }
        player->jumpPressed = false;
    }
}

/* Lands the collider on whatever is under it, within a step: walking down a
   slope of cells keeps the feet on it instead of stepping off each cell into
   the air. */
static bool PlayerSnapDown(Player *player, const World *world)
{
    int drop;

    for (drop = 1; drop <= PLAYER_STEP_HEIGHT; ++drop) {
        Vector2 candidate = {player->position.x, player->position.y + (float)drop};

        if (PlayerCollidesAt(player, world, candidate)) {
            return false;
        }
        if (PlayerCollidesAt(player, world,
                             (Vector2){candidate.x, candidate.y + 0.6f})) {
            player->position = candidate;
            return true;
        }
    }
    return false;
}

void PlayerRecordLanding(Player *player, float speed, Vector2 contact, bool onBody)
{
    if (player == NULL || speed < PLAYER_HEAVY_LANDING_SPEED ||
        player->landingTimer > 0.0f) return;
    player->landingSpeed = speed;
    player->landingPosition = contact;
    player->landingOnBody = onBody;
    player->landingTimer = PLAYER_LANDING_RECOVERY;
}

static void PlayerUpdateWalk(Player *player, World *world, Vector2 input,
                             float gravityScale, float deltaTime)
{
    float target = input.x * (player->runHeld ? PLAYER_RUN_SPEED : PLAYER_WALK_SPEED);
    float acceleration = player->grounded ? PLAYER_GROUND_ACCELERATION
                                          : PLAYER_AIR_ACCELERATION;
    float change = target - player->velocity.x;
    float gravity = PLAYER_GRAVITY * gravityScale;
    bool wasGrounded = player->grounded;
    float moveX;
    float moveY;
    int moveSteps;
    int step;
    float stepTime;

    player->boosting = false;
    player->boostGrace = 0.0f;
    player->thrusting = fabsf(input.x) > 0.0f;
    player->thrust = (Vector2){input.x, 0.0f};

    /* Along the ground: toward the speed asked for, quickly on the ground
       and more gently in the air. */
    if (change > acceleration * deltaTime) change = acceleration * deltaTime;
    if (change < -acceleration * deltaTime) change = -acceleration * deltaTime;
    player->velocity.x += change;

    if (player->jumpPressed &&
        (player->grounded || player->airTime <= PLAYER_COYOTE_TIME)) {
        player->velocity.y = -PLAYER_JUMP_SPEED;
        player->grounded = false;
        player->airTime = PLAYER_COYOTE_TIME + 0.001f;
        player->jumped = true;
    }
    /* A committed fall gains weight beyond an ordinary jump's return speed.
       The existing terminal speed and half-cell substep budget still apply. */
    if (player->velocity.y > 250.0f) gravity *= 1.35f;
    player->velocity.y += gravity * deltaTime;
    /* Let go of jump on the way up and the rise is cut short: a tap is a hop,
       a hold is a leap. */
    if (player->jumped && !player->jumpHeld && player->velocity.y < 0.0f) {
        player->velocity.y += gravity * 1.6f * deltaTime;
    }
    if (player->velocity.y > PLAYER_FALL_SPEED_LIMIT) {
        player->velocity.y = PLAYER_FALL_SPEED_LIMIT;
    }

    moveX = player->velocity.x * deltaTime;
    moveY = player->velocity.y * deltaTime;
    moveSteps = (int)ceilf(fmaxf(fabsf(moveX), fabsf(moveY)) / 0.5f);
    if (moveSteps < 1) moveSteps = 1;
    if (moveSteps > PLAYER_MAX_MOVE_SUBSTEPS) moveSteps = PLAYER_MAX_MOVE_SUBSTEPS;
    stepTime = deltaTime / (float)moveSteps;

    for (step = 0; step < moveSteps; ++step) {
        Vector2 candidate = player->position;

        candidate.x += player->velocity.x * stepTime;
        if (!PlayerCollidesAt(player, world, candidate)) {
            player->position.x = candidate.x;
        } else {
            /* Up a step, if there is one within reach and room above it. */
            bool climbed = false;

            if (wasGrounded || player->grounded) {
                int lift;

                for (lift = 1; lift <= PLAYER_STEP_HEIGHT; ++lift) {
                    Vector2 raised = {candidate.x, player->position.y - (float)lift};

                    if (!PlayerCollidesAt(player, world, raised)) {
                        player->position = raised;
                        climbed = true;
                        break;
                    }
                }
            }
            if (!climbed) {
                player->velocity.x = 0.0f;
            }
        }

        candidate = player->position;
        candidate.y += player->velocity.y * stepTime;
        if (!PlayerCollidesAt(player, world, candidate)) {
            player->position.y = candidate.y;
        } else {
            if (player->velocity.y > 0.0f) {
                PlayerRecordLanding(player, player->velocity.y, PlayerFeet(player), false);
                player->grounded = true;
            }
            player->velocity.y = 0.0f;
        }
    }

    player->grounded = PlayerCollidesAt(
        player, world, (Vector2){player->position.x, player->position.y + 0.6f});
    if (!player->grounded && wasGrounded && player->velocity.y >= 0.0f &&
        !player->jumpPressed) {
        player->grounded = PlayerSnapDown(player, world);
    }
    if (player->grounded) {
        player->airTime = 0.0f;
        player->jumped = false;
        if (player->velocity.y > 0.0f) {
            player->velocity.y = 0.0f;
        }
    } else {
        player->airTime += deltaTime;
    }

    player->position.y = Clamp(player->position.y, PlayerExtent(player),
                               (float)world->height - PlayerExtent(player));
    if (player->grounded) {
        player->walkPhase += fabsf(player->velocity.x) * deltaTime /
                             PlayerStride(player);
        player->walkPhase -= floorf(player->walkPhase);
    }
    player->animationTime += deltaTime;
    if (fabsf(input.x) > 0.0f) {
        player->facingRight = input.x > 0.0f;
    }
    player->leanAmount += (0.0f - player->leanAmount) * (1.0f - expf(-12.0f * deltaTime));
    player->poseTimer = fmaxf(0.0f, player->poseTimer - deltaTime);
    if (player->poseTimer <= 0.0f) {
        player->pose = PLAYER_POSE_FLY;
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
    player->landingSpeed = 0.0f;
    player->landingTimer = fmaxf(0.0f, player->landingTimer - deltaTime);
    player->impactNormal = (Vector2){0.0f, 0.0f};
    player->impactTimer = fmaxf(0.0f, player->impactTimer - deltaTime);
    player->drilledCells = 0;
    player->boostEngaged = false;
    player->boostBurstTimer = fmaxf(0.0f, player->boostBurstTimer - deltaTime);
    player->thrusting = false;
    player->thrust = (Vector2){0.0f, 0.0f};
    PlayerUpdateMode(player, WorldGravityScaleAt(world, player->position.y), deltaTime);
    if (player->mode == PLAYER_MODE_WALK) {
        PlayerUpdateWalk(player, world, input,
                         WorldGravityScaleAt(world, player->position.y), deltaTime);
        player->jumpPressed = false;
        return;
    }
    player->grounded = false;
    player->jumpPressed = false;
    inputLength = sqrtf(input.x * input.x + input.y * input.y);
    if (inputLength > 0.0f) {
        input.x /= inputLength;
        input.y /= inputLength;
        player->thrusting = true;
        player->thrust = input;
    }
    /* Boost outlives the directional input for a moment, so letting go of WASD
       inside a tunnel coasts out instead of dropping the drill into a wall. */
    if (boostHeld && player->thrusting) {
        player->boostGrace = 0.14f;
    } else {
        player->boostGrace = fmaxf(0.0f, player->boostGrace - deltaTime);
    }
    {
        bool wasBoosting = player->boosting;

        player->boosting =
            boostHeld && (player->thrusting || player->boostGrace > 0.0f);
        PlayerUpdateBoostEngagement(player, input, wasBoosting);
    }
    velocityLength = sqrtf(player->velocity.x * player->velocity.x +
                           player->velocity.y * player->velocity.y);
    acceleration = player->boosting ? player->boostAcceleration
                                    : player->acceleration;
    speedLimit = player->boosting ? player->boostSpeed : player->maxSpeed;
    damping = player->boosting ? player->boostDrag : player->drag;
    if (player->thrusting) {
        PlayerApplyThrust(player, input, acceleration, velocityLength, deltaTime);
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
            float boostSpan = player->boostSpeed - player->drillSpeed * 0.5f;
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

    /* Only held in vertically: the world wraps across, and the game moves
       the character back into the map a whole width at a time when it
       crosses the seam (GameUpdate), with everything around it. */
    player->position.y = Clamp(player->position.y, PlayerExtent(player),
                               (float)world->height - PlayerExtent(player));

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

int PlayerBrushFlora(Player *player, World *world)
{
    float speed;
    float extent;
    float share;
    int firstX;
    int lastX;
    int firstY;
    int lastY;
    int minimumX = 0;
    int minimumY = 0;
    int maximumX = -1;
    int maximumY = -1;
    int removed = 0;
    int y;

    if (player == NULL || world == NULL) {
        return 0;
    }
    player->brushedLeaves = 0;
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    if (speed < PLAYER_BRUSH_SPEED) {
        return 0;
    }
    /* A run knocks a few leaves off; a flight at cruise strips a path; the
       boost's drill takes the rest, wood and all. */
    share = Clamp((speed - PLAYER_BRUSH_SPEED) / 220.0f, 0.12f, 1.0f);
    extent = PlayerExtent(player);
    firstX = (int)floorf(player->position.x - player->radius - 1.0f);
    lastX = (int)floorf(player->position.x + player->radius + 1.0f);
    firstY = (int)floorf(player->position.y - extent);
    lastY = (int)floorf(player->position.y + extent);
    for (y = firstY; y <= lastY; ++y) {
        int x;

        for (x = firstX; x <= lastX; ++x) {
            uint32_t hash;

            if (WorldGetCell(world, x, y) != MATERIAL_LEAF) {
                continue;
            }
            /* Which leaves go is a hash of where they are and when, so a
               replay strips the same ones. */
            hash = (uint32_t)x * 0x9e3779b1u ^ (uint32_t)y * 0x85ebca77u ^
                   (uint32_t)world->tick * 0xc2b2ae3du;
            hash ^= hash >> 15;
            hash *= 0x2c1b3c6du;
            hash ^= hash >> 12;
            if ((float)(hash & 0xffffu) / 65535.0f > share) {
                continue;
            }
            WorldSetCell(world, x, y, MATERIAL_EMPTY);
            ++removed;
            if (maximumX < minimumX) {
                minimumX = maximumX = x;
                minimumY = maximumY = y;
            } else {
                if (x < minimumX) minimumX = x;
                if (x > maximumX) maximumX = x;
                if (y < minimumY) minimumY = y;
                if (y > maximumY) maximumY = y;
            }
        }
    }
    if (removed > 0) {
        /* The pass takes leaves at random, and a leaf whose neighbours all
           went is a speck hanging in the air with nothing to be part of:
           it goes with them. */
        for (y = minimumY - 1; y <= maximumY + 1; ++y) {
            int x;

            for (x = minimumX - 1; x <= maximumX + 1; ++x) {
                int offsetY;
                bool joined = false;

                if (WorldGetCell(world, x, y) != MATERIAL_LEAF) {
                    continue;
                }
                for (offsetY = -1; offsetY <= 1 && !joined; ++offsetY) {
                    int offsetX;

                    for (offsetX = -1; offsetX <= 1; ++offsetX) {
                        if ((offsetX != 0 || offsetY != 0) &&
                            MaterialIsFlora(WorldGetCell(world, x + offsetX, y + offsetY))) {
                            joined = true;
                            break;
                        }
                    }
                }
                if (!joined) {
                    WorldSetCell(world, x, y, MATERIAL_EMPTY);
                    ++removed;
                }
            }
        }
        /* A clump the path cut loose from its branch comes down as a body,
           the ordinary way. */
        WorldRecordDestruction(world, minimumX, minimumY, maximumX, maximumY);
        player->brushedLeaves = removed;
    }
    return removed;
}
