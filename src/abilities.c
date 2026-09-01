/* The ability registry: one table, one apply function per power, one driver.
 *
 * The driver owns everything that is the same for every ability — trigger
 * shape, cooldown, follow-through timer, which power was last used — so an
 * apply function contains only what its own power does to the world.
 */
#include "abilities.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

/* A beam stops at the edge of the map rather than being drawn into the void,
   and clamping the endpoint here keeps every beam's range honest whichever
   direction the player aims. */
static Vector2 BeamEndAtWorldEdge(const World *world, Vector2 origin,
                                  Vector2 direction, float maximumLength)
{
    float length = maximumLength;

    if (direction.x > 0.001f) {
        length = fminf(length, ((float)world->width - 0.5f - origin.x) / direction.x);
    } else if (direction.x < -0.001f) {
        length = fminf(length, (0.5f - origin.x) / direction.x);
    }
    if (direction.y > 0.001f) {
        length = fminf(length, ((float)world->height - 0.5f - origin.y) / direction.y);
    } else if (direction.y < -0.001f) {
        length = fminf(length, (0.5f - origin.y) / direction.y);
    }

    length = fmaxf(length, 0.0f);
    return (Vector2){origin.x + direction.x * length,
                     origin.y + direction.y * length};
}

/* Knockback away from a point, falling off linearly to nothing at `radius`.
   The ability computes what it does to the player and publishes it as an
   event, so the player never has to know which powers exist. */
static Vector2 RadialImpulse(Vector2 target, Vector2 center, float radius,
                             float force)
{
    Vector2 away = {target.x - center.x, target.y - center.y};
    float distance = sqrtf(away.x * away.x + away.y * away.y);
    float strength;

    if (radius <= 0.0f || distance >= radius) {
        return (Vector2){0.0f, 0.0f};
    }
    if (distance < 0.001f) {
        away = (Vector2){0.0f, -1.0f};
        distance = 0.0f;
    } else {
        away.x /= distance;
        away.y /= distance;
    }
    strength = 1.0f - distance / radius;
    return (Vector2){away.x * force * strength, away.y * force * strength};
}

static float BeamDistance(Vector2 from, Vector2 to)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;

    return sqrtf(dx * dx + dy * dy);
}

static void AbilityApplyLaser(const AbilityContext *context, AbilityState *state)
{
    Vector2 reach = BeamEndAtWorldEdge(context->world, context->origin,
                                       context->direction, ABILITY_LASER_RANGE);
    /* Which of the two the beam actually reaches is decided before either of
       them is touched. Burning the wall first and only then noticing that a
       slab stood in front of it damages terrain the shot never arrived at, and
       no amount of later correction puts those cells back. */
    LaserResult result = WorldBeamHit(context->world, context->origin, reach);
    TerrainBodyHandle cutBody;
    Vector2 cutAt;
    bool bodyFirst = false;

    if (context->terrain != NULL && context->damage != NULL &&
        DynamicTerrainRaycast(context->terrain, context->origin, reach, &cutBody,
                              &cutAt)) {
        bodyFirst = !result.hit ||
                    BeamDistance(context->origin, cutAt) <
                        BeamDistance(context->origin, result.position);
    }

    if (bodyFirst) {
        /* Rate-limited: the beam is held, and a bite on every frame would
           evaporate a slab in well under a second. The world is left alone
           entirely — the beam stopped here. */
        if (TerrainDamageBeamReady(context->damage, context->deltaTime)) {
            (void)TerrainDamageApplyCircle(context->damage, context->terrain,
                                           cutBody, cutAt,
                                           context->damage->config.beamCutRadius);
        }
        result.position = cutAt;
        result.material = MATERIAL_ROCK;
        result.hit = true;
    } else {
        /* The wall is what the beam reached, so it is the only thing that takes
           heat. A body behind it is shielded exactly as loose cells are. */
        result = WorldApplyLaser(context->world, context->origin, reach,
                                 ABILITY_LASER_RADIUS, context->deltaTime);
    }

    state->origin = context->origin;
    state->direction = context->direction;
    state->endpoint = result.position;
    state->hit = result.hit;
    state->hitMaterial = result.material;
    if (!result.hit) {
        return;
    }
    /* Sparks on every frame of a held beam would be a solid sheet of them. */
    if (RngRange(context->rng, 0, 1) == 0) {
        ParticlesSpawnLaserSparks(context->particles, result.position,
                                  context->direction);
    }
    (void)GameEventsPush(context->events, (GameEvent){
        .type = GAME_EVENT_LASER_HIT,
        .position = result.position,
        .direction = context->direction,
        .material = result.material,
    });
}

