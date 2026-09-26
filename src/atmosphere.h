#ifndef ATMOSPHERE_H
#define ATMOSPHERE_H

/* Re-entry.
 *
 * Between the space line and the cloud line the world has an atmosphere that
 * thins to nothing, and anything falling through it fast enough burns: the
 * air is thin where the speed is highest, so the heat is a corridor — nothing
 * in space, nothing in the thick air under the clouds where nothing can be
 * going that fast for long, and a band between where a body dropped from
 * orbit glows. That is what `AtmosphereCorridorAt` returns, from the same two
 * lines that gravity fades between, so a body that starts to fall where the
 * pull starts is also where the air starts.
 *
 * Below the entry speed — above the character's cruise — the air does
 * nothing to anything: flight without boost never burns.
 *
 * The character is never slowed — nothing in the world may slow the
 * character, and the air is no exception — they only heat, and the heat is
 * published as GAME_EVENT_REENTRY for presentation to draw the sheath and
 * shake the frame; on the ground it scorches what they land on. A terrain
 * body is not the character: it is slowed by the air it falls through and
 * its leading face is heated through the same damage path the drill uses, so
 * a slab dropped from space arrives glowing and lights what it lands in. The
 * heat is remembered per slot and cools once the body is out of the
 * corridor, so nothing glows for ever.
 *
 * Bounded like everything on the fixed step: one pass over the live bodies,
 * a heating of at most the leading half of a body every `bodyHeatInterval`,
 * and a scorch of a few cells around the character every tick they burn.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dynamic_terrain.h"
#include "game_events.h"
#include "player.h"
#include "terrain_damage.h"
#include "world.h"

typedef struct AtmosphereConfig {
    /* Speed below which the air does nothing at all — no heat, no drag —
       above the character's cruise, so only boosted flight burns. Heat and
       drag grow with the speed over it. */
    float entrySpeed;
    /* The same for a body. Lower than the character's: a body can fall no
       faster than the terrain's speed ceiling (DynamicTerrainConfig
       .maximumSpeed, 300), and a slab dropped from orbit reaches that — at
       the character's threshold it would never burn at all. */
    float bodyEntrySpeed;
    /* Heat gained per second in the densest part of the corridor per unit of
       speed over the entry speed (in units of it), on a scale where one is
       fully ablaze, and lost per second out of it. */
    float heatRate;
    float coolRate;
    /* Heat at and above which something is burning, and at and below which
       it has stopped: hysteresis, so a body skimming the corridor does not
       flicker. */
    float glowOn;
    float glowOff;
    /* Seconds between two re-entry events for the same thing. */
    float eventInterval;
    /* Drag on a body per second in the densest air, scaled by its speed
       over the entry speed; zero at or below it. Never applied to the
       character. */
    float bodyDrag;
    /* How hot a burning body's leading face is driven, as a fraction of each
       material's phase threshold at full heat, and how often. */
    float bodyHeatStrength;
    float bodyHeatInterval;
    /* How far around the character the ground is scorched while they burn,
       and how hot, as a fraction of the phase threshold at full heat. */
    float scorchRadius;
    float scorchStrength;
} AtmosphereConfig;

typedef struct AtmosphereStats {
    int playerEntries;
    int playerExits;
    int playerEvents;
    int bodyEntries;
    int bodyExits;
    int bodyEvents;
    int bodyHeatings;
    int cellsScorched;
    /* Refreshed by every update. */
    int bodiesBurning;
} AtmosphereStats;

typedef struct AtmosphereSystem {
    AtmosphereConfig config;
    AtmosphereStats stats;
    float playerHeat;
    bool playerBurning;
    float playerEventCooldown;
    /* Per slot, keyed by generation so a body that took over a slot starts
       cold. */
    float bodyHeat[MAX_TERRAIN_BODIES];
    uint16_t bodyGeneration[MAX_TERRAIN_BODIES];
    float bodyHeatCooldown[MAX_TERRAIN_BODIES];
    float bodyEventCooldown[MAX_TERRAIN_BODIES];
    bool bodyBurning[MAX_TERRAIN_BODIES];
} AtmosphereSystem;

AtmosphereConfig AtmosphereDefaultConfig(void);
void AtmosphereInit(AtmosphereSystem *system);

/* How much re-entry air there is at `y`: zero in space and under the clouds,
   one in the middle of the band between them. */
float AtmosphereCorridorAt(const World *world, float y);

/* The character's heat, on the frame, before they move. Reads the player and
   never writes it. */
void AtmosphereUpdatePlayer(AtmosphereSystem *system, const Player *player,
                            World *world, GameEventBuffer *events,
                            float deltaTime);

/* Every live body, on the fixed step, before it is integrated: drag on the
   awake ones in the corridor, heat on the fast ones, cooling on the rest. */
void AtmosphereUpdateBodies(AtmosphereSystem *system,
                            DynamicTerrainSystem *terrain,
                            TerrainDamageSystem *damage, const World *world,
                            GameEventBuffer *events, float deltaTime);

#endif
