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
    GAME_EVENT_BOOST_STAGE,
    GAME_EVENT_FORCE,
    GAME_EVENT_EXPLOSION,
    GAME_EVENT_LASER_HIT,
    GAME_EVENT_CRYO_HIT,
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