static void AbilityApplyCryo(const AbilityContext *context, AbilityState *state)
{
    Vector2 reach = BeamEndAtWorldEdge(context->world, context->origin,
                                       context->direction, ABILITY_CRYO_RANGE);
    LaserResult result = WorldApplyChill(context->world, context->origin, reach,
                                         ABILITY_CRYO_RADIUS, context->deltaTime);

    state->origin = context->origin;
    state->direction = context->direction;
    state->endpoint = result.position;
    state->hit = result.hit;
    state->hitMaterial = result.material;
    if (result.hit) {
        (void)GameEventsPush(context->events, (GameEvent){
            .type = GAME_EVENT_CRYO_HIT,
            .position = result.position,
            .direction = context->direction,
            .material = result.material,
        });
    }
}

static void AbilityApplyForce(const AbilityContext *context, AbilityState *state)
{
    /* Where the blow lands: the first solid thing along the aim, or a point out
       in front when there is nothing. Finding it first is what lets the crater
       be cut into the surface that was actually struck rather than into
       whatever happens to be under the cursor. */
    LaserResult contact = WorldBeamHit(context->world, context->origin,
                                       (Vector2){context->origin.x +
                                                     context->direction.x *
                                                         ABILITY_FORCE_PUNCH_REACH,
                                                 context->origin.y +
                                                     context->direction.y *
                                                         ABILITY_FORCE_PUNCH_REACH});

    WorldApplyForceBlast(context->world, context->origin, context->direction,
                         ABILITY_FORCE_LENGTH, ABILITY_FORCE_SPREAD_COSINE,
                         ABILITY_FORCE_REACH);
    if (contact.hit) {
        WorldApplyPunch(context->world, contact.position, context->direction,
                        ABILITY_FORCE_CRATER_RADIUS, ABILITY_FORCE_CRACK_COUNT,
                        ABILITY_FORCE_CRACK_LENGTH);
    }
    /* The same cone, the same reach, the same refusal to reach round a corner:
       a detached slab standing in the blow is not a different kind of thing
       from the loose cells beside it. */
    (void)TerrainImpulseQueueBlast(context->impulses, (TerrainBlast){
        .shape = TERRAIN_BLAST_CONE,
        .origin = context->origin,
        .direction = context->direction,
        .radius = ABILITY_FORCE_LENGTH,
        .spreadCosine = ABILITY_FORCE_SPREAD_COSINE,
        .momentum = ABILITY_FORCE_BODY_IMPULSE,
        .carveRadius = ABILITY_FORCE_BODY_CARVE,
    });
    ParticlesSpawnForceBlast(context->particles, context->origin,
                             context->direction);
    if (contact.hit) {
        /* Rubble thrown back out of the crater. The blow has to leave something
           in the air, or all the player sees of a hit that heavy is cells
           quietly ceasing to exist. */
        Vector2 back = {-context->direction.x, -context->direction.y};

        ParticlesSpawnImpact(context->particles, contact.position, back,
                             contact.material);
        ParticlesSpawnDrillDebris(context->particles, contact.position,
                                  context->direction, contact.material);
    }
    state->origin = context->origin;
    state->direction = context->direction;
    state->endpoint = (Vector2){
        context->origin.x + context->direction.x * ABILITY_FORCE_LENGTH,
        context->origin.y + context->direction.y * ABILITY_FORCE_LENGTH};
    (void)GameEventsPush(context->events, (GameEvent){
        .type = GAME_EVENT_FORCE,
        /* Reported where it landed, not where it was thrown from, so the camera
           and the effects answer to the impact rather than to the fist. */
        .position = contact.hit ? contact.position : context->origin,
        .direction = context->direction,
        .strength = ABILITY_FORCE_IMPACT_STRENGTH,
        .radius = ABILITY_FORCE_LENGTH,
        .material = contact.material,
        /* Deliberately none: see ABILITY_FORCE_RECOIL. */
        .playerImpulse = {0.0f, 0.0f},
    });
}

