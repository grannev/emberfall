#include "presentation_fx.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <raymath.h>

#define PRESENTATION_FX_RANDOM_SEED 0x4f1bbcddu
#define PRESENTATION_FX_MAX_DELAY 5.0f

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
        !isfinite(description->delay) || description->delay < 0.0f ||
        description->delay > PRESENTATION_FX_MAX_DELAY ||
        description->intensity <= 0.0f || description->lifetime <= 0.0f) {
        return false;
    }

    switch (description->type) {
    case PRESENTATION_FX_FLASH:
    case PRESENTATION_FX_GLOW:
    case PRESENTATION_FX_PUFF:
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

static float PresentationFxClamp(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static float PresentationFxProgress(const PresentationFx *effect)
{
    return PresentationFxClamp(effect->age / effect->description.lifetime,
                               0.0f, 1.0f);
}

static uint32_t PresentationFxRandomU32(PresentationFxSystem *system)
{
    uint32_t value = system->randomState;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    system->randomState = value;
    return value;
}

static float PresentationFxRandomUnit(PresentationFxSystem *system)
{
    return (float)(PresentationFxRandomU32(system) & 0xffffu) / 65535.0f;
}

static float PresentationFxRandomRange(PresentationFxSystem *system,
                                       float minimum, float maximum)
{
    return minimum + (maximum - minimum) * PresentationFxRandomUnit(system);
}

static Vector2 PresentationFxDirection(Vector2 value, Vector2 fallback)
{
    float length = Vector2Length(value);

    return length > 0.001f ? Vector2Scale(value, 1.0f / length) : fallback;
}

static bool PresentationFxSpawnCounted(PresentationFxSystem *system,
                                       PresentationFxDescription description,
                                       uint16_t *spawned)
{
    if (!PresentationFxSpawn(system, description)) {
        return false;
    }
    if (*spawned < UINT16_MAX) {
        ++*spawned;
    }
    return true;
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

void PresentationFxInit(PresentationFxSystem *system)
{
    if (system == NULL) {
        return;
    }
    memset(system, 0, sizeof(*system));
    system->randomState = PRESENTATION_FX_RANDOM_SEED;
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
        .age = -description.delay,
    };
    return true;
}

static void PresentationFxSpawnExplosion(PresentationFxSystem *system,
                                         const GameEvent *event,
                                         uint16_t *spawned)
{
    float radius = PresentationFxClamp(event->radius, 18.0f, 96.0f);
    int index;

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_FLASH,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position,
        .color = {255, 233, 176, 255},
        .startRadius = 2.0f,
        .endRadius = radius * 0.55f,
        .intensity = 0.96f,
        .lifetime = 0.075f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_GLOW,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position,
        .color = {255, 139, 43, 255},
        .startRadius = radius * 0.20f,
        .endRadius = radius * 0.10f,
        .intensity = 0.94f,
        .lifetime = 0.28f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position,
        .color = {255, 183, 82, 255},
        .startRadius = radius * 0.12f,
        .endRadius = radius,
        .width = 1.55f,
        .intensity = 0.92f,
        .lifetime = 0.34f,
        .delay = 0.025f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = {220, 204, 178, 255},
        .startRadius = radius * 0.20f,
        .endRadius = radius * 1.18f,
        .width = 1.0f,
        .intensity = 0.46f,
        .lifetime = 0.46f,
        .delay = 0.075f,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_GLOW,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = {255, 92, 28, 255},
        .startRadius = radius * 0.08f,
        .endRadius = radius * 0.32f,
        .intensity = 0.30f,
        .lifetime = 0.72f,
        .delay = 0.16f,
        .emissive = true,
    }, spawned);

    for (index = 0; index < 10; ++index) {
        float angle = PresentationFxRandomRange(system, 0.0f, 2.0f * PI);
        float length = PresentationFxRandomRange(system, radius * 0.32f,
                                                 radius * 0.92f);
        Vector2 direction = {cosf(angle), sinf(angle)};
        Vector2 start = Vector2Add(
            event->position,
            Vector2Scale(direction, PresentationFxRandomRange(system, 1.0f, 5.0f)));
        Vector2 end = Vector2Add(start, Vector2Scale(direction, length));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_TRAIL,
            .priority = index < 4 ? PRESENTATION_FX_PRIORITY_HIGH
                                  : PRESENTATION_FX_PRIORITY_NORMAL,
            .start = start,
            .end = end,
            .color = index < 4 ? (Color){255, 242, 173, 255}
                               : (Color){255, 128, 38, 255},
            .width = PresentationFxRandomRange(system, 0.55f, 1.25f),
            .intensity = PresentationFxRandomRange(system, 0.62f, 0.94f),
            .lifetime = PresentationFxRandomRange(system, 0.18f, 0.40f),
            .delay = PresentationFxRandomRange(system, 0.0f, 0.045f),
            .emissive = true,
        }, spawned);
    }
    for (index = 0; index < 7; ++index) {
        float angle = PresentationFxRandomRange(system, 0.0f, 2.0f * PI);
        float offset = PresentationFxRandomRange(system, radius * 0.10f,
                                                 radius * 0.56f);
        Vector2 center = {event->position.x + cosf(angle) * offset,
                          event->position.y + sinf(angle) * offset};
        unsigned char shade = (unsigned char)PresentationFxRandomRange(
            system, 108.0f, 164.0f);

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_PUFF,
            .priority = PRESENTATION_FX_PRIORITY_LOW,
            .start = center,
            .color = {shade, (unsigned char)(shade * 0.82f),
                      (unsigned char)(shade * 0.62f), 220},
            .startRadius = PresentationFxRandomRange(system, 2.0f, 5.0f),
            .endRadius = PresentationFxRandomRange(system, 8.0f, 15.0f),
            .intensity = PresentationFxRandomRange(system, 0.28f, 0.48f),
            .lifetime = PresentationFxRandomRange(system, 0.65f, 1.05f),
            .delay = PresentationFxRandomRange(system, 0.08f, 0.22f),
        }, spawned);
    }
}

