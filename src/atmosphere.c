/* Re-entry. See atmosphere.h for the model; this file records the decisions
 * inside it.
 */
#include "atmosphere.h"

#include <math.h>
#include <stddef.h>

#include "materials.h"

AtmosphereConfig AtmosphereDefaultConfig(void)
{
    AtmosphereConfig config;

    /* Well above the character's cruise (118): flying without boost is never
       re-entry, whatever the altitude — no heat, no drag, no sparks. Only a
       boosted dive or a slab falling out of orbit is going fast enough, and
       both pass 300. */
    config.entrySpeed = 200.0f;
    /* Heat per second per unit of speed over the entry speed, in units of
       the entry speed: a full boost (380, 0.9 over) in the thick of the
       corridor is ablaze in a fifth of a second; just over the threshold it
       barely warms, so nothing flickers at the edge. Out in a second and a
       half. */
    config.heatRate = 2.8f;
    config.coolRate = 0.7f;
    config.glowOn = 0.30f;
    config.glowOff = 0.10f;
    config.eventInterval = 0.05f;
    /* Enough that a slab dropped from orbit arrives at a speed a fall from
       the clouds would give, not at the speed the vacuum let it keep. */
    config.bodyDrag = 0.35f;
    config.bodyHeatStrength = 0.9f;
    config.bodyHeatInterval = 0.08f;
    config.scorchRadius = 3.0f;
    config.scorchStrength = 0.85f;
    return config;
}

void AtmosphereInit(AtmosphereSystem *system)
{
    AtmosphereStats empty = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    int slot;

    if (system == NULL) {
        return;
    }
    system->config = AtmosphereDefaultConfig();
    system->stats = empty;
    system->playerHeat = 0.0f;
    system->playerBurning = false;
    system->playerEventCooldown = 0.0f;
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        system->bodyHeat[slot] = 0.0f;
        system->bodyGeneration[slot] = 0u;
        system->bodyHeatCooldown[slot] = 0.0f;
        system->bodyEventCooldown[slot] = 0.0f;
        system->bodyBurning[slot] = false;
    }
}

float AtmosphereCorridorAt(const World *world, float y)
{
    float density = WorldGravityScaleAt(world, y);

    /* Peaks where the air is half there: below, the air is thick but the
       band is short and a body has already been slowed; above, there is
       nothing to burn in. */
    return 4.0f * density * (1.0f - density);
}

/* Heat after one step at `speed` through `corridor`. */
static float AtmosphereExcess(const AtmosphereConfig *config, float speed)
{
    float excess = (speed - config->entrySpeed) / config->entrySpeed;

    return excess > 0.0f ? excess : 0.0f;
}

static float AtmosphereStepHeat(const AtmosphereConfig *config, float heat,
                                float corridor, float speed, float deltaTime)
{
    float excess = AtmosphereExcess(config, speed);

    if (corridor > 0.0f && excess > 0.0f) {
        heat += config->heatRate * corridor * excess * deltaTime;
    } else {
        heat -= config->coolRate * deltaTime;
    }
    if (heat < 0.0f) heat = 0.0f;
    if (heat > 1.0f) heat = 1.0f;
    return heat;
}

/* Whether something is burning after this step, with hysteresis. */
static bool AtmosphereBurning(const AtmosphereConfig *config, bool was, float heat)
{
    if (was) {
        return heat > config->glowOff;
    }
    return heat >= config->glowOn;
}

static void AtmosphereScorch(AtmosphereSystem *system, const Player *player,
                             World *world)
{
    int centreX = (int)floorf(player->position.x);
    int centreY = (int)floorf(player->position.y);
    int extent = (int)ceilf(player->radius + system->config.scorchRadius);
    float reach = player->radius + system->config.scorchRadius;
    int y;

    for (y = centreY - extent; y <= centreY + extent; ++y) {
        int x;

        for (x = centreX - extent; x <= centreX + extent; ++x) {
            CellMaterial material = WorldGetCell(world, x, y);
            const MaterialInfo *info;
            float dx = (float)x + 0.5f - player->position.x;
            float dy = (float)y + 0.5f - player->position.y;
            float distance = sqrtf(dx * dx + dy * dy);
            float band;
            float target;

            if (!MaterialIsSolid(material) || distance > reach) {
                continue;
            }
            info = MaterialAt(material);
            if (!info->onHeat.enabled || info->onHeat.threshold <= 60.0f) {
                continue;
            }
            band = 1.0f - (distance - player->radius) / system->config.scorchRadius;
            if (band > 1.0f) band = 1.0f;
            if (band <= 0.0f) continue;
            target = info->onHeat.threshold * system->config.scorchStrength *
                     system->playerHeat * band;
            if (target > WorldGetTemperature(world, x, y)) {
                WorldSetTemperature(world, x, y, target);
                ++system->stats.cellsScorched;
            }
        }
    }
}

