/* The main menu. See menu.h for what it is responsible for; this file
 * records how it is laid out and drawn.
 */
#include "menu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Main screen items. */
enum {
    MENU_MAIN_PLAY = 0,
    MENU_MAIN_NEW_WORLD,
    MENU_MAIN_SETTINGS,
    MENU_MAIN_QUIT,
    MENU_MAIN_COUNT
};

/* New world items. */
enum {
    MENU_NEW_SEED = 0,
    MENU_NEW_CREATE,
    MENU_NEW_BACK,
    MENU_NEW_COUNT
};

static const Color MENU_TEXT = {214, 221, 229, 255};
static const Color MENU_DIM = {120, 132, 146, 255};
static const Color MENU_EMBER = {255, 148, 52, 255};
static const Color MENU_EMBER_DEEP = {196, 64, 20, 255};

void MenuInit(Menu *menu)
{
    if (menu == NULL) {
        return;
    }
    memset(menu, 0, sizeof(*menu));
    MenuOpen(menu);
}

void MenuOpen(Menu *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->screen = MENU_SCREEN_MAIN;
    menu->selected = 0;
}

static int MenuItemCount(const Menu *menu)
{
    switch (menu->screen) {
    case MENU_SCREEN_NEW_WORLD:
        return MENU_NEW_COUNT;
    case MENU_SCREEN_SETTINGS:
        return SETTINGS_COUNT + 1;
    case MENU_SCREEN_MAIN:
    default:
        return MENU_MAIN_COUNT;
    }
}

/* Everything is sized from the window's height, so the menu reads the same
   at 720 lines and at 2160. */
static float MenuUnit(int height)
{
    float unit = (float)height / 720.0f;

    return unit < 0.75f ? 0.75f : unit;
}

static int MenuLeft(int width, int height)
{
    (void)width;
    return (int)(88.0f * MenuUnit(height));
}

/* The row setting `index` sits on: one row per setting before it, one per
   category heading at or before it, and one more of space before BACK. */
static int MenuSettingsRow(int index)
{
    int last = index < SETTINGS_COUNT ? index : SETTINGS_COUNT - 1;
    int headings = 0;
    int setting;

    for (setting = 0; setting <= last; ++setting) {
        if (setting == 0 || strcmp(SETTINGS_TABLE[setting].category,
                                   SETTINGS_TABLE[setting - 1].category) != 0) {
            ++headings;
        }
    }
    return index + headings + (index >= SETTINGS_COUNT ? 1 : 0);
}

/* Where item `index` of the current screen is drawn. */
static Rectangle MenuItemBounds(const Menu *menu, int index, int width, int height)
{
    float unit = MenuUnit(height);
    float left = (float)MenuLeft(width, height);

    switch (menu->screen) {
    case MENU_SCREEN_SETTINGS: {
        float top = 150.0f * unit;
        float rowHeight = 34.0f * unit;

        return (Rectangle){left, top + (float)MenuSettingsRow(index) * rowHeight,
                           560.0f * unit, rowHeight};
    }
    case MENU_SCREEN_NEW_WORLD:
    case MENU_SCREEN_MAIN:
    default: {
        float top = 300.0f * unit;
        float rowHeight = 50.0f * unit;

        return (Rectangle){left, top + (float)index * rowHeight, 420.0f * unit,
                           rowHeight};
    }
    }
}