static void PresentationFxSpawnLaserContact(PresentationFxSystem *system,
                                             const GameEvent *event,
                                             uint16_t *spawned)
{
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){1.0f, 0.0f});
    Vector2 normal = {-direction.y, direction.x};
    float heat = PresentationFxClamp(system->laserContactTime / 0.55f,
                                     0.0f, 1.0f);
    float side = PresentationFxRandomRange(system, -1.0f, 1.0f);
    Vector2 sparkDirection = Vector2Normalize(Vector2Add(
        Vector2Scale(direction, -1.0f), Vector2Scale(normal, side * 1.25f)));

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_FLASH,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = {255, 229, 154, 255},
        .startRadius = 0.7f,
        .endRadius = 2.5f + heat * 1.7f,
        .intensity = 0.68f + heat * 0.18f,
        .lifetime = 0.075f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_GLOW,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = heat > 0.55f ? (Color){255, 91, 27, 255}
                             : (Color){255, 151, 48, 255},
        .startRadius = 1.4f + heat,
        .endRadius = 3.2f + heat * 2.4f,
        .intensity = 0.34f + heat * 0.22f,
        .lifetime = 0.18f + heat * 0.12f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_TRAIL,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .end = Vector2Add(event->position,
                          Vector2Scale(sparkDirection,
                                       PresentationFxRandomRange(system, 5.0f, 12.0f))),
        .color = {255, 219, 112, 255},
        .width = 0.7f,
        .intensity = 0.84f,
        .lifetime = 0.13f,
        .emissive = true,
    }, spawned);
}

static void PresentationFxSpawnCryoContact(PresentationFxSystem *system,
                                            const GameEvent *event,
                                            uint16_t *spawned)
{
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){1.0f, 0.0f});
    Vector2 normal = {-direction.y, direction.x};
    int index;

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_FLASH,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = {228, 251, 255, 255},
        .startRadius = 0.8f,
        .endRadius = 3.6f,
        .intensity = 0.72f,
        .lifetime = 0.10f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = {137, 224, 255, 255},
        .startRadius = 1.2f,
        .endRadius = 7.0f,
        .width = 0.75f,
        .intensity = 0.68f,
        .lifetime = 0.27f,
    }, spawned);
    for (index = -1; index <= 1; index += 2) {
        Vector2 shard = Vector2Normalize(Vector2Add(
            Vector2Scale(direction, -0.45f),
            Vector2Scale(normal, (float)index)));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_LINE,
            .priority = PRESENTATION_FX_PRIORITY_LOW,
            .start = event->position,
            .end = Vector2Add(event->position, Vector2Scale(shard, 5.5f)),
            .color = {198, 243, 255, 255},
            .width = 0.55f,
            .intensity = 0.66f,
            .lifetime = 0.20f,
        }, spawned);
    }
}

