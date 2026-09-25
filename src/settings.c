/* Settings. See settings.h for the contract; this file records how it is
 * kept.
 */
#include "settings.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const FRAME_LIMIT_CHOICES[SETTINGS_FRAME_LIMIT_COUNT] = {
    "60", "120", "144", "240", "UNLIMITED",
};

#define SETTING_OFFSET(field) offsetof(Settings, field)

const SettingDescriptor SETTINGS_TABLE[] = {
    {"master_volume", "MASTER VOLUME", "AUDIO", SETTING_FLOAT,
     SETTING_OFFSET(masterVolume), 0.0f, 1.0f, 0.05f, true, NULL, 0},
    {"fullscreen", "FULLSCREEN", "DISPLAY", SETTING_BOOL,
     SETTING_OFFSET(fullscreen), 0.0f, 0.0f, 0.0f, false, NULL, 0},
    {"vsync", "VSYNC", "DISPLAY", SETTING_BOOL, SETTING_OFFSET(vsync), 0.0f, 0.0f,
     0.0f, false, NULL, 0},
    {"frame_limit", "FRAME LIMIT", "DISPLAY", SETTING_CHOICE,
     SETTING_OFFSET(frameLimit), 0.0f, 0.0f, 0.0f, false, FRAME_LIMIT_CHOICES,
     SETTINGS_FRAME_LIMIT_COUNT},
    {"screen_shake", "SCREEN SHAKE", "CAMERA", SETTING_FLOAT,
     SETTING_OFFSET(screenShake), 0.0f, 1.0f, 0.1f, true, NULL, 0},
    {"zoom_speed", "ZOOM SPEED", "CAMERA", SETTING_FLOAT, SETTING_OFFSET(zoomSpeed),
     0.25f, 3.0f, 0.25f, false, NULL, 0},
    {"debug_hud", "DEBUG HUD", "INTERFACE", SETTING_BOOL,
     SETTING_OFFSET(showDebugHud), 0.0f, 0.0f, 0.0f, false, NULL, 0},
    {"controls_hint", "CONTROLS HINT", "INTERFACE", SETTING_BOOL,
     SETTING_OFFSET(showControls), 0.0f, 0.0f, 0.0f, false, NULL, 0},
};

const int SETTINGS_COUNT = (int)(sizeof(SETTINGS_TABLE) / sizeof(SETTINGS_TABLE[0]));

Settings SettingsDefaults(void)
{
    Settings settings;

    settings.masterVolume = 0.72f;
    settings.fullscreen = false;
    settings.vsync = true;
    settings.frameLimit = SETTINGS_FRAME_LIMIT_120;
    settings.screenShake = 1.0f;
    settings.zoomSpeed = 1.0f;
    settings.showDebugHud = true;
    settings.showControls = true;
    return settings;
}

static void *SettingField(Settings *settings, const SettingDescriptor *setting)
{
    return (char *)settings + setting->offset;
}

static const void *SettingFieldConst(const Settings *settings,
                                     const SettingDescriptor *setting)
{
    return (const char *)settings + setting->offset;
}

static float SettingsClampFloat(float value, float minimum, float maximum)
{
    if (!isfinite(value)) return minimum;
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

void SettingsSanitize(Settings *settings)
{
    int index;

    if (settings == NULL) {
        return;
    }
    for (index = 0; index < SETTINGS_COUNT; ++index) {
        const SettingDescriptor *setting = &SETTINGS_TABLE[index];

        if (setting->kind == SETTING_FLOAT) {
            float *value = SettingField(settings, setting);

            *value = SettingsClampFloat(*value, setting->minimum, setting->maximum);
        } else if (setting->kind == SETTING_CHOICE) {
            int *value = SettingField(settings, setting);

            if (*value < 0) *value = 0;
            if (*value >= setting->choiceCount) *value = setting->choiceCount - 1;
        }
    }
}

bool SettingsPath(char *buffer, size_t size)
{
    const char *config = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int written;

    if (buffer == NULL || size == 0u) {
        return false;
    }
    if (config != NULL && config[0] != '\0') {
        written = snprintf(buffer, size, "%s/emberfall/settings.ini", config);
    } else if (home != NULL && home[0] != '\0') {
        written = snprintf(buffer, size, "%s/.config/emberfall/settings.ini", home);
    } else {
        written = snprintf(buffer, size, "settings.ini");
    }
    return written > 0 && (size_t)written < size;
}

/* Trims spaces from both ends in place. */
static char *SettingsTrim(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                          end[-1] == '\r')) {
        --end;
    }
    *end = '\0';
    return text;
}

