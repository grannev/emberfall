#ifndef GAME_EVENTS_H
#define GAME_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

#define MAX_GAME_EVENTS 256

typedef enum GameEventType {
    GAME_EVENT_MATERIAL_REACTION = 0,
    GAME_EVENT_PLAYER_IMPACT,
    GAME_EVENT_PLAYER_DRILL,
    GAME_EVENT_BOOST_ENGAGED,
    /* The first crossing of sonic speed in air, with hysteresis before it can
       fire again. Direction is travel; there is no pressure wave in space. */
    GAME_EVENT_SONIC_BREAK,
    GAME_EVENT_FORCE,
    GAME_EVENT_EXPLOSION,
    GAME_EVENT_LASER_HIT,
    GAME_EVENT_CRYO_HIT,
    /* A piece of terrain came loose and is now a body. `position` is where the
       body starts, `count` is how many cells it took with it. */
    GAME_EVENT_TERRAIN_DETACHED,
    /* Something broke a liquid surface: the player or a body, going in or
       coming out. `position` is on the surface, `direction` the way it was
       travelling, `strength` its speed, `material` the liquid, `count` the
       cells of body that went in (zero for the player). Presentation makes
       the splash from this; the water itself is already moving. */
    GAME_EVENT_LIQUID_SPLASH,
    /* A liquid surface was disturbed without being broken — a fast pass just
       above it, a push arriving from below. `position`, `strength` and
       `material` as above, `radius` how wide. */
    GAME_EVENT_LIQUID_RIPPLE,
    /* Something is burning its way down through the atmosphere. `position`
       is where it is, `direction` the way it is going, `strength` how far
       into the burn it is (0..1), `radius` how big it is, `count` the cells
       of body (zero for the character). Repeated while the burn lasts, at
       most every `eventInterval`. */
    GAME_EVENT_REENTRY,
    /* A piece of the back layer lost its hold and came away. `position` is
       the top-left cell of the piece, `count` a mask of which of its 4x4
       blocks it carries (bit by * 4 + bx), `material` what it is made of.
       The world has already removed it; presentation lets it fall and fade. */
    GAME_EVENT_BACK_WALL_FALL,
    GAME_EVENT_HEAVY_LANDING,
    GAME_EVENT_FOOTSTEP,
    GAME_EVENT_TAKEOFF,
    GAME_EVENT_COUNT
} GameEventType;

/* A deliberately plain payload. Most events use only two or three fields; one
   stable shape keeps producers and presentation consumers explicit without a
   callback bus or heap-owned polymorphic messages. */
typedef struct GameEvent {
    GameEventType type;
    Vector2 position;
    Vector2 direction;
    float strength;
    float radius;
    CellMaterial material;
    int count;
    /* Velocity this event adds to the player, already resolved by whatever
       produced it. Abilities publish their own knockback this way, so the
       player module never has to learn which powers exist and a new one that
       shoves the player needs no change outside its own file. */
    Vector2 playerImpulse;
} GameEvent;

typedef struct GameEventBuffer {
    GameEvent events[MAX_GAME_EVENTS];
    uint16_t count;
    uint16_t dropped;
} GameEventBuffer;

void GameEventsClear(GameEventBuffer *buffer);
bool GameEventsPush(GameEventBuffer *buffer, GameEvent event);

#endif
