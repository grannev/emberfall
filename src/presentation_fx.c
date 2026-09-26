#include "presentation_fx.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <raymath.h>

#include "materials.h"

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
    float radius = PresentationFxClamp(event->radius, 18.0f, 116.0f);
    int index;

    /* Large charged detonations grow a broken column and crown after the
       shock front. Nine bounded puffs, entirely in the presentation pool. */
    if (event->radius > 70.0f) {
        for (index = 0; index < 9; ++index) {
            bool crown = index >= 4;
            float x = crown ? (float)(index - 6) * radius * 0.12f : 0.0f;
            float y = crown ? -radius * 0.55f : -(float)index * radius * 0.12f;
            (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
                .type = PRESENTATION_FX_PUFF,
                .priority = PRESENTATION_FX_PRIORITY_NORMAL,
                .start = {event->position.x + x, event->position.y + y},
                .color = crown ? (Color){152, 130, 104, 220} : (Color){221, 147, 69, 200},
                .startRadius = 2.0f,
                .endRadius = radius * (crown ? 0.17f : 0.09f),
                .intensity = crown ? 0.45f : 0.55f,
                .lifetime = 1.25f,
                .delay = 0.10f + (float)index * 0.035f,
                .emissive = !crown,
            }, spawned);
        }
    }

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

static void PresentationFxSpawnLanding(PresentationFxSystem *system,
                                        const GameEvent *event, uint16_t *spawned)
{
    int index;
    float power = PresentationFxClamp(event->strength, 0.0f, 1.0f);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING, .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position, .color = {226, 214, 182, 255},
        .startRadius = 2.0f, .endRadius = event->radius,
        .width = 1.0f, .intensity = 0.7f, .lifetime = 0.32f,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_FLASH, .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = event->position, .color = {253, 219, 160, 255},
        .startRadius = 1.0f, .endRadius = 3.0f + power * 6.0f,
        .intensity = 0.65f, .lifetime = 0.08f, .emissive = true,
    }, spawned);
    for (index = -3; index <= 3; ++index) {
        float spread = (float)index * event->radius * 0.15f;
        Vector2 start = {event->position.x + spread, event->position.y - 1.0f};
        Vector2 end = {start.x + spread * 0.65f,
                       start.y - (5.0f + power * 12.0f) + fabsf((float)index)};
        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_TRAIL, .priority = PRESENTATION_FX_PRIORITY_NORMAL,
            .start = start, .end = end, .color = {183, 168, 144, 235},
            .width = 1.0f + power, .intensity = 0.5f, .lifetime = 0.24f,
        }, spawned);
        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_PUFF, .priority = PRESENTATION_FX_PRIORITY_LOW,
            .start = end, .color = {143, 130, 113, 210},
            .startRadius = 1.0f, .endRadius = 3.0f + power * 5.0f,
            .intensity = 0.45f, .lifetime = 0.55f, .delay = 0.05f,
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
        float distance = fminf(radius * 0.1f, 13.0f) *
                         (0.35f + 0.15f * (float)(index + 2));
        Vector2 center = Vector2Add(
            Vector2Add(event->position, Vector2Scale(direction, -distance)),
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
    Color color = {211, 228, 225, 255};

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

static void PresentationFxSpawnSonic(PresentationFxSystem *system,
                                     const GameEvent *event,
                                     uint16_t *spawned)
{
    Vector2 back = Vector2Scale(PresentationFxDirection(event->direction,
                                                        (Vector2){1.0f, 0.0f}), -5.0f);
    Vector2 centre = Vector2Add(event->position, back);

    /* A one-time compression shell at Mach, behind the continuous bow at the
       nose. The shock does not draw speed lines and does not exist in vacuum. */
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_HIGH,
        .start = centre,
        .color = {191, 222, 225, 255},
        .startRadius = 8.0f,
        .endRadius = 42.0f,
        .width = 1.2f,
        .intensity = 0.55f * event->strength,
        .lifetime = 0.24f,
        .emissive = true,
    }, spawned);
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = centre,
        .color = {118, 154, 169, 255},
        .startRadius = 10.0f,
        .endRadius = 55.0f,
        .width = 0.85f,
        .intensity = 0.3f * event->strength,
        .lifetime = 0.34f,
        .delay = 0.035f,
    }, spawned);
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

/* The colour of a liquid's spray: the material's own, lightened, so lava
   throws embers and water throws foam. */
static Color PresentationFxLiquidColor(CellMaterial material, unsigned char alpha)
{
    Color base = MaterialAt(material)->color;

    return (Color){(unsigned char)(base.r + (255 - base.r) / 2),
                   (unsigned char)(base.g + (255 - base.g) / 2),
                   (unsigned char)(base.b + (255 - base.b) / 2), alpha};
}

/* A splash: a ring spreading on the surface and a fan of droplets thrown up
   and away from the way the thing was going. Sized by the speed and, for a
   body, by how much of it went in. */