static void PresentationFxSpawnForce(PresentationFxSystem *system,
                                     const GameEvent *event,
                                     uint16_t *spawned)
{
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){1.0f, 0.0f});
    Vector2 normal = {-direction.y, direction.x};
    float radius = PresentationFxClamp(event->radius, 24.0f, 128.0f);
    int index;

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_FLASH,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position,
        .color = {220, 242, 255, 255},
        .startRadius = 1.5f,
        .endRadius = 8.0f,
        .intensity = 0.76f,
        .lifetime = 0.10f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position,
        .color = {176, 222, 255, 255},
        .startRadius = 4.0f,
        .endRadius = 22.0f,
        .width = 1.0f,
        .intensity = 0.62f,
        .lifetime = 0.24f,
    }, spawned);
    for (index = -2; index <= 2; ++index) {
        float across = (float)index * radius * 0.07f;
        int distanceFromCenter = index < 0 ? -index : index;
        float reach = radius *
                      (0.66f + 0.07f * (float)(2 - distanceFromCenter));
        Vector2 start = Vector2Add(event->position,
                                   Vector2Scale(normal, across * 0.18f));
        Vector2 end = Vector2Add(
            Vector2Add(event->position, Vector2Scale(direction, reach)),
            Vector2Scale(normal, across));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_TRAIL,
            .priority = PRESENTATION_FX_PRIORITY_NORMAL,
            .start = start,
            .end = end,
            .color = {172, 216, 242, 255},
            .width = index == 0 ? 1.15f : 0.65f,
            .intensity = index == 0 ? 0.55f : 0.34f,
            .lifetime = 0.25f,
        }, spawned);
    }
    for (index = -2; index <= 2; ++index) {
        float distance = radius * (0.38f + 0.09f * (float)(index + 2));
        Vector2 center = Vector2Add(
            Vector2Add(event->position, Vector2Scale(direction, distance)),
            Vector2Scale(normal, PresentationFxRandomRange(system, -7.0f, 7.0f)));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_PUFF,
            .priority = PRESENTATION_FX_PRIORITY_LOW,
            .start = center,
            .color = {150, 160, 170, 210},
            .startRadius = 1.5f,
            .endRadius = 5.5f,
            .intensity = 0.24f,
            .lifetime = 0.42f,
            .delay = 0.02f * (float)(index + 2),
        }, spawned);
    }
}

static void PresentationFxSpawnBoost(PresentationFxSystem *system,
                                     const GameEvent *event,
                                     uint16_t *spawned)
{
    int stage = event->count < 1 ? 1 : (event->count > 3 ? 3 : event->count);
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){1.0f, 0.0f});
    Vector2 normal = {-direction.y, direction.x};
    Color color = stage == 3 ? (Color){255, 236, 183, 255}
                             : (Color){116, 224, 255, 255};
    int index;

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position,
        .color = color,
        .startRadius = 2.0f,
        .endRadius = 8.0f + (float)stage * 6.0f,
        .width = stage == 3 ? 1.35f : 0.85f,
        .intensity = 0.56f + (float)stage * 0.10f,
        .lifetime = stage == 3 ? 0.46f : 0.28f,
        .emissive = true,
    }, spawned);
    for (index = -stage; index <= stage; ++index) {
        float side = (float)index * 2.2f;
        Vector2 start = Vector2Add(event->position, Vector2Scale(normal, side));
        Vector2 end = Vector2Add(
            Vector2Add(start, Vector2Scale(
                direction, -(12.0f + (float)stage * 6.0f))),
            Vector2Scale(normal, PresentationFxRandomRange(system, -2.0f, 2.0f)));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_TRAIL,
            .priority = stage == 3 ? PRESENTATION_FX_PRIORITY_HIGH
                                   : PRESENTATION_FX_PRIORITY_NORMAL,
            .start = start,
            .end = end,
            .color = color,
            .width = index == 0 ? 1.25f : 0.65f,
            .intensity = 0.46f + (float)stage * 0.12f,
            .lifetime = 0.18f + (float)stage * 0.07f,
            .emissive = true,
        }, spawned);
    }
    if (stage == 3) {
        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_RING,
            .priority = PRESENTATION_FX_PRIORITY_HIGH,
            .start = event->position,
            .color = {218, 241, 255, 255},
            .startRadius = 6.0f,
            .endRadius = 31.0f,
            .width = 0.8f,
            .intensity = 0.62f,
            .lifetime = 0.40f,
            .delay = 0.055f,
        }, spawned);
    }
}

