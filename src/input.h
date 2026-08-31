#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

#include <raylib.h>

#include "game_input.h"
#include "world.h"

typedef struct AppInput {
    GameInput game;
    Vector2 cursorCell;
    bool toggleDebugPressed;
} AppInput;

AppInput InputPoll(const World *world, Camera2D camera);

#endif
