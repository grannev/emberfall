#ifndef PLAYER_H
#define PLAYER_H

#include <raylib.h>

#include "world.h"

/* What the character is doing with their hands. Drawing needs it, and only the
   caller knows which power is firing. */
typedef enum PlayerPose {
    PLAYER_POSE_FLY = 0,
    PLAYER_POSE_LASER,
    PLAYER_POSE_CHILL,
    PLAYER_POSE_BLAST,
    /* Both palms flat on something solid, shoulder-width apart, leaning into
       it. Held by whatever is being pushed rather than by an ability. */
    PLAYER_POSE_PUSH
} PlayerPose;

/* How the character moves: on foot, with gravity, or flying. A double jump
   takes off; a double tap of jump in the air lands the flight and lets the
   character fall again. Where nothing pulls — above the space line — there
   is no walking, and the character flies whatever the mode says. */
typedef enum PlayerMode {
    PLAYER_MODE_FLY = 0,
    PLAYER_MODE_WALK,
} PlayerMode;

/* On foot, in cells and seconds. The character is sixteen cells tall: a walk
   is four of its heights a second, a run seven, and a jump clears about one
   and a half of them. */
#define PLAYER_WALK_SPEED 64.0f
#define PLAYER_RUN_SPEED 118.0f
#define PLAYER_GROUND_ACCELERATION 760.0f
#define PLAYER_AIR_ACCELERATION 420.0f
#define PLAYER_GRAVITY 620.0f
#define PLAYER_JUMP_SPEED 205.0f
#define PLAYER_FALL_SPEED_LIMIT 560.0f
/* The tallest step the character walks up without jumping, in cells: the
   world is cells, and ground that rises a cell at a time is a slope, not a
   series of walls. */
#define PLAYER_STEP_HEIGHT 4
/* Seconds after walking off an edge in which a jump still counts as from the
   ground, and seconds within which two taps of jump in flight land it. */
#define PLAYER_COYOTE_TIME 0.1f
#define PLAYER_DOUBLE_TAP_TIME 0.32f

/* Most pieces one frame of movement is ever broken into. The step is half a
   cell, and the largest displacement a frame can produce is the top speed times
   the longest step the game will take, so this covers it with room to spare:
   the cap exists to bound the work, not to be reached. */
#define PLAYER_MAX_MOVE_SUBSTEPS 64

/* How wide a tunnel the boost cuts, as a multiple of the player's own radius. A
   tunnel the size of the collider reads as a worm hole; what a body moving this
   fast should leave is a corridor with room around it. The idle figure is what
   a player pressed into a wall below the drill threshold scrapes free. */
#define PLAYER_DRILL_WIDTH_IDLE 1.02f
#define PLAYER_DRILL_WIDTH_BOOST 1.15f

/* The shove the engine gives the moment it lights, in cells per second. It is
   what makes engaging the boost a decision the player feels rather than a
   number quietly changing: the ceiling alone would be reached a third of a
   second later and read as the same flight, slightly faster. */
#define PLAYER_BOOST_ENGAGE_IMPULSE 125.0f
/* How long the ring behind the character lasts after the engine lights. Shared
   with the renderer, which draws the ring's progress from it: two copies of one
   duration is a ring that finishes before or after the burst it belongs to. */
#define PLAYER_BOOST_BURST_TIME 0.34f

