#ifndef SETTINGS_H
#define SETTINGS_H

/* The player's settings, and the table that describes them.
 *
 * Every setting is one field of `Settings` and one row of SETTINGS_TABLE,
 * which gives its key in the file, its label in the menu, its kind and its
 * range. The menu draws and edits settings from the table alone, and the file
 * is read and written from it, so adding a setting is a field and a row — and
 * then whatever makes it take effect — with no menu code and no parser code.
 *
 * The file is a flat `key = value` list. Unknown keys are ignored and missing
 * ones keep their defaults, so an old file still loads after a setting is
 * added and a new one still loads after a setting is removed. Values out of
 * range are clamped on load: a hand-edited file cannot put the game in a
 * state the menu could not.
 *
 * Presentation and application only. Nothing in the gameplay core reads a
 * setting; a setting that changed how the world behaves would make a seed and
 * a sequence of inputs replay differently on two machines.
 */

#include <stdbool.h>
#include <stddef.h>

typedef enum SettingsFrameLimit {
    SETTINGS_FRAME_LIMIT_60 = 0,
    SETTINGS_FRAME_LIMIT_120,
    SETTINGS_FRAME_LIMIT_144,
    SETTINGS_FRAME_LIMIT_240,
    SETTINGS_FRAME_LIMIT_NONE,
    SETTINGS_FRAME_LIMIT_COUNT
} SettingsFrameLimit;

typedef struct Settings {
    /* Audio. */
    float masterVolume;
    /* Display. */
    bool fullscreen;
    bool vsync;
    int frameLimit;
    /* Camera. */
    float screenShake;
    float zoomSpeed;
    /* Interface. */
    bool showDebugHud;
    bool showControls;
} Settings;

typedef enum SettingKind {
    SETTING_BOOL = 0,
    SETTING_FLOAT,
    /* An int indexing `choices`. */
    SETTING_CHOICE,
} SettingKind;

typedef struct SettingDescriptor {
    /* The key in the file: lower case, no spaces, never renamed once
       shipped, or every saved value of it is lost. */
    const char *key;
    const char *label;
    /* The heading the menu groups it under. */
    const char *category;
    SettingKind kind;
    size_t offset;
    /* SETTING_FLOAT: range and the step one press moves it by. Shown as a
       percentage when `percent` is set. */
    float minimum;
    float maximum;
    float step;
    bool percent;
    /* SETTING_CHOICE: the labels, `choiceCount` of them. */
    const char *const *choices;
    int choiceCount;
} SettingDescriptor;

extern const SettingDescriptor SETTINGS_TABLE[];
extern const int SETTINGS_COUNT;

Settings SettingsDefaults(void);

/* Where the file lives: $XDG_CONFIG_HOME/emberfall/settings.ini, else
   ~/.config/emberfall/settings.ini, else settings.ini beside the working
   directory. Written into `buffer`; false when it did not fit. */
bool SettingsPath(char *buffer, size_t size);

/* Loads over `settings`, which should hold the defaults: what the file does
   not mention stays as it was. False when there was no file to read, which
   is not an error — it is the first run. */
bool SettingsLoad(Settings *settings, const char *path);
/* Writes every setting, creating the directory if it has to. */
bool SettingsSave(const Settings *settings, const char *path);

/* Moves a setting one step: a bool flips, a float moves by its step, a
   choice to the next or previous one. `direction` is -1 or 1. */
void SettingsStep(Settings *settings, const SettingDescriptor *setting,
                  int direction);
/* The value as the menu shows it. */
void SettingsFormat(const Settings *settings, const SettingDescriptor *setting,
                    char *buffer, size_t size);
/* Clamps every value into its range. Load does it; exposed for tests. */
void SettingsSanitize(Settings *settings);

/* Frames per second a frame limit stands for; zero for none. */
int SettingsFrameLimitRate(int frameLimit);

#endif