static void PresentationFxSpawnDrill(PresentationFxSystem *system,
                                     const GameEvent *event,
                                     uint16_t *spawned)
{
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){1.0f, 0.0f});
    Vector2 normal = {-direction.y, direction.x};
    int sparks = event->count > 8 ? 3 : 2;
    int index;

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_GLOW,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = {255, 126, 30, 255},
        .startRadius = 1.8f,
        .endRadius = 4.6f,
        .intensity = 0.58f,
        .lifetime = 0.14f,
        .emissive = true,
    }, spawned);
    for (index = 0; index < sparks; ++index) {
        float side = PresentationFxRandomRange(system, -1.2f, 1.2f);
        Vector2 spark = Vector2Normalize(Vector2Add(
            Vector2Scale(direction, -1.0f), Vector2Scale(normal, side)));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_TRAIL,
            .priority = PRESENTATION_FX_PRIORITY_NORMAL,
            .start = event->position,
            .end = Vector2Add(event->position,
                              Vector2Scale(spark, PresentationFxRandomRange(
                                  system, 4.0f, 10.0f))),
            .color = {255, 195, 77, 255},
            .width = 0.65f,
            .intensity = 0.82f,
            .lifetime = 0.14f,
            .emissive = true,
        }, spawned);
    }
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_PUFF,
        .priority = PRESENTATION_FX_PRIORITY_LOW,
        .start = Vector2Add(event->position, Vector2Scale(direction, -2.0f)),
        .color = event->material == MATERIAL_ROCK
                     ? (Color){122, 112, 105, 220}
                     : (Color){150, 119, 81, 210},
        .startRadius = 1.3f,
        .endRadius = 5.0f,
        .intensity = 0.28f,
        .lifetime = 0.40f,
    }, spawned);
}

uint16_t PresentationFxConsumeEvents(PresentationFxSystem *system,
                                     const GameEventBuffer *events)
{
    uint16_t spawned = 0u;
    uint16_t index;
    bool sawLaser = false;

    if (system == NULL || events == NULL) {
        return 0u;
    }

    for (index = 0u; index < events->count; ++index) {
        const GameEvent *event = &events->events[index];

        switch (event->type) {
        case GAME_EVENT_EXPLOSION:
            PresentationFxSpawnExplosion(system, event, &spawned);
            break;
        case GAME_EVENT_PLAYER_IMPACT: {
            float radius = 1.8f +
                           PresentationFxClamp(event->strength * 0.018f,
                                               0.0f, 4.2f);

            (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
                    .type = PRESENTATION_FX_FLASH,
                    .priority = PRESENTATION_FX_PRIORITY_NORMAL,
                    .start = event->position,
                    .color = {255, 196, 104, 255},
                    .startRadius = 0.8f,
                    .endRadius = radius,
                    .intensity = 0.62f,
                    .lifetime = 0.09f,
                    .emissive = true,
                }, &spawned);
            break;
        }
        case GAME_EVENT_LASER_HIT:
            sawLaser = true;
            if (!system->laserContactValid ||
                Vector2DistanceSqr(system->lastLaserContact, event->position) >
                    64.0f) {
                system->laserContactTime = 0.0f;
            }
            system->laserContactValid = true;
            system->lastLaserContact = event->position;
            if (system->laserSpawnCooldown <= 0.0f) {
                PresentationFxSpawnLaserContact(system, event, &spawned);
                system->laserSpawnCooldown = 0.045f;
            }
            break;
        case GAME_EVENT_CRYO_HIT:
            if (system->cryoSpawnCooldown <= 0.0f) {
                PresentationFxSpawnCryoContact(system, event, &spawned);
                system->cryoSpawnCooldown = 0.075f;
            }
            break;
        case GAME_EVENT_FORCE:
            PresentationFxSpawnForce(system, event, &spawned);
            break;
        case GAME_EVENT_BOOST_ENGAGED:
            PresentationFxSpawnBoost(system, event, &spawned);
            break;
        case GAME_EVENT_PLAYER_DRILL:
            if (system->drillSpawnCooldown <= 0.0f) {
                PresentationFxSpawnDrill(system, event, &spawned);
                system->drillSpawnCooldown = 0.035f;
            }
            break;
        default:
            break;
        }
    }
    if (!sawLaser) {
        system->laserContactValid = false;
        system->laserContactTime = 0.0f;
    }
    return spawned;
}

void PresentationFxUpdate(PresentationFxSystem *system, float deltaTime)
{
    uint16_t index = 0u;

    if (system == NULL || !isfinite(deltaTime) || deltaTime <= 0.0f) {
        return;
    }

    system->laserSpawnCooldown = fmaxf(0.0f,
                                       system->laserSpawnCooldown - deltaTime);
    system->cryoSpawnCooldown = fmaxf(0.0f,
                                      system->cryoSpawnCooldown - deltaTime);
    system->drillSpawnCooldown = fmaxf(0.0f,
                                       system->drillSpawnCooldown - deltaTime);
    /* Contact heat is presentation time, not simulation ticks or render-event
       count. Accumulating here keeps the ramp identical at 30, 60 and 144 Hz;
       ConsumeEvents below either preserves the streak or resets it. */
    if (system->laserContactValid) {
        system->laserContactTime = fminf(1.5f,
                                         system->laserContactTime + deltaTime);
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
