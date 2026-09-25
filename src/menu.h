#ifndef MENU_H
#define MENU_H

/* The main menu, drawn over the world.
 *
 * Three screens: the main one, a new world with an optional seed, and the
 * settings, which are drawn and edited from SETTINGS_TABLE alone — a new
 * setting appears here without a line of menu code. The menu decides nothing
 * about the game: it reports what the player chose as a MenuAction and the
 * application carries it out, so it never touches GameState.
 *
 * Layout is one function shared by the update and the draw, so what the
 * mouse hits is exactly what is drawn where it points.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "settings.h"

#define MENU_SEED_CAPACITY 24

typedef enum MenuScreen {
    MENU_SCREEN_MAIN = 0,
    MENU_SCREEN_NEW_WORLD,
    MENU_SCREEN_SETTINGS,
} MenuScreen;

typedef enum MenuActionType {
    MENU_ACTION_NONE = 0,
    /* Back to the world that is loaded. */
    MENU_ACTION_CONTINUE,
    /* A new world: from `seed` when `seeded`, else the session's next. */
    MENU_ACTION_NEW_WORLD,
    /* A setting changed: apply and save. */
    MENU_ACTION_SETTINGS_CHANGED,
    MENU_ACTION_QUIT,
} MenuActionType;

typedef struct MenuAction {
    MenuActionType type;
    bool seeded;
    uint64_t seed;
} MenuAction;

/* What the menu listens to this frame, gathered by input.c. */
typedef struct MenuInput {
    bool up;
    bool down;
    bool left;
    bool right;
    bool confirm;
    bool back;
    Vector2 mouse;
    bool mouseMoved;
    bool click;
    /* Characters typed this frame, for the seed field. */
    int characters[8];
    int characterCount;
    bool erase;
} MenuInput;

typedef struct Menu {
    MenuScreen screen;
    int selected;
    /* A world has been entered this session: the first item says CONTINUE
       rather than PLAY, and escape goes back to it. */
    bool worldEntered;
    float time;
    char seedText[MENU_SEED_CAPACITY];
    int seedLength;
    /* The loaded world's seed, shown under the menu. */
    uint64_t worldSeed;
} Menu;

void MenuInit(Menu *menu);
/* Opens on the main screen with the first item selected. */
void MenuOpen(Menu *menu);
MenuAction MenuUpdate(Menu *menu, const MenuInput *input, Settings *settings,
                      int width, int height, float deltaTime);
void MenuDraw(const Menu *menu, const Settings *settings, int width, int height);

#endif
