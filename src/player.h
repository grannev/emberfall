#ifndef PLAYER_H
#define PLAYER_H

#include <raylib.h>

#include "world.h"

typedef struct Player {
    Vector2 position;
    Vector2 velocity;
    Vector2 impactPosition;
    Vector2 impactNormal;
    float acceleration;
    float maxSpeed;
    float drag;
    float restitution;
    float radius;
    float impactStrength;
    float impactTimer;
    bool facingRight;
    bool thrusting;
} Player;

void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, const World *world, float deltaTime);
void PlayerResolveWorldCollision(Player *player, const World *world);
void PlayerApplyExplosionImpulse(Player *player, Vector2 center, float radius, float force);
void PlayerDraw(const Player *player, Vector2 aimPosition);

#endif
