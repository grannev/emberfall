#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

#include <raylib.h>

#include "abilities.h"
#include "game_input.h"
#include "world.h"

typedef struct AppInput {
    GameInput game;
    Vector2 cursorCell;
    bool toggleDebugPressed;
} AppInput;

AppInput InputPoll(const World *world, Camera2D camera);
/* The control an ability is bound to, for the HUD and the controls hint. The
   binding table lives with the raylib polling so gameplay never names a key. */
const char *InputAbilityBinding(AbilityId id);

#endif