static void PresentationFxSpawnSplash(PresentationFxSystem *system,
                                      const GameEvent *event, uint16_t *spawned)
{
    float size = PresentationFxClamp(event->strength / 120.0f, 0.25f, 2.0f) +
                 PresentationFxClamp((float)event->count / 400.0f, 0.0f, 1.5f);
    Color color = PresentationFxLiquidColor(event->material, 235);
    bool glowing = event->material == MATERIAL_LAVA;
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){0.0f, 1.0f});
    int droplets = 3 + (int)(size * 3.0f);
    int index;

    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_NORMAL,
        .start = event->position,
        .color = color,
        .startRadius = 1.5f,
        .endRadius = 6.0f + 9.0f * size,
        .width = 0.9f,
        .intensity = 0.55f,
        .lifetime = 0.34f + 0.12f * size,
        .emissive = glowing,
    }, spawned);
    for (index = 0; index < droplets; ++index) {
        /* Up and out, leaning away from the direction of travel: a diver
           throws spray behind them, a body falling in throws it all round. */
        float angle = -PI * 0.5f +
                      PresentationFxRandomRange(system, -0.9f, 0.9f) -
                      direction.x * 0.5f;
        float reach = PresentationFxRandomRange(system, 5.0f, 9.0f + 10.0f * size);
        Vector2 spray = {cosf(angle), sinf(angle)};

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_TRAIL,
            .priority = PRESENTATION_FX_PRIORITY_LOW,
            .start = event->position,
            .end = Vector2Add(event->position, Vector2Scale(spray, reach)),
            .color = color,
            .width = 0.7f,
            .intensity = 0.5f,
            .lifetime = 0.22f + 0.1f * size,
            .delay = 0.01f * (float)index,
            .emissive = glowing,
        }, spawned);
    }
}

/* A ripple: a low, wide ring on the surface, nothing thrown. */
static void PresentationFxSpawnRipple(PresentationFxSystem *system,
                                      const GameEvent *event, uint16_t *spawned)
{
    (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
        .type = PRESENTATION_FX_RING,
        .priority = PRESENTATION_FX_PRIORITY_LOW,
        .start = event->position,
        .color = PresentationFxLiquidColor(event->material, 170),
        .startRadius = 1.0f,
        .endRadius = 3.0f + event->radius,
        .width = 0.6f,
        .intensity = 0.3f,
        .lifetime = 0.3f,
        .emissive = event->material == MATERIAL_LAVA,
    }, spawned);
}

/* Re-entry: short turbulent hot-air clumps off the shoulders. The cap in front
   belongs to reentry_renderer.c; world-space trail primitives spawned every
   few frames connected into ruler-straight orange stripes at flight speed. */
static void PresentationFxSpawnReentry(PresentationFxSystem *system,
                                       const GameEvent *event, uint16_t *spawned)
{
    float heat = PresentationFxClamp(event->strength, 0.0f, 1.0f);
    float size = event->radius > 0.5f ? event->radius : 0.5f;
    Vector2 direction = PresentationFxDirection(event->direction,
                                                (Vector2){0.0f, 1.0f});
    Vector2 normal = {-direction.y, direction.x};
    int index;

    for (index = 0; index < 3; ++index) {
        float across = PresentationFxRandomRange(system, -1.25f, 1.25f) * size;
        float behind = PresentationFxRandomRange(system, 0.5f, 1.7f) * size;
        Vector2 at = Vector2Add(
            Vector2Subtract(event->position, Vector2Scale(direction, behind)),
            Vector2Scale(normal, across));

        (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
            .type = PRESENTATION_FX_PUFF,
            .priority = PRESENTATION_FX_PRIORITY_LOW,
            .start = at,
            .color = {255, (unsigned char)(150.0f + 60.0f * (1.0f - heat)), 70, 255},
            .startRadius = size * 0.12f,
            .endRadius = size * 0.45f,
            .intensity = 0.22f + 0.28f * heat,
            .lifetime = 0.14f + 0.11f * heat,
            .emissive = true,
        }, spawned);
    }
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
        case GAME_EVENT_HEAVY_LANDING:
            PresentationFxSpawnLanding(system, event, &spawned);
            break;
        case GAME_EVENT_FOOTSTEP:
        case GAME_EVENT_TAKEOFF: {
            bool takeoff = event->type == GAME_EVENT_TAKEOFF;
            (void)PresentationFxSpawnCounted(system, (PresentationFxDescription){
                .type = takeoff ? PRESENTATION_FX_RING : PRESENTATION_FX_PUFF,
                .priority = PRESENTATION_FX_PRIORITY_LOW,
                .start = event->position, .color = {175, 170, 154, 190},
                .startRadius = 0.5f,
                .endRadius = takeoff ? 5.0f + event->strength * 6.0f :
                                       1.0f + event->strength * 2.0f,
                .intensity = takeoff ? 0.38f : 0.24f,
                .width = 0.6f, .lifetime = takeoff ? 0.22f : 0.18f,
            }, &spawned);
            break;
        }
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
        case GAME_EVENT_SONIC_BREAK:
            PresentationFxSpawnSonic(system, event, &spawned);
            break;
        case GAME_EVENT_PLAYER_DRILL:
            if (system->drillSpawnCooldown <= 0.0f) {
                PresentationFxSpawnDrill(system, event, &spawned);
                system->drillSpawnCooldown = 0.035f;
            }
            break;
        case GAME_EVENT_LIQUID_SPLASH:
            /* Only for the character. A body's splash is the water itself,
               thrown up in a crown by the gameplay push, and a ring drawn
               over it read as a hit effect on the body rather than as
               water; it was removed at the player's request. */
            if (event->count == 0) {
                PresentationFxSpawnSplash(system, event, &spawned);
            }
            break;
        case GAME_EVENT_LIQUID_RIPPLE:
            PresentationFxSpawnRipple(system, event, &spawned);
            break;
        case GAME_EVENT_REENTRY:
            if (system->reentrySpawnCooldown <= 0.0f) {
                PresentationFxSpawnReentry(system, event, &spawned);
                system->reentrySpawnCooldown = 0.05f;
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

void PresentationFxShift(PresentationFxSystem *system, float dx)
{
    uint16_t index;

    if (system == NULL) {
        return;
    }
    for (index = 0u; index < PRESENTATION_FX_CAPACITY; ++index) {
        system->effects[index].description.start.x += dx;
        system->effects[index].description.end.x += dx;
    }
    system->lastLaserContact.x += dx;
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
    system->reentrySpawnCooldown = fmaxf(0.0f,
                                         system->reentrySpawnCooldown - deltaTime);
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