static void AbilityApplyExplosion(const AbilityContext *context,
                                  AbilityState *state)
{
    int centerX = (int)context->aim.x;
    int centerY = (int)context->aim.y;

    WorldDestroyCircle(context->world, centerX, centerY,
                       ABILITY_EXPLOSION_CORE_RADIUS, 0.38f);
    WorldApplyShockwave(context->world, centerX, centerY,
                        ABILITY_EXPLOSION_CORE_RADIUS,
                        (int)ABILITY_EXPLOSION_SHOCK_RADIUS);
    /* Queued rather than applied: the piece this blast is about to cut free
       does not exist yet. The fixed step runs the connectivity check first and
       then delivers this, so the slab the explosion just severed is thrown by
       the very explosion that severed it. Reach matches the shockwave's, so a
       body is pushed exactly as far out as loose sand would be. */
    (void)TerrainImpulseQueueBlast(context->impulses, (TerrainBlast){
        .shape = TERRAIN_BLAST_RADIAL,
        .origin = context->aim,
        .radius = ABILITY_EXPLOSION_SHOCK_RADIUS,
        .momentum = ABILITY_EXPLOSION_BODY_IMPULSE,
        .carveRadius = ABILITY_EXPLOSION_BODY_CARVE,
    });
    ParticlesSpawnExplosion(context->particles, context->aim);
    state->origin = context->aim;
    state->endpoint = context->aim;
    state->direction = context->direction;
    (void)GameEventsPush(context->events, (GameEvent){
        .type = GAME_EVENT_EXPLOSION,
        .position = context->aim,
        .radius = ABILITY_EXPLOSION_SHOCK_RADIUS,
        .strength = ABILITY_EXPLOSION_KNOCKBACK,
        .playerImpulse = RadialImpulse(context->origin, context->aim,
                                       ABILITY_EXPLOSION_SHOCK_RADIUS,
                                       ABILITY_EXPLOSION_KNOCKBACK),
    });
}

static const AbilityDefinition ABILITIES[ABILITY_COUNT] = {
    [ABILITY_LASER] = {
        .name = "LASER",
        .trigger = ABILITY_TRIGGER_HELD,
        .pose = PLAYER_POSE_LASER,
        .poseHold = 0.06f,
        .apply = AbilityApplyLaser,
    },
    [ABILITY_EXPLOSION] = {
        .name = "EXPLOSION",
        .trigger = ABILITY_TRIGGER_PRESSED,
        .cooldown = 0.7f,
        .effectTime = 0.32f,
        .apply = AbilityApplyExplosion,
    },
    [ABILITY_FORCE] = {
        .name = "FORCE",
        .trigger = ABILITY_TRIGGER_PRESSED,
        .cooldown = 0.42f,
        .effectTime = 0.26f,
        .pose = PLAYER_POSE_BLAST,
        .poseHold = 0.28f,
        .apply = AbilityApplyForce,
    },
    [ABILITY_CRYO] = {
        .name = "CRYO",
        .trigger = ABILITY_TRIGGER_HELD,
        .pose = PLAYER_POSE_CHILL,
        .poseHold = 0.06f,
        .apply = AbilityApplyCryo,
    },
};

