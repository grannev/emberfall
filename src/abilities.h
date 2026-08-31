#ifndef ABILITIES_H
#define ABILITIES_H

/* The player's powers, as a small registry rather than a growing pile of named
 * fields and boolean parameters.
 *
 * Adding one used to mean editing main.c, input.c, game_input.h, powers.h,
 * powers.c, the renderer, game.c and audio.c, with a new bool threaded through
 * two signatures. Now an ability is one enum id, one table entry, one apply
 * function, one input binding and its drawing. The driver below owns triggers,
 * cooldowns and follow-through for every ability, so none of them reimplements
 * that.
 *
 * This deliberately stops short of data-driven abilities: what a power *does*
 * to a cellular world is real code and belongs in a C function. Only the parts
 * that are the same for every ability are tabulated.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "game_events.h"
#include "particles.h"
#include "player.h"
#include "rng.h"
#include "world.h"

typedef enum AbilityId {
    ABILITY_LASER = 0,
    ABILITY_EXPLOSION,
    ABILITY_FORCE,
    ABILITY_CRYO,
    ABILITY_COUNT
} AbilityId;

/* HELD runs on every frame the control is down. PRESSED fires once per press
   and is the right choice whenever the power has a moment of impact: a held
   force gust reads as weak however large its numbers are. */
typedef enum AbilityTrigger {
    ABILITY_TRIGGER_HELD = 0,
    ABILITY_TRIGGER_PRESSED
} AbilityTrigger;

/* Tuning shared by an ability's simulation and its drawing. One definition
   each, because a cone drawn at a different angle from the one that was
   applied is a bug nobody sees until they look closely. */
#define ABILITY_LASER_RANGE 280.0f
#define ABILITY_LASER_RADIUS 2.25f
#define ABILITY_CRYO_RANGE 190.0f
#define ABILITY_CRYO_RADIUS 2.6f
#define ABILITY_FORCE_LENGTH 84.0f
#define ABILITY_FORCE_SPREAD_COSINE 0.78f
#define ABILITY_FORCE_REACH 54
#define ABILITY_FORCE_RECOIL 132.0f
#define ABILITY_EXPLOSION_CORE_RADIUS 17
#define ABILITY_EXPLOSION_SHOCK_RADIUS 42.0f
#define ABILITY_EXPLOSION_KNOCKBACK 145.0f

/* What the most recent activation did, in one shape for every ability, so the
   renderer and the event publisher never need a per-ability cast or a
   per-ability field on the system struct. */
typedef struct AbilityState {
    bool active;
    bool triggered;
    bool hit;
    float cooldown;
    /* Seconds of visual follow-through still owed, counted down by the driver.
       Divide by the definition's effectTime for a 0..1 progress. */
    float effectTime;
    Vector2 origin;
    Vector2 endpoint;
    Vector2 direction;
    CellMaterial hitMaterial;
} AbilityState;

/* Everything an ability's simulation may touch. Passing a context rather than
   eight arguments is what keeps adding an ability from widening a signature
   every other ability has to be re-checked against. */
typedef struct AbilityContext {
    World *world;
    ParticleSystem *particles;
    GameEventBuffer *events;
    Rng *rng;
    Vector2 origin;
    Vector2 aim;
    Vector2 direction;
    float deltaTime;
} AbilityContext;

typedef struct AbilityDefinition {
    const char *name;
    AbilityTrigger trigger;
    float cooldown;
    float effectTime;
    /* Pose the player holds while the ability reads as active. */
    PlayerPose pose;
    float poseHold;
    /* The whole of the ability's simulation. It writes `state` to tell
       presentation what happened and pushes events to tell audio and the
       camera; it never draws and never touches the player directly. */
    void (*apply)(const AbilityContext *context, AbilityState *state);
} AbilityDefinition;

typedef struct AbilitySystem {
    Rng rng;
    AbilityId lastUsed;
    AbilityState states[ABILITY_COUNT];
} AbilitySystem;

const AbilityDefinition *AbilityDefinitionAt(AbilityId id);
const AbilityState *AbilityStateAt(const AbilitySystem *abilities, AbilityId id);
const char *AbilitiesCurrentName(const AbilitySystem *abilities);

void AbilitiesInit(AbilitySystem *abilities, uint64_t seed);
/* `requested` is one flag per ability, in AbilityId order: held for HELD
   abilities, the press edge for PRESSED ones. */
void AbilitiesUpdate(AbilitySystem *abilities, World *world,
                     ParticleSystem *particles, GameEventBuffer *events,
                     Vector2 origin, Vector2 aim, float deltaTime,
                     const bool *requested);

/* Rejects a table with a missing name or apply function, the same way
   MaterialsValidate rejects a malformed material. */
bool AbilitiesValidate(void);

#endif
