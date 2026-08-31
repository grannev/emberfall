#ifndef GAME_INPUT_H
#define GAME_INPUT_H

#include <stdbool.h>

#include <raylib.h>

/* Gameplay-oriented input for one render frame. No gameplay module polls
   raylib directly; tests and future replay code can construct this value. */
typedef struct GameInput {
    Vector2 move;
    Vector2 aimWorld;
    bool boostHeld;
    bool laserHeld;
    bool explosionPressed;
    bool forcePressed;
    bool chillHeld;
    bool regeneratePressed;
} GameInput;

#endif