static void SettingsParseValue(Settings *settings, const SettingDescriptor *setting,
                               const char *text)
{
    switch (setting->kind) {
    case SETTING_BOOL: {
        bool *value = SettingField(settings, setting);

        *value = strcmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
                 strcmp(text, "on") == 0 || strcmp(text, "yes") == 0;
        break;
    }
    case SETTING_FLOAT: {
        float *value = SettingField(settings, setting);
        char *end = NULL;
        float parsed = strtof(text, &end);

        if (end != text) {
            *value = parsed;
        }
        break;
    }
    case SETTING_CHOICE: {
        int *value = SettingField(settings, setting);
        int choice;

        /* By label, so the file reads as the menu does and a reordered list
           does not move anybody's choice. */
        for (choice = 0; choice < setting->choiceCount; ++choice) {
            if (strcmp(text, setting->choices[choice]) == 0) {
                *value = choice;
                break;
            }
        }
        break;
    }
    }
}

bool SettingsLoad(Settings *settings, const char *path)
{
    FILE *file;
    char line[256];

    if (settings == NULL || path == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals = strchr(line, '=');
        char *key;
        char *value;
        int index;

        if (line[0] == '#' || line[0] == ';' || equals == NULL) {
            continue;
        }
        *equals = '\0';
        key = SettingsTrim(line);
        value = SettingsTrim(equals + 1);
        for (index = 0; index < SETTINGS_COUNT; ++index) {
            if (strcmp(key, SETTINGS_TABLE[index].key) == 0) {
                SettingsParseValue(settings, &SETTINGS_TABLE[index], value);
                break;
            }
        }
    }
    fclose(file);
    SettingsSanitize(settings);
    return true;
}

/* Creates every directory on the way to the file. */
static void SettingsMakeParents(const char *path)
{
    char buffer[512];
    size_t length = strlen(path);
    size_t index;

    if (length >= sizeof(buffer)) {
        return;
    }
    memcpy(buffer, path, length + 1u);
    for (index = 1u; index < length; ++index) {
        if (buffer[index] == '/') {
            buffer[index] = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
                return;
            }
            buffer[index] = '/';
        }
    }
}

bool SettingsSave(const Settings *settings, const char *path)
{
    FILE *file;
    int index;

    if (settings == NULL || path == NULL) {
        return false;
    }
    SettingsMakeParents(path);
    file = fopen(path, "w");
    if (file == NULL) {
        return false;
    }
    fprintf(file, "# Emberfall settings. Written by the game; safe to edit.\n");
    for (index = 0; index < SETTINGS_COUNT; ++index) {
        const SettingDescriptor *setting = &SETTINGS_TABLE[index];
        const void *field = SettingFieldConst(settings, setting);

        switch (setting->kind) {
        case SETTING_BOOL:
            fprintf(file, "%s = %s\n", setting->key,
                    *(const bool *)field ? "true" : "false");
            break;
        case SETTING_FLOAT:
            fprintf(file, "%s = %.3f\n", setting->key, (double)*(const float *)field);
            break;
        case SETTING_CHOICE:
            fprintf(file, "%s = %s\n", setting->key,
                    setting->choices[*(const int *)field]);
            break;
        }
    }
    return fclose(file) == 0;
}

void SettingsStep(Settings *settings, const SettingDescriptor *setting,
                  int direction)
{
    if (settings == NULL || setting == NULL) {
        return;
    }
    switch (setting->kind) {
    case SETTING_BOOL: {
        bool *value = SettingField(settings, setting);

        *value = !*value;
        break;
    }
    case SETTING_FLOAT: {
        float *value = SettingField(settings, setting);

        /* Snapped to the step, so a value loaded from a hand-edited file
           rejoins the grid the menu moves on. */
        *value = roundf((*value + (float)direction * setting->step) / setting->step) *
                 setting->step;
        *value = SettingsClampFloat(*value, setting->minimum, setting->maximum);
        break;
    }
    case SETTING_CHOICE: {
        int *value = SettingField(settings, setting);

        *value = (*value + direction + setting->choiceCount) % setting->choiceCount;
        break;
    }
    }
}

void SettingsFormat(const Settings *settings, const SettingDescriptor *setting,
                    char *buffer, size_t size)
{
    const void *field;

    if (buffer == NULL || size == 0u) {
        return;
    }
    if (settings == NULL || setting == NULL) {
        buffer[0] = '\0';
        return;
    }
    field = SettingFieldConst(settings, setting);
    switch (setting->kind) {
    case SETTING_BOOL:
        snprintf(buffer, size, "%s", *(const bool *)field ? "ON" : "OFF");
        break;
    case SETTING_FLOAT:
        if (setting->percent) {
            snprintf(buffer, size, "%d%%",
                     (int)lroundf(*(const float *)field * 100.0f));
        } else {
            snprintf(buffer, size, "%.2fx", (double)*(const float *)field);
        }
        break;
    case SETTING_CHOICE:
        snprintf(buffer, size, "%s", setting->choices[*(const int *)field]);
        break;
    }
}

int SettingsFrameLimitRate(int frameLimit)
{
    static const int RATES[SETTINGS_FRAME_LIMIT_COUNT] = {60, 120, 144, 240, 0};

    if (frameLimit < 0 || frameLimit >= SETTINGS_FRAME_LIMIT_COUNT) {
        return 120;
    }
    return RATES[frameLimit];
}