void AtmosphereUpdatePlayer(AtmosphereSystem *system, const Player *player,
                            World *world, GameEventBuffer *events,
                            float deltaTime)
{
    float speed;
    float corridor;
    bool burning;

    if (system == NULL || player == NULL || world == NULL || world->cells == NULL) {
        return;
    }
    speed = sqrtf(player->velocity.x * player->velocity.x +
                  player->velocity.y * player->velocity.y);
    corridor = AtmosphereCorridorAt(world, player->position.y);
    system->playerHeat = AtmosphereStepHeat(&system->config, system->playerHeat,
                                            corridor, speed, deltaTime);
    system->playerEventCooldown -= deltaTime;
    burning = AtmosphereBurning(&system->config, system->playerBurning,
                                system->playerHeat);
    if (burning && !system->playerBurning) {
        ++system->stats.playerEntries;
    } else if (!burning && system->playerBurning) {
        ++system->stats.playerExits;
    }
    system->playerBurning = burning;
    if (!burning) {
        return;
    }
    /* What the character brushes against while ablaze is scorched: a
       landing out of orbit leaves a glowing print. Never the character
       themselves — heat is theirs to carry, not to suffer. */
    AtmosphereScorch(system, player, world);
    if (system->playerEventCooldown <= 0.0f && events != NULL) {
        system->playerEventCooldown = system->config.eventInterval;
        ++system->stats.playerEvents;
        (void)GameEventsPush(events, (GameEvent){
            .type = GAME_EVENT_REENTRY,
            .position = player->position,
            .direction = speed > 0.0f
                             ? (Vector2){player->velocity.x / speed,
                                         player->velocity.y / speed}
                             : (Vector2){0.0f, 1.0f},
            .strength = system->playerHeat,
            .radius = player->radius,
            .count = 0,
        });
    }
}

void AtmosphereUpdateBodies(AtmosphereSystem *system,
                            DynamicTerrainSystem *terrain,
                            TerrainDamageSystem *damage, const World *world,
                            GameEventBuffer *events, float deltaTime)
{
    int slot;

    if (system == NULL || terrain == NULL || world == NULL || world->cells == NULL) {
        return;
    }
    system->stats.bodiesBurning = 0;
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &terrain->bodies[slot];
        TerrainBodyHandle handle;
        float speed;
        float corridor;
        float heat;
        bool burning;

        if (!body->active || body->cellCount <= 0) {
            system->bodyHeat[slot] = 0.0f;
            system->bodyBurning[slot] = false;
            continue;
        }
        if (system->bodyGeneration[slot] != body->generation) {
            /* A different body in this slot: cold, whatever the last one
               was. */
            system->bodyGeneration[slot] = body->generation;
            system->bodyHeat[slot] = 0.0f;
            system->bodyBurning[slot] = false;
            system->bodyHeatCooldown[slot] = 0.0f;
            system->bodyEventCooldown[slot] = 0.0f;
        }
        handle = (TerrainBodyHandle){(uint16_t)slot, body->generation};
        speed = sqrtf(body->velocity.x * body->velocity.x +
                      body->velocity.y * body->velocity.y);
        corridor = body->awake ? AtmosphereCorridorAt(world, body->position.y)
                               : 0.0f;
        /* The air's resistance, on the awake alone and never on the character,
           and only on what the body has over the entry speed: below it there
           is no friction at all, so a slab drifting through the band or
           thrown about at ordinary speeds is not held back by it. */
        if (body->awake && corridor > 0.0f &&
            AtmosphereExcess(&system->config, speed) > 0.0f) {
            float keep = expf(-system->config.bodyDrag * corridor *
                              AtmosphereExcess(&system->config, speed) * 4.0f *
                              deltaTime);

            body->velocity.x *= keep;
            body->velocity.y *= keep;
        }
        heat = AtmosphereStepHeat(&system->config, system->bodyHeat[slot],
                                  corridor, speed, deltaTime);
        system->bodyHeatCooldown[slot] -= deltaTime;
        system->bodyEventCooldown[slot] -= deltaTime;
        burning = AtmosphereBurning(&system->config, system->bodyBurning[slot], heat);
        if (burning && !system->bodyBurning[slot]) {
            ++system->stats.bodyEntries;
        } else if (!burning && system->bodyBurning[slot]) {
            ++system->stats.bodyExits;
        }
        system->bodyBurning[slot] = burning;
        if (burning) {
            ++system->stats.bodiesBurning;
        }
        /* The leading face is driven toward the heat: up while it burns, and
           back down once the heat is gone, until the face is cold and the
           slot forgets it. Bounded: the leading half of one raster, at most
           every `bodyHeatInterval`. */
        if ((heat > 0.0f || system->bodyHeat[slot] > 0.0f) && damage != NULL &&
            system->bodyHeatCooldown[slot] <= 0.0f) {
            Vector2 front = body->position;
            float radius = body->boundingRadius * 0.6f + 1.0f;

            if (speed > 0.0f) {
                front.x += body->velocity.x / speed * body->boundingRadius * 0.55f;
                front.y += body->velocity.y / speed * body->boundingRadius * 0.55f;
            }
            TerrainDamageTemperAround(damage, terrain, handle, front, radius,
                                      heat * system->config.bodyHeatStrength);
            system->bodyHeatCooldown[slot] = system->config.bodyHeatInterval;
            ++system->stats.bodyHeatings;
        }
        system->bodyHeat[slot] = heat;
        if (burning && events != NULL && system->bodyEventCooldown[slot] <= 0.0f) {
            system->bodyEventCooldown[slot] = system->config.eventInterval;
            ++system->stats.bodyEvents;
            (void)GameEventsPush(events, (GameEvent){
                .type = GAME_EVENT_REENTRY,
                .position = body->position,
                .direction = speed > 0.0f
                                 ? (Vector2){body->velocity.x / speed,
                                             body->velocity.y / speed}
                                 : (Vector2){0.0f, 1.0f},
                .strength = heat,
                .radius = body->boundingRadius,
                .count = body->cellCount,
            });
        }
    }
}