static void MenuSeedType(Menu *menu, const MenuInput *input)
{
    int index;

    for (index = 0; index < input->characterCount; ++index) {
        int character = input->characters[index];
        bool accepted = (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f') ||
                        (character >= 'A' && character <= 'F') ||
                        character == 'x' || character == 'X';

        if (accepted && menu->seedLength < MENU_SEED_CAPACITY - 1) {
            menu->seedText[menu->seedLength++] = (char)character;
            menu->seedText[menu->seedLength] = '\0';
        }
    }
    if (input->erase && menu->seedLength > 0) {
        menu->seedText[--menu->seedLength] = '\0';
    }
}

static MenuAction MenuActivate(Menu *menu, Settings *settings, int direction)
{
    MenuAction action = {MENU_ACTION_NONE, false, 0u};

    switch (menu->screen) {
    case MENU_SCREEN_MAIN:
        if (direction != 0) {
            break;
        }
        switch (menu->selected) {
        case MENU_MAIN_PLAY:
            action.type = MENU_ACTION_CONTINUE;
            break;
        case MENU_MAIN_NEW_WORLD:
            menu->screen = MENU_SCREEN_NEW_WORLD;
            menu->selected = MENU_NEW_CREATE;
            break;
        case MENU_MAIN_SETTINGS:
            menu->screen = MENU_SCREEN_SETTINGS;
            menu->selected = 0;
            break;
        case MENU_MAIN_QUIT:
            action.type = MENU_ACTION_QUIT;
            break;
        default:
            break;
        }
        break;
    case MENU_SCREEN_NEW_WORLD:
        if (direction != 0) {
            break;
        }
        if (menu->selected == MENU_NEW_CREATE) {
            char *end = NULL;

            action.type = MENU_ACTION_NEW_WORLD;
            if (menu->seedLength > 0) {
                unsigned long long seed = strtoull(menu->seedText, &end, 0);

                /* A seed that does not parse, or parses to zero, is no seed:
                   zero is what "pick one" means everywhere else. */
                if (end != menu->seedText && seed != 0u) {
                    action.seeded = true;
                    action.seed = (uint64_t)seed;
                }
            }
        } else if (menu->selected == MENU_NEW_BACK) {
            menu->screen = MENU_SCREEN_MAIN;
            menu->selected = MENU_MAIN_NEW_WORLD;
        }
        break;
    case MENU_SCREEN_SETTINGS:
        if (menu->selected >= SETTINGS_COUNT) {
            if (direction == 0) {
                menu->screen = MENU_SCREEN_MAIN;
                menu->selected = MENU_MAIN_SETTINGS;
            }
            break;
        }
        SettingsStep(settings, &SETTINGS_TABLE[menu->selected],
                     direction == 0 ? 1 : direction);
        action.type = MENU_ACTION_SETTINGS_CHANGED;
        break;
    }
    return action;
}

MenuAction MenuUpdate(Menu *menu, const MenuInput *input, Settings *settings,
                      int width, int height, float deltaTime)
{
    MenuAction none = {MENU_ACTION_NONE, false, 0u};
    int count;
    int index;

    if (menu == NULL || input == NULL || settings == NULL) {
        return none;
    }
    menu->time += deltaTime;
    count = MenuItemCount(menu);

    if (input->back) {
        if (menu->screen != MENU_SCREEN_MAIN) {
            menu->selected = menu->screen == MENU_SCREEN_SETTINGS
                                 ? MENU_MAIN_SETTINGS
                                 : MENU_MAIN_NEW_WORLD;
            menu->screen = MENU_SCREEN_MAIN;
            return none;
        }
        if (menu->worldEntered) {
            MenuAction resume = {MENU_ACTION_CONTINUE, false, 0u};

            return resume;
        }
        return none;
    }
    if (input->up) {
        menu->selected = (menu->selected + count - 1) % count;
    }
    if (input->down) {
        menu->selected = (menu->selected + 1) % count;
    }
    /* The pointer selects what it rests on, and only when it moves: a mouse
       lying still over one item must not fight the keys. */
    for (index = 0; index < count; ++index) {
        Rectangle bounds = MenuItemBounds(menu, index, width, height);

        if (CheckCollisionPointRec(input->mouse, bounds)) {
            if (input->mouseMoved) {
                menu->selected = index;
            }
            if (input->click) {
                int direction = 0;

                menu->selected = index;
                /* On a setting, the left half steps back and the right half
                   forward, as the arrows either side of its value say. */
                if (menu->screen == MENU_SCREEN_SETTINGS && index < SETTINGS_COUNT &&
                    SETTINGS_TABLE[index].kind != SETTING_BOOL) {
                    direction = input->mouse.x < bounds.x + bounds.width * 0.72f
                                    ? -1
                                    : 1;
                }
                return MenuActivate(menu, settings, direction);
            }
        }
    }
    if (menu->screen == MENU_SCREEN_NEW_WORLD && menu->selected == MENU_NEW_SEED) {
        MenuSeedType(menu, input);
    }
    if (input->confirm) {
        if (menu->screen == MENU_SCREEN_NEW_WORLD && menu->selected == MENU_NEW_SEED) {
            menu->selected = MENU_NEW_CREATE;
        }
        return MenuActivate(menu, settings, 0);
    }
    if (menu->screen == MENU_SCREEN_SETTINGS && (input->left || input->right)) {
        return MenuActivate(menu, settings, input->left ? -1 : 1);
    }
    return none;
}

/* ---- drawing -------------------------------------------------------------- */

static void MenuText(const char *text, float x, float y, float size, Color color)
{
    DrawText(text, (int)x, (int)y, (int)size, color);
}

/* The title: the name in ember over a darker ember shadow, with a flicker
   that steps rather than slides, like everything else in the game. */
static void MenuDrawTitle(const Menu *menu, int width, int height)
{
    float unit = MenuUnit(height);
    float left = (float)MenuLeft(width, height);
    float size = 84.0f * unit;
    float top = 118.0f * unit;
    int step = (int)floorf(menu->time * 8.0f);
    float flicker = 0.85f + 0.15f * (float)((step * 7919) % 13) / 12.0f;
    Color glow = MENU_EMBER;

    glow.a = (unsigned char)(255.0f * flicker);
    MenuText("EMBERFALL", left + 5.0f * unit, top + 5.0f * unit, size,
             (Color){40, 12, 4, 220});
    MenuText("EMBERFALL", left + 2.0f * unit, top + 2.0f * unit, size, MENU_EMBER_DEEP);
    MenuText("EMBERFALL", left, top, size, glow);
    MenuText("A WORLD OF LOOSE STONE, LIVE WATER AND FIRE", left + 4.0f * unit,
             top + size + 8.0f * unit, 18.0f * unit, MENU_DIM);
}

static void MenuDrawItem(const Menu *menu, int index, const char *label,
                         int width, int height)
{
    Rectangle bounds = MenuItemBounds(menu, index, width, height);
    float unit = MenuUnit(height);
    float size = 30.0f * unit;
    bool selected = menu->selected == index;
    float y = bounds.y + (bounds.height - size) * 0.5f;

    if (selected) {
        float pulse = 0.5f + 0.5f * sinf(menu->time * 5.0f);

        DrawRectangle((int)bounds.x - (int)(22.0f * unit), (int)y,
                      (int)(8.0f * unit), (int)size,
                      (Color){255, 148, 52, (unsigned char)(160 + 95 * pulse)});
        MenuText(label, bounds.x + 4.0f * unit, y, size, MENU_EMBER);
    } else {
        MenuText(label, bounds.x, y, size, MENU_TEXT);
    }
}

static void MenuDrawMain(const Menu *menu, int width, int height)
{
    static const char *const LABELS[MENU_MAIN_COUNT] = {
        "PLAY", "NEW WORLD", "SETTINGS", "QUIT",
    };
    int index;

    MenuDrawTitle(menu, width, height);
    for (index = 0; index < MENU_MAIN_COUNT; ++index) {
        const char *label = LABELS[index];

        if (index == MENU_MAIN_PLAY && menu->worldEntered) {
            label = "CONTINUE";
        }
        MenuDrawItem(menu, index, label, width, height);
    }
}

static void MenuDrawNewWorld(const Menu *menu, int width, int height)
{
    float unit = MenuUnit(height);
    float left = (float)MenuLeft(width, height);
    Rectangle field = MenuItemBounds(menu, MENU_NEW_SEED, width, height);
    bool editing = menu->selected == MENU_NEW_SEED;
    bool caret = editing && fmodf(menu->time, 1.0f) < 0.55f;
    char shown[MENU_SEED_CAPACITY + 16];

    MenuText("NEW WORLD", left, 150.0f * unit, 56.0f * unit, MENU_EMBER);
    MenuText("A SEED MAKES THE SAME WORLD EVERY TIME. LEAVE IT EMPTY FOR A NEW ONE.",
             left, 222.0f * unit, 16.0f * unit, MENU_DIM);

    snprintf(shown, sizeof(shown), "SEED  %s%s",
             menu->seedLength > 0 ? menu->seedText : (editing ? "" : "RANDOM"),
             caret ? "_" : "");
    DrawRectangleLines((int)field.x - (int)(10.0f * unit), (int)field.y,
                       (int)(field.width + 20.0f * unit), (int)field.height,
                       editing ? MENU_EMBER : (Color){70, 80, 94, 255});
    MenuText(shown, field.x, field.y + (field.height - 30.0f * unit) * 0.5f,
             30.0f * unit, editing ? MENU_EMBER : MENU_TEXT);
    MenuDrawItem(menu, MENU_NEW_CREATE, "CREATE", width, height);
    MenuDrawItem(menu, MENU_NEW_BACK, "BACK", width, height);
}

static void MenuDrawSettings(const Menu *menu, const Settings *settings, int width,
                             int height)
{
    float unit = MenuUnit(height);
    float left = (float)MenuLeft(width, height);
    int index;

    MenuText("SETTINGS", left, 72.0f * unit, 56.0f * unit, MENU_EMBER);
    for (index = 0; index < SETTINGS_COUNT; ++index) {
        const SettingDescriptor *setting = &SETTINGS_TABLE[index];
        Rectangle bounds = MenuItemBounds(menu, index, width, height);
        bool selected = menu->selected == index;
        float size = 24.0f * unit;
        float y = bounds.y + (bounds.height - size) * 0.5f;
        char value[48];
        char shown[64];

        if (index == 0 ||
            strcmp(setting->category, SETTINGS_TABLE[index - 1].category) != 0) {
            MenuText(setting->category, left, bounds.y - 18.0f * unit, 14.0f * unit,
                     MENU_DIM);
        }
        SettingsFormat(settings, setting, value, sizeof(value));
        snprintf(shown, sizeof(shown), setting->kind == SETTING_BOOL ? "%s" : "<  %s  >",
                 value);
        if (selected) {
            DrawRectangle((int)bounds.x - (int)(14.0f * unit), (int)bounds.y,
                          (int)(bounds.width + 28.0f * unit), (int)bounds.height,
                          (Color){255, 148, 52, 34});
        }
        MenuText(setting->label, bounds.x, y, size, selected ? MENU_EMBER : MENU_TEXT);
        MenuText(shown,
                 bounds.x + bounds.width - (float)MeasureText(shown, (int)size),
                 y, size, selected ? MENU_EMBER : MENU_TEXT);
    }
    MenuDrawItem(menu, SETTINGS_COUNT, "BACK", width, height);
}

void MenuDraw(const Menu *menu, const Settings *settings, int width, int height)
{
    float unit;
    char footer[96];

    if (menu == NULL || settings == NULL) {
        return;
    }
    unit = MenuUnit(height);
    /* The world stays in view behind the menu, darkened toward the side the
       text is on so the text reads and the world still shows. */
    DrawRectangleGradientH(0, 0, width / 2 + width / 6, height,
                           (Color){2, 4, 9, 236}, (Color){2, 4, 9, 120});
    DrawRectangle(width / 2 + width / 6, 0, width - (width / 2 + width / 6), height,
                  (Color){2, 4, 9, 120});

    switch (menu->screen) {
    case MENU_SCREEN_NEW_WORLD:
        MenuDrawNewWorld(menu, width, height);
        break;
    case MENU_SCREEN_SETTINGS:
        MenuDrawSettings(menu, settings, width, height);
        break;
    case MENU_SCREEN_MAIN:
    default:
        MenuDrawMain(menu, width, height);
        break;
    }

    snprintf(footer, sizeof(footer), "SEED 0x%llx    ARROWS / MOUSE  -  ENTER  -  ESC",
             (unsigned long long)menu->worldSeed);
    MenuText(footer, (float)MenuLeft(width, height), (float)height - 44.0f * unit,
             16.0f * unit, MENU_DIM);
}
