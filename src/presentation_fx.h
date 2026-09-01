#ifndef PRESENTATION_FX_H
#define PRESENTATION_FX_H

/* Short-lived, presentation-only world-space effects.
 *
 * This is deliberately a bounded list of drawing primitives, not another
 * particle engine. Gameplay publishes GameEvent values; the presentation
 * layer converts selected events into instances stored here. Nothing in this
 * module can read or mutate World, Player, or Ability state.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "game_events.h"

#define PRESENTATION_FX_CAPACITY 128u

_Static_assert(PRESENTATION_FX_CAPACITY <= UINT16_MAX,
               "presentation FX count must fit its telemetry fields");

typedef enum PresentationFxType {
    PRESENTATION_FX_FLASH = 0,
    PRESENTATION_FX_RING,
    PRESENTATION_FX_GLOW,
    PRESENTATION_FX_LINE,
    PRESENTATION_FX_TRAIL,
    PRESENTATION_FX_PUFF,
    PRESENTATION_FX_TYPE_COUNT
} PresentationFxType;

typedef enum PresentationFxPriority {
    PRESENTATION_FX_PRIORITY_LOW = 0,
    PRESENTATION_FX_PRIORITY_NORMAL,
    PRESENTATION_FX_PRIORITY_HIGH,
    PRESENTATION_FX_PRIORITY_COUNT
} PresentationFxPriority;

/* One compact shape covers the five primitives without callbacks or heap-owned
   payloads. `start` is the centre for radial effects and the first endpoint
   for line/trail effects. Irrelevant fields remain zero. */
typedef struct PresentationFxDescription {
    PresentationFxType type;
    PresentationFxPriority priority;
    Vector2 start;
    Vector2 end;
    Color color;
    float startRadius;
    float endRadius;
    float width;
    float intensity;
    float lifetime;
    /* A small presentation-side delay lets one gameplay event describe a
       readable attack -> impact -> decay sequence without timers in gameplay. */
    float delay;
    bool emissive;
} PresentationFxDescription;

typedef struct PresentationFx {
    PresentationFxDescription description;
    float age;
} PresentationFx;

typedef struct PresentationFxStats {
    uint16_t active;
    uint16_t peak;
    uint32_t dropped;
} PresentationFxStats;

typedef struct PresentationFxSystem {
    PresentationFx effects[PRESENTATION_FX_CAPACITY];
    PresentationFxStats stats;
    uint32_t randomState;
    Vector2 lastLaserContact;
    float laserContactTime;
    float laserSpawnCooldown;
    float cryoSpawnCooldown;
    float drillSpawnCooldown;
    bool laserContactValid;
} PresentationFxSystem;

void PresentationFxInit(PresentationFxSystem *system);
void PresentationFxClear(PresentationFxSystem *system);
bool PresentationFxSpawn(PresentationFxSystem *system,
                         PresentationFxDescription description);
uint16_t PresentationFxConsumeEvents(PresentationFxSystem *system,
                                     const GameEventBuffer *events);
void PresentationFxUpdate(PresentationFxSystem *system, float deltaTime);
const PresentationFxStats *PresentationFxGetStats(
    const PresentationFxSystem *system);

#endif