typedef struct Player {
    Vector2 position;
    Vector2 velocity;
    /* This frame's movement input, normalised, or zero when there is none.
       Kept because it is the one thing that says which way the character is
       *pushing* as opposed to which way they happen to be drifting — and a
       body braced against needs exactly that. Velocity is no substitute: the
       moment the player is pressed against a rock, the collision has taken
       their velocity away and the thrust is all that is left. */
    Vector2 thrust;
    Vector2 impactPosition;
    Vector2 impactNormal;
    Vector2 drillPosition;
    /* What the drill is biting into, sampled before the cut. Audio pitches the
       grind by it, so rock does not sound like dirt. */
    CellMaterial drillMaterial;
    float acceleration;
    float maxSpeed;
    /* One boost, one speed.
     *
     * The flight used to climb through three tiers, each with its own
     * acceleration, ceiling and drag, reached by holding a straight line for
     * long enough. It meant the same key did three different things depending
     * on how long ago it was pressed, and the tier the player actually flew in
     * was whichever one the last corner had knocked them back to. What is left
     * is the middle tier, entered the moment boost is held: the speed is
     * whatever the player asks for, immediately, every time. */
    float boostAcceleration;
    float boostSpeed;
    float boostDrag;
    /* Where the flight starts drawing a shock cone. Just under the boost
       ceiling, so it is the reward for a clean straight run rather than a
       fourth tier by another name. */
    float sonicSpeed;
    float boostGrace;
    float drillSpeed;
    /* Fraction of a material's own phase threshold the tunnel wall is heated
       to, at the centre of the cut. Expressed as a fraction rather than a
       temperature so that rock and dirt glow alike without either being pushed
       over its own transition — the point is a visible hot wall, not automatic
       lava. */
    float drillHeat;
    /* How much of the steering input survives at top speed, as a fraction of
       what it is worth at rest. Thrust across the direction of travel is scaled
       by it, so a turn at six hundred cells a second is a wide arc rather than
       a right angle — and never a total loss of control, which is what a
       fraction rather than a cutoff guarantees. */
    float turnAuthorityAtHighSpeed;
    /* Thrust straight back along the direction of travel is worth this many
       times an ordinary push. It is a multiplier rather than an absolute rate
       so that braking out of stage three is as forceful as getting into it: the
       harder the engine, the harder it can also stop. */
    float brakingAuthority;
    float drag;
    float restitution;
    /* The collider: a vertical capsule, a segment `halfHeight` either side of
       the position with `radius` round it — from the soles of the boots to the
       top of the helmet. The position is the hips. */
    float radius;
    float halfHeight;
    PlayerMode mode;
    /* This frame's jump input, set by the caller before PlayerUpdate: the
       press edge and whether the button is held (a short tap is a short hop). */
    bool jumpPressed;
    bool jumpHeld;
    /* Standing on something, and for how long it has been since that was
       last true. */
    bool grounded;
    float airTime;
    /* A jump was taken from the ground and the next one in the air takes off. */
    bool jumped;
    /* Seconds since the last tap of jump in flight, for the double tap that
       lands it; negative when there is none pending. */
    float flightTapTimer;
    /* Walking wants to run: Shift on foot. */
    bool runHeld;
    /* The walk cycle, advancing with ground speed. */
    float walkPhase;
    float impactStrength;
    float impactTimer;
    float animationTime;
    /* 0 upright and hovering, 1 laid out flat along the direction of travel.
       Smoothed, so the change of posture reads as the character shifting their
       weight rather than as a sprite swap. */
    float leanAmount;
    PlayerPose pose;
    float poseTimer;
    float boostTrailTimer;
    float boostBurstTimer;
    int drilledCells;
    /* One-frame event consumed by main for particles, audio and camera kick:
       the boost has just been engaged from rest. */
    bool boostEngaged;
    bool facingRight;
    bool thrusting;
    bool boosting;
    bool boostTrailEmitted;
} Player;

void PlayerInit(Player *player, Vector2 position);
/* How far the collider reaches from the position up and down: halfHeight
   plus radius. */
float PlayerExtent(const Player *player);
/* Where the soles of the boots are. */
Vector2 PlayerFeet(const Player *player);
/* True when the collider would overlap solid ground at `position`. */
bool PlayerCollidesAt(const Player *player, const World *world, Vector2 position);
void PlayerUpdate(Player *player, World *world, Vector2 input, bool boostHeld,
                  float deltaTime);