const AbilityDefinition *AbilityDefinitionAt(AbilityId id)
{
    if (id < 0 || id >= ABILITY_COUNT) {
        return &ABILITIES[ABILITY_LASER];
    }
    return &ABILITIES[id];
}

const AbilityState *AbilityStateAt(const AbilitySystem *abilities, AbilityId id)
{
    static const AbilityState empty = {0};

    if (abilities == NULL || id < 0 || id >= ABILITY_COUNT) {
        return &empty;
    }
    return &abilities->states[id];
}

const char *AbilitiesCurrentName(const AbilitySystem *abilities)
{
    if (abilities == NULL) {
        return "UNKNOWN";
    }
    return AbilityDefinitionAt(abilities->lastUsed)->name;
}

bool AbilitiesValidate(void)
{
    int id;

    for (id = 0; id < ABILITY_COUNT; ++id) {
        const AbilityDefinition *definition = &ABILITIES[id];

        if (definition->name == NULL || definition->name[0] == '\0' ||
            definition->apply == NULL) {
            return false;
        }
        /* A one-shot power with no cooldown fires on every frame the button is
           held down, which is the held behaviour it was not asked for. */
        if (definition->trigger == ABILITY_TRIGGER_PRESSED &&
            definition->cooldown <= 0.0f) {
            return false;
        }
    }
    return true;
}

void AbilitiesInit(AbilitySystem *abilities, uint64_t seed)
{
    int id;

    if (abilities == NULL) {
        return;
    }
    *abilities = (AbilitySystem){0};
    RngSeed(&abilities->rng, seed);
    abilities->lastUsed = ABILITY_LASER;
    for (id = 0; id < ABILITY_COUNT; ++id) {
        abilities->states[id].direction = (Vector2){1.0f, 0.0f};
    }
}

void AbilitiesUpdate(AbilitySystem *abilities, World *world,
                     DynamicTerrainSystem *terrain, TerrainDamageSystem *damage,
                     TerrainImpulseSystem *impulses, ParticleSystem *particles,
                     GameEventBuffer *events, Vector2 origin, Vector2 aim,
                     float deltaTime, const bool *requested)
{
    AbilityContext context;
    Vector2 direction = {aim.x - origin.x, aim.y - origin.y};
    float length = sqrtf(direction.x * direction.x + direction.y * direction.y);
    int id;

    if (abilities == NULL || world == NULL || requested == NULL) {
        return;
    }

    if (length > 0.001f) {
        direction.x /= length;
        direction.y /= length;
    } else {
        /* Aiming exactly at your own feet still has to point somewhere. */
        direction = (Vector2){1.0f, 0.0f};
    }

    context.world = world;
    context.terrain = terrain;
    context.damage = damage;
    context.impulses = impulses;
    context.particles = particles;
    context.events = events;
    context.rng = &abilities->rng;
    context.origin = origin;
    context.aim = aim;
    context.direction = direction;
    context.deltaTime = deltaTime;

    for (id = 0; id < ABILITY_COUNT; ++id) {
        const AbilityDefinition *definition = &ABILITIES[id];
        AbilityState *state = &abilities->states[id];
        bool wanted = requested[id];
        bool wasActive = state->active;

        state->active = false;
        state->triggered = false;
        state->hit = false;
        state->cooldown = fmaxf(0.0f, state->cooldown - deltaTime);
        state->effectTime = fmaxf(0.0f, state->effectTime - deltaTime);

        if (!wanted) {
            continue;
        }
        /* Reaching for a power on cooldown still selects it, so the HUD shows
           what the player is trying to use rather than the last thing that
           happened to succeed. */
        abilities->lastUsed = (AbilityId)id;
        if (state->cooldown > 0.0f) {
            continue;
        }

        state->active = true;
        /* A rising edge, so a held beam reports one start rather than one per
           frame. For a PRESSED power every activation is already an edge. */
        state->triggered = !wasActive;
        state->cooldown = definition->cooldown;
        state->effectTime = definition->effectTime;
        definition->apply(&context, state);
    }
}
