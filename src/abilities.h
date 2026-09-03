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
#include "terrain_damage.h"
#include "terrain_impulse.h"
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

/* Cells per effect unit.
 *
 * The same job PLAYER_BODY_SCALE does for the figure, and deliberately the same
 * number. Every beam, ring and spark in the game was drawn against a character
 * thirteen cells tall; he is now eight, and a laser as thick as his chest reads
 * as a power holding onto him rather than one he is aiming. It is one shared
 * constant rather than a factor folded into each number so that resizing the
 * hero again moves his powers with him — which is the failure this is fixing.
 *
 * It scales widths, not reaches. How far a beam carries is a decision about the
 * world; how thick it is, is a decision about the body it comes out of. */
#define ABILITY_EFFECT_SCALE 0.62f

/* Tuning shared by an ability's simulation and its drawing. One definition
   each, because a cone drawn at a different angle from the one that was
   applied is a bug nobody sees until they look closely. */
#define ABILITY_LASER_RANGE 280.0f
#define ABILITY_LASER_RADIUS (2.25f * ABILITY_EFFECT_SCALE)
/* Holding the beam on one spot builds toward a detonation. The beam is a
   cutting tool, not a gun, and a tool held against rock long enough should do
   something the rock cannot absorb: the heat has to go somewhere, and after
   this long it goes outward.

   What counts as "the same spot" is measured across the beam, not along it. A
   beam boring into rock walks its own hit point deeper — three cells at a time,
   every two thirds of a second — and that displacement is entirely along the
   beam, so anchoring to a fixed point would call boring a sweep and the charge
   would never complete. Sweeping is lateral. `ABILITY_LASER_DWELL_SLACK` is
   therefore how far the hit point may move *sideways* in one frame and still
   count as held. */
#define ABILITY_LASER_DWELL_TIME 1.4f
#define ABILITY_LASER_DWELL_SLACK 2.0f
#define ABILITY_LASER_BURST_RADIUS 11
#define ABILITY_LASER_BURST_CRACKS 9
#define ABILITY_LASER_BURST_CRACK_LENGTH 26
#define ABILITY_LASER_BURST_SHOCK 26
#define ABILITY_LASER_BURST_IMPULSE 16000.0f
#define ABILITY_CRYO_RANGE 190.0f
#define ABILITY_CRYO_RADIUS (2.6f * ABILITY_EFFECT_SCALE)
#define ABILITY_FORCE_LENGTH 84.0f
#define ABILITY_FORCE_SPREAD_COSINE 0.78f
#define ABILITY_FORCE_REACH 54
/* The blow does not shove the one who threw it. A punch that knocks the player
   backwards reads as recoil from a gun rather than as a strike landing, and it
   takes the player out of position at the exact moment they wanted to be
   there. The force is meant to be felt in what it does to the world — the
   crater, the fractures, the debris and the camera — not in being pushed away
   from it. */
#define ABILITY_FORCE_RECOIL 0.0f
/* What the blow is worth to the camera and the effects. Kept separate from the
   recoil so that "how hard it looks" and "how far it moves the player" are two
   decisions rather than one number doing both jobs. */
#define ABILITY_FORCE_IMPACT_STRENGTH 320.0f
/* The dent the blow leaves, and the fractures out of it. A punch that made a
   neat little hole would read as a gunshot; what it should read as is something
   very heavy landing. */
#define ABILITY_FORCE_CRATER_RADIUS 14
#define ABILITY_FORCE_CRACK_COUNT 5
#define ABILITY_FORCE_CRACK_LENGTH 22
/* How far in front of the player the blow lands when it meets nothing solid. */
#define ABILITY_FORCE_PUNCH_REACH 26.0f
#define ABILITY_EXPLOSION_CORE_RADIUS 17
/* Fractures thrown out of the crater, and how far each may run. Twelve rays
   with one fork each reach much further than the crater itself, which is what
   makes a blast feel like it broke the ground rather than removed part of it. */
#define ABILITY_EXPLOSION_CRACKS 12
#define ABILITY_EXPLOSION_CRACK_LENGTH 34
#define ABILITY_EXPLOSION_SHOCK_RADIUS 42.0f
#define ABILITY_EXPLOSION_KNOCKBACK 145.0f

/* Impulse each power delivers to a terrain body standing at its centre, before
   distance falloff, in mass-cells per second. There is no second coefficient
   and no angular scale: spin comes from where the blow lands relative to the
   centre of mass, and how far a body actually travels comes from dividing these
   by its mass. A hundred-cell block of rock weighs 260, so the explosion figure
   is roughly "a small block is thrown at a hundred cells a second, a piece ten
   times heavier at a tenth of that". */
/* How much of a body an explosion eats. Smaller than the core radius it takes
   out of the static world: a slab is already loose, and a blast that swallowed
   it whole would leave nothing to throw. */
#define ABILITY_EXPLOSION_BODY_CARVE 9.0f
#define ABILITY_EXPLOSION_BODY_IMPULSE 26000.0f
#define ABILITY_FORCE_BODY_IMPULSE 42000.0f
/* Cells the blow takes out of a body it lands on. Smaller than the crater it
   leaves in the ground: a slab is already loose, and a punch that swallowed it
   would leave nothing to send flying. */
#define ABILITY_FORCE_BODY_CARVE 6.0f

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
    /* Where the beam has been resting and for how long. Presentation reads
       `dwell` as a 0..1 charge toward the detonation. */
    Vector2 dwellPoint;
    float dwellTime;
} AbilityState;

/* Everything an ability's simulation may touch. Passing a context rather than
   eight arguments is what keeps adding an ability from widening a signature
   every other ability has to be re-checked against. */
typedef struct AbilityContext {
    World *world;
    /* Detached terrain a power can act on. Both may be NULL in a caller that
       has no bodies — a headless probe, a test of the world half alone. */
    DynamicTerrainSystem *terrain;
    TerrainDamageSystem *damage;
    /* Where a power describes the shove it wants to give detached terrain. It
       cannot apply one itself: the fragment a blast sets free does not exist
       until the connectivity check runs, which happens on the fixed step after
       every ability has had its turn. May be NULL in a caller that has no
       terrain bodies. */
    TerrainImpulseSystem *impulses;
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
                     DynamicTerrainSystem *terrain, TerrainDamageSystem *damage,
                     TerrainImpulseSystem *impulses, ParticleSystem *particles,
                     GameEventBuffer *events, Vector2 origin, Vector2 aim,
                     float deltaTime, const bool *requested);

/* Rejects a table with a missing name or apply function, the same way
   MaterialsValidate rejects a malformed material. */
bool AbilitiesValidate(void);

#endif
