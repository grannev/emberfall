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

/* Only the powers the player actually has. The table is a list of bindings
   rather than one entry per ability on purpose: an ability with no row here has
   no control, and that is how a mechanic can stay in the engine — reachable by
   tests, by world reactions, by whatever gameplay wants it later — without
   being something the player can fire. Explosion is exactly that. */
static const AbilityBinding ABILITY_BINDINGS[] = {
    {ABILITY_FORCE, "LMB", 0, MOUSE_BUTTON_LEFT},
    {ABILITY_CRYO, "Q", KEY_Q, -1},
    {ABILITY_LASER, "E", KEY_E, -1},
};

#define ABILITY_BINDING_COUNT \
    (int)(sizeof(ABILITY_BINDINGS) / sizeof(ABILITY_BINDINGS[0]))

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

    for (index = 0; index < ABILITY_BINDING_COUNT; ++index) {
        if (ABILITY_BINDINGS[index].id == id) {
            return ABILITY_BINDINGS[index].label;
        }
    }
    /* NULL rather than a placeholder: an unbound ability has no control to
       show, and a caller listing the controls has to be able to tell. */
    return NULL;
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
    for (index = 0; index < ABILITY_BINDING_COUNT; ++index) {
        const AbilityBinding *binding = &ABILITY_BINDINGS[index];

        input.game.ability[binding->id] = AbilityRequested(binding);
    }
    /* The right hand's other button. Grab is held rather than pressed, and it
       is not a power: no cooldown, no world effect of its own, nothing to put
       in the ability table. */
    input.game.grabHeld = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    input.game.regeneratePressed = IsKeyPressed(KEY_R);
    input.toggleDebugPressed = IsKeyPressed(KEY_F1);
    return input;
}
