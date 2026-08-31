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
    float boostMaxSpeed;
    float boostDrag;
    float boostGrace;
    float drillSpeed;
    float drillResistance;
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
    int drilledCells;
    bool facingRight;
    bool thrusting;
    bool boosting;
    bool boostTrailEmitted;
} Player;

void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, World *world, Vector2 input, bool boostHeld,
                  float deltaTime);
void PlayerResolveWorldCollision(Player *player, World *world);
void PlayerApplyExplosionImpulse(Player *player, Vector2 center, float radius, float force);
/* Adds velocity directly. Used for recoil, where the direction is known and no
   falloff applies. */
void PlayerApplyImpulse(Player *player, Vector2 impulse);
/* Holds a pose for `holdTime` seconds. Held powers refresh it every frame with a
   short time; a one-shot like the force blast asks for the length of its own
   animation. */
void PlayerSetPose(Player *player, PlayerPose pose, float holdTime);
void PlayerDraw(const Player *player, Vector2 aimPosition);

#endif
