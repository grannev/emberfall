#ifndef GAME_INPUT_H
#define GAME_INPUT_H

#include <stdbool.h>

#include <raylib.h>

#include "abilities.h"

/* Gameplay-oriented input for one render frame. No gameplay module polls
   raylib directly; tests and future replay code can construct this value. */
typedef struct GameInput {
    Vector2 move;
    Vector2 aimWorld;
    bool boostHeld;
    /* One flag per ability, in AbilityId order. Held abilities get the button
       state, one-shot abilities get the press edge — input.c decides which from
       the ability's own definition, so a new power adds a binding rather than
       another named field here and another argument downstream. */
    bool ability[ABILITY_COUNT];
    bool regeneratePressed;
} GameInput;

#endif
