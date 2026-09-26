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
    {ABILITY_LASER, "LMB", 0, MOUSE_BUTTON_LEFT},
    {ABILITY_CRYO, "LMB", 0, MOUSE_BUTTON_LEFT},
    {ABILITY_NUCLEAR, "LMB", 0, MOUSE_BUTTON_LEFT},
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

MenuInput InputPollMenu(void)
{
    static Vector2 lastMouse = {-1.0f, -1.0f};
    MenuInput input = {0};
    int character;

    input.up = IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W);
    input.down = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S);
    input.left = IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A);
    input.right = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D);
    input.confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
                    IsKeyPressed(KEY_SPACE);
    input.back = IsKeyPressed(KEY_ESCAPE);
    input.erase = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
    input.mouse = GetMousePosition();
    input.mouseMoved = input.mouse.x != lastMouse.x || input.mouse.y != lastMouse.y;
    lastMouse = input.mouse;
    input.click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    while ((character = GetCharPressed()) > 0 &&
           input.characterCount < (int)(sizeof(input.characters) /
                                        sizeof(input.characters[0]))) {
        input.characters[input.characterCount++] = character;
    }
    return input;
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

int InputAbilityCount(void)
{
    return ABILITY_BINDING_COUNT;
}

AbilityId InputAbilityAt(int index)
{
    return ABILITY_BINDINGS[index >= 0 && index < ABILITY_BINDING_COUNT ? index : 0].id;
}

AppInput InputPoll(const World *world, Camera2D camera, AbilitySelection *selection)
{
    AppInput input = {0};
    Vector2 point = GetScreenToWorld2D(GetMousePosition(), camera);
    bool wasOpen = selection->open;

    selection->open = IsKeyDown(KEY_TAB) && !IsKeyPressed(KEY_ESCAPE) && IsWindowFocused();
    if (!IsWindowFocused()) selection->blockPrimary = true;
    if (selection->open && !wasOpen) {
        selection->center = (Vector2){(float)GetScreenWidth() * 0.5f,
                                      (float)GetScreenHeight() * 0.5f};
        selection->hovered = -1;
    }
    if (selection->open) {
        Vector2 offset = Vector2Subtract(GetMousePosition(), selection->center);
        float scale = fminf((float)GetScreenWidth() / 1280.0f,
                            (float)GetScreenHeight() / 720.0f);
        float angle = atan2f(offset.y, offset.x) + PI * 0.5f +
                      PI / (float)ABILITY_BINDING_COUNT;

        if (angle < 0.0f) angle += 2.0f * PI;
        selection->hovered = Vector2Length(offset) < 40.0f * scale ? -1 :
            (int)(angle / (2.0f * PI) * (float)ABILITY_BINDING_COUNT) %
                ABILITY_BINDING_COUNT;
        selection->blockPrimary = true;
    } else if (wasOpen && !IsKeyPressed(KEY_ESCAPE)) {
        if (selection->hovered >= 0) {
            selection->selected = InputAbilityAt(selection->hovered);
        }
    }
    if (!selection->open && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        selection->blockPrimary = false;
    }

    /* Rows are clamped to the world; columns are left where the camera put
       them, unwrapped like the character they are aimed from. */
    if (world != NULL && world->width > 0 && world->height > 0) {
        point.x = floorf(point.x);
        point.y = Clamp(floorf(point.y), 0.0f, (float)(world->height - 1));
    }
    input.cursorCell = point;
    input.game.aimWorld = (Vector2){point.x + 0.5f, point.y + 0.5f};
    input.game.cancelCharge = selection->open || IsKeyPressed(KEY_ESCAPE) ||
                              !IsWindowFocused();

    if (IsKeyDown(KEY_A)) input.game.move.x -= 1.0f;
    if (IsKeyDown(KEY_D)) input.game.move.x += 1.0f;
    if (IsKeyDown(KEY_W)) input.game.move.y -= 1.0f;
    if (IsKeyDown(KEY_S)) input.game.move.y += 1.0f;
    input.game.boostHeld = IsKeyDown(KEY_LEFT_SHIFT) ||
                           IsKeyDown(KEY_RIGHT_SHIFT);
    if (!input.game.cancelCharge && !wasOpen && !selection->blockPrimary) {
        int index;

        for (index = 0; index < ABILITY_BINDING_COUNT; ++index) {
            const AbilityBinding *binding = &ABILITY_BINDINGS[index];

            if (binding->id == selection->selected) {
                input.game.ability[binding->id] = AbilityRequested(binding);
            }
        }
    }
    /* The right hand's other button. Grab is held rather than pressed, and it
       is not a power: no cooldown, no world effect of its own, nothing to put
       in the ability table. */
    input.game.grabHeld = IsMouseButtonDown(MOUSE_BUTTON_RIGHT);
    input.game.jumpPressed = IsKeyPressed(KEY_SPACE);
    input.game.jumpHeld = IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_W);
    input.game.upPressed = IsKeyPressed(KEY_W);
    input.game.regeneratePressed = IsKeyPressed(KEY_R);
    input.toggleDebugPressed = IsKeyPressed(KEY_F1);
    input.zoomSteps = GetMouseWheelMove();
    if (selection->open) input.zoomSteps = 0.0f;
    input.menuPressed = IsKeyPressed(KEY_ESCAPE);
    return input;
}