void PlayerResolveWorldCollision(Player *player, World *world);
/* Adds velocity directly. Used for recoil, where the direction is known and no
   falloff applies. */
void PlayerApplyImpulse(Player *player, Vector2 impulse);

/* True while the boost is actually cutting rather than merely running. Shared
   so that detached terrain can be cut by the same drill that cuts the static
   world: a slab that stopped a player who is boring through bedrock beside it
   would be the odd one out, not the rule. */
bool PlayerIsDrilling(const Player *player);
/* Radius of the tunnel the boost cuts. */
float PlayerDrillRadius(const Player *player);

/* Where a beam leaves the character: the eye, pushed just clear of the collider
   along the line of aim.

   One helper, used by the gameplay ray and by the drawn beam alike. Two
   derivations of "where the laser starts" is two chances for them to disagree,
   and a beam that is cast from the chest but drawn from the head reads as a
   bug the moment the player aims down. */
Vector2 PlayerBeamOrigin(const Player *player, Vector2 aim);

/* The visor itself, without the step forward PlayerBeamOrigin adds.
 *
 * The gameplay ray has to start clear of the face or it would immediately hit
 * the head it came out of; the *drawn* beam has to start on the face or it
 * looks detached from it, which is exactly what a beam beginning a cell and a
 * half in front of the eyes looked like. One derivation, two uses: whatever
 * moves the eye moves both. */
Vector2 PlayerVisorOrigin(const Player *player, Vector2 aim);

/* Cells per body unit.
 *
 * The figure is written in body units — the shoulders at 3.2, the visor at 6.0,
 * the feet seven below the hips — and this is the one number that turns those
 * into cells. The view is 426 cells across, the scale Noita shows its world
 * at, and the character stands some sixteen cells tall in it: tall enough to
 * carry a helmet, a belt, boots and a gait, small enough that a tree stands
 * over him and a cavern is a place rather than a corridor.
 *
 * It has to be shared rather than living in the renderer, because the beams are
 * cast from the same body frame they are drawn in, and the collider is the
 * same height as the drawing. A scale applied to the drawing alone would put
 * the eyes in one place and the laser's muzzle in another — the exact bug the
 * visor origin already had once. */
#define PLAYER_BODY_SCALE 1.2f

/* Where the shoulders sit along the body axis, and how far a two-handed power
   reaches past them, in body units. Here rather than in the renderer because
   the telekinetic hold is drawn from the hands and cast from the hands: the
   moment those are two different numbers, the beam leaves from a point where no
   hand is, which is the bug the visor origin already had once. */
#define PLAYER_SHOULDER_UP 3.2f
#define PLAYER_TWO_HAND_REACH 4.6f

/* World position of one hand of a two-handed reach toward `aim`. `trailing`
   picks the far hand; the near one leads. Built in the same body frame as
   PlayerBeamOrigin, so both rotate with the lean together. */
Vector2 PlayerHandOrigin(const Player *player, Vector2 aim, bool trailing);

/* The body axis: hips to head. Straight up when hovering, turning toward the
   direction of travel as the character leans, so at full boost it *is* the
   direction of travel.

   It lives here rather than in the renderer because the beam origin needs it
   too, and the head has to be in one place: a visor drawn six cells up the body
   axis while the laser is cast from a point measured straight up in world space
   are two different heads, and the player sees both. */
Vector2 PlayerBodyUp(const Player *player);
/* Cells the body travels over one whole walking cycle, two steps: longer as
   the walk becomes a run. The gait advances by distance over this, and the
   renderer swings the legs by a quarter of it either way, so a planted foot
   stays where it was put. */
float PlayerStride(const Player *player);
/* Holds a pose for `holdTime` seconds. Held powers refresh it every frame with a
   short time; a one-shot like the force blast asks for the length of its own
   animation. */
void PlayerSetPose(Player *player, PlayerPose pose, float holdTime);

#endif
