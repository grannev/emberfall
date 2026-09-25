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
    /* Notches of the mouse wheel this frame, positive away from the player:
       the camera zooms in on it. Presentation only — gameplay never sees it. */
    float zoomSteps;
    /* Escape: the menu. */
    bool menuPressed;
} AppInput;

AppInput InputPoll(const World *world, Camera2D camera);
/* The control an ability is bound to, for the HUD and the controls hint. The
   binding table lives with the raylib polling so gameplay never names a key. */
const char *InputAbilityBinding(AbilityId id);

#endif
