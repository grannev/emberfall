#ifndef PLAYER_H
#define PLAYER_H

#include <raylib.h>

typedef struct Player {
    Vector2 position;
    Vector2 velocity;
    float speed;
    float radius;
    bool facingRight;
} Player;

void PlayerInit(Player *player, Vector2 position);
void PlayerUpdate(Player *player, float deltaTime, int worldWidth, int worldHeight);
void PlayerDraw(const Player *player, Vector2 aimPosition);

#endif
