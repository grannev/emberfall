#include "game_events.h"

#include <stddef.h>

void GameEventsClear(GameEventBuffer *buffer)
{
    if (buffer == NULL) {
        return;
    }
    buffer->count = 0u;
    buffer->dropped = 0u;
}

bool GameEventsPush(GameEventBuffer *buffer, GameEvent event)
{
    if (buffer == NULL) {
        return false;
    }
    if (buffer->count >= MAX_GAME_EVENTS) {
        if (buffer->dropped < UINT16_MAX) {
            ++buffer->dropped;
        }
        return false;
    }
    buffer->events[buffer->count++] = event;
    return true;
}
