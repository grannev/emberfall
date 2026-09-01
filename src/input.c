#include "input.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

/* Which control each ability answers to, and what to call it on screen. This
   is the one place raylib key and mouse constants meet the ability ids; the
   ability table itself stays free of platform input. */
typedef struct AbilityBinding {
    AbilityId id;
    const char *label;
    int key;          /* KEY_* or 0 when the binding is a mouse button */
    int mouseButton;  /* MOUSE_BUTTON_* or -1 */
} AbilityBinding;

static const AbilityBinding ABILITY_BINDINGS[ABILITY_COUNT] = {
    {ABILITY_LASER, "LMB", 0, MOUSE_BUTTON_LEFT},
    {ABILITY_EXPLOSION, "RMB", 0, MOUSE_BUTTON_RIGHT},
    {ABILITY_FORCE, "Q", KEY_Q, -1},
    {ABILITY_CRYO, "E", KEY_E, -1},
};

/* Held abilities want the button state, one-shot abilities want the press
   edge. Reading that from the ability's own definition means a new power
   behaves correctly from the moment it is bound. */
static bool AbilityRequested(const AbilityBinding *binding)
{
    bool pressed = AbilityDefinitionAt(binding->id)->trigger ==
                   ABILITY_TRIGGER_PRESSED;

    if (binding->mouseButton >= 0) {
        return pressed ? IsMouseButtonPressed(binding->mouseButton)
                       : IsMouseButtonDown(binding->mouseButton);
    }
    return pressed ? IsKeyPressed(binding->key) : IsKeyDown(binding->key);
}

const char *InputAbilityBinding(AbilityId id)
{
    int index;

    for (index = 0; index < ABILITY_COUNT; ++index) {
        if (ABILITY_BINDINGS[index].id == id) {
            return ABILITY_BINDINGS[index].label;
        }
    }
    return "?";
}

AppInput InputPoll(const World *world, Camera2D camera)
{
    AppInput input = {0};
    Vector2 point = GetScreenToWorld2D(GetMousePosition(), camera);
    int index;

    if (world != NULL && world->width > 0 && world->height > 0) {
        point.x = Clamp(floorf(point.x), 0.0f, (float)(world->width - 1));
        point.y = Clamp(floorf(point.y), 0.0f, (float)(world->height - 1));
    }
    input.cursorCell = point;
    input.game.aimWorld = (Vector2){point.x + 0.5f, point.y + 0.5f};

    if (IsKeyDown(KEY_A)) input.game.move.x -= 1.0f;
    if (IsKeyDown(KEY_D)) input.game.move.x += 1.0f;
    if (IsKeyDown(KEY_W)) input.game.move.y -= 1.0f;
    if (IsKeyDown(KEY_S)) input.game.move.y += 1.0f;
    input.game.boostHeld = IsKeyDown(KEY_LEFT_SHIFT) ||
                           IsKeyDown(KEY_RIGHT_SHIFT);
    for (index = 0; index < ABILITY_COUNT; ++index) {
        const AbilityBinding *binding = &ABILITY_BINDINGS[index];

        input.game.ability[binding->id] = AbilityRequested(binding);
    }
    /* F, the one key near the movement hand that no power had taken. */
    input.game.grabHeld = IsKeyDown(KEY_F);
    input.game.regeneratePressed = IsKeyPressed(KEY_R);
    input.toggleDebugPressed = IsKeyPressed(KEY_F1);
    return input;
}
