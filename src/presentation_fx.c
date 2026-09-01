#include "presentation_fx.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static void PresentationFxCountDrop(PresentationFxSystem *system)
{
    if (system->stats.dropped < UINT32_MAX) {
        ++system->stats.dropped;
    }
}

static bool PresentationFxVectorIsFinite(Vector2 value)
{
    return isfinite(value.x) && isfinite(value.y);
}

static bool PresentationFxDescriptionIsValid(
    const PresentationFxDescription *description)
{
    if (description->type < PRESENTATION_FX_FLASH ||
        description->type >= PRESENTATION_FX_TYPE_COUNT ||
        description->priority < PRESENTATION_FX_PRIORITY_LOW ||
        description->priority >= PRESENTATION_FX_PRIORITY_COUNT ||
        !PresentationFxVectorIsFinite(description->start) ||
        !PresentationFxVectorIsFinite(description->end) ||
        !isfinite(description->startRadius) ||
        !isfinite(description->endRadius) || !isfinite(description->width) ||
        !isfinite(description->intensity) || !isfinite(description->lifetime) ||
        description->intensity <= 0.0f || description->lifetime <= 0.0f) {
        return false;
    }

    switch (description->type) {
    case PRESENTATION_FX_FLASH:
    case PRESENTATION_FX_GLOW:
        return description->startRadius >= 0.0f &&
               description->endRadius > 0.0f;
    case PRESENTATION_FX_RING:
        return description->startRadius >= 0.0f &&
               description->endRadius > 0.0f && description->width > 0.0f;
    case PRESENTATION_FX_LINE:
    case PRESENTATION_FX_TRAIL:
        return description->width > 0.0f;
    default:
        return false;
    }
}

static float PresentationFxProgress(const PresentationFx *effect)
{
    return effect->age / effect->description.lifetime;
}

/* When full, replace the lowest-priority effect nearest expiration. An
   incoming effect may never evict a higher-priority one. Replacement still
   increments `dropped`, because one requested visual instance was lost. */
static uint16_t PresentationFxReplacementIndex(
    const PresentationFxSystem *system)
{
    uint16_t replacement = 0u;
    uint16_t index;

    for (index = 1u; index < system->stats.active; ++index) {
        const PresentationFx *candidate = &system->effects[index];
        const PresentationFx *current = &system->effects[replacement];

        if (candidate->description.priority < current->description.priority ||
            (candidate->description.priority == current->description.priority &&
             PresentationFxProgress(candidate) >
                 PresentationFxProgress(current))) {
            replacement = index;
        }
    }
    return replacement;
}

static float PresentationFxClamp(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

void PresentationFxInit(PresentationFxSystem *system)
{
    if (system == NULL) {
        return;
    }
    memset(system, 0, sizeof(*system));
}

void PresentationFxClear(PresentationFxSystem *system)
{
    PresentationFxInit(system);
}

bool PresentationFxSpawn(PresentationFxSystem *system,
                         PresentationFxDescription description)
{
    uint16_t slot;

    if (system == NULL) {
        return false;
    }
    if (!PresentationFxDescriptionIsValid(&description)) {
        PresentationFxCountDrop(system);
        return false;
    }

    if (system->stats.active < PRESENTATION_FX_CAPACITY) {
        slot = system->stats.active++;
        if (system->stats.active > system->stats.peak) {
            system->stats.peak = system->stats.active;
        }
    } else {
        slot = PresentationFxReplacementIndex(system);
        if (description.priority <
            system->effects[slot].description.priority) {
            PresentationFxCountDrop(system);
            return false;
        }
        PresentationFxCountDrop(system);
    }

    system->effects[slot] = (PresentationFx){
        .description = description,
        .age = 0.0f,
    };
    return true;
}

uint16_t PresentationFxConsumeEvents(PresentationFxSystem *system,
                                     const GameEventBuffer *events)
{
    uint16_t spawned = 0u;
    uint16_t index;

    if (system == NULL || events == NULL) {
        return 0u;
    }

    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];

        switch (event->type) {
        case GAME_EVENT_EXPLOSION: {
            float radius = PresentationFxClamp(event->radius, 18.0f, 96.0f);

            if (PresentationFxSpawn(system, (PresentationFxDescription){
                    .type = PRESENTATION_FX_FLASH,
                    .priority = PRESENTATION_FX_PRIORITY_HIGH,
                    .start = event->position,
                    .color = {255, 205, 112, 255},
                    .startRadius = 3.0f,
                    .endRadius = radius * 0.48f,
                    .intensity = 0.82f,
                    .lifetime = 0.11f,
                    .emissive = true,
                })) {
                ++spawned;
            }
            if (PresentationFxSpawn(system, (PresentationFxDescription){
                    .type = PRESENTATION_FX_RING,
                    .priority = PRESENTATION_FX_PRIORITY_HIGH,
                    .start = event->position,
                    .color = {255, 171, 73, 255},
                    .startRadius = 6.0f,
                    .endRadius = radius,
                    .width = 1.35f,
                    .intensity = 0.88f,
                    .lifetime = 0.32f,
                    .emissive = true,
                })) {
                ++spawned;
            }
            break;
        }
        case GAME_EVENT_PLAYER_IMPACT: {
            float radius = 1.8f +
                           PresentationFxClamp(event->strength * 0.018f,
                                               0.0f, 4.2f);

            if (PresentationFxSpawn(system, (PresentationFxDescription){
                    .type = PRESENTATION_FX_FLASH,
                    .priority = PRESENTATION_FX_PRIORITY_NORMAL,
                    .start = event->position,
                    .color = {255, 196, 104, 255},
                    .startRadius = 0.8f,
                    .endRadius = radius,
                    .intensity = 0.62f,
                    .lifetime = 0.09f,
                    .emissive = true,
                })) {
                ++spawned;
            }
            break;
        }
        default:
            break;
        }
    }
    return spawned;
}

void PresentationFxUpdate(PresentationFxSystem *system, float deltaTime)
{
    uint16_t index = 0u;

    if (system == NULL || !isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }

    while (index < system->stats.active) {
        PresentationFx *effect = &system->effects[index];

        effect->age += deltaTime;
        if (effect->age >= effect->description.lifetime) {
            --system->stats.active;
            system->effects[index] = system->effects[system->stats.active];
            system->effects[system->stats.active] = (PresentationFx){0};
            continue;
        }
        ++index;
    }
}

const PresentationFxStats *PresentationFxGetStats(
    const PresentationFxSystem *system)
{
    static const PresentationFxStats empty = {0};

    return system != NULL ? &system->stats : &empty;
}
