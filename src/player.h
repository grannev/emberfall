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
    PLAYER_POSE_BLAST
} PlayerPose;

typedef enum PlayerBoostStage {
    PLAYER_BOOST_NONE = 0,
    PLAYER_BOOST_STAGE_ONE,
    PLAYER_BOOST_STAGE_TWO,
    PLAYER_BOOST_STAGE_THREE
} PlayerBoostStage;

/* Most pieces one frame of movement is ever broken into. The step is half a
   cell, and the largest displacement a frame can produce is the top speed times
   the longest step the game will take, so this covers it with room to spare:
   the cap exists to bound the work, not to be reached. */
#define PLAYER_MAX_MOVE_SUBSTEPS 64

/* How wide a tunnel each boost stage cuts, as a multiple of the player's own
   radius. A tunnel the size of the collider reads as a worm hole; what a body
   moving this fast should leave is a corridor with room around it. */
#define PLAYER_DRILL_WIDTH_IDLE 1.15f
#define PLAYER_DRILL_WIDTH_STAGE_ONE 1.55f
#define PLAYER_DRILL_WIDTH_STAGE_TWO 2.00f
#define PLAYER_DRILL_WIDTH_STAGE_THREE 2.45f

typedef struct Player {
    Vector2 position;
    Vector2 velocity;
    Vector2 impactPosition;
    Vector2 impactNormal;
    Vector2 drillPosition;
    /* What the drill is biting into, sampled before the cut. Audio pitches the
       grind by it, so rock does not sound like dirt. */
    CellMaterial drillMaterial;
    float acceleration;
    float maxSpeed;
    float boostAcceleration;
    float boostStageTwoAcceleration;
    float boostStageThreeAcceleration;
    float boostStageOneSpeed;
    float boostStageTwoSpeed;
    float boostMaxSpeed;
    float boostDrag;
    float boostStageTwoDrag;
    float boostStageThreeDrag;
    float sonicSpeed;
    float boostStageTwoDelay;
    float boostStageThreeDelay;
    float boostStageTime;
    float boostGrace;
    float drillSpeed;
    /* Speed shed per cell of travel through material, before the boost stage
       and the material's own weight scale it. Measured per distance rather
       than per cell removed: a wider tunnel destroys far more cells for the
       same journey, and charging for those would mean every widening of the
       drill silently slowed the player down. */
    float drillResistance;
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
    /* Drag per second per unit of material density, applied while the player is
       inside something they can move through. One rule covers every fluid the
       table has or will have: water slows, lava slows harder, smoke barely
       registers, and a new liquid needs no code here at all. */
    float fluidDrag;
    float drag;
    float restitution;
    float radius;
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
    PlayerBoostStage boostStage;
    /* One-frame event consumed by main for particles, audio and camera kick. */
    PlayerBoostStage boostStageChanged;
    /* Presentation reads this while boostBurstTimer is active to animate the
       stage ring without feeding visual state back into simulation. */
    PlayerBoostStage boostBurstStage;
    bool facingRight;
    bool thrusting;
    bool boosting;
    bool boostTrailEmitted;
} Player;

void PlayerInit(Player *player, Vector2 position);
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
/* Radius of the tunnel the current boost stage cuts. */
float PlayerDrillRadius(const Player *player);

/* Where a beam leaves the character: the eye, pushed just clear of the collider
   along the line of aim.

   One helper, used by the gameplay ray and by the drawn beam alike. Two
   derivations of "where the laser starts" is two chances for them to disagree,
   and a beam that is cast from the chest but drawn from the head reads as a
   bug the moment the player aims down. */
Vector2 PlayerBeamOrigin(const Player *player, Vector2 aim);
/* Holds a pose for `holdTime` seconds. Held powers refresh it every frame with a
   short time; a one-shot like the force blast asks for the length of its own
   animation. */
void PlayerSetPose(Player *player, PlayerPose pose, float holdTime);

#endif
