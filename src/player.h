#ifndef PLAYER_H
#define PLAYER_H

#include <raylib.h>

#include "world.h"

typedef struct Player {
    Vector2 position;
    Vector2 velocity;
    float speed;
    float radius;
    float health;
    float maxHealth;
    float invulnerability;
    float respawnTimer;
    bool alive;
    bool facingRight;
} Player;

void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, const World *world, float deltaTime);
void PlayerResolveWorldCollision(Player *player, const World *world);
void PlayerApplyWorldHazards(Player *player, const World *world);
bool PlayerNeedsRespawn(const Player *player);
void PlayerApplyExplosionImpulse(Player *player, Vector2 center, float radius, float force);
void PlayerDraw(const Player *player, Vector2 aimPosition);

#endif
