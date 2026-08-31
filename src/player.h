#ifndef PLAYER_H
#define PLAYER_H

#include <raylib.h>

#include "world.h"

typedef struct Player {
    Vector2 position;
    Vector2 velocity;
    Vector2 impactPosition;
    Vector2 impactNormal;
    Vector2 drillPosition;
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
void PlayerDraw(const Player *player, Vector2 aimPosition);

#endif
