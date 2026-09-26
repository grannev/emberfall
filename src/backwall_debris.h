#ifndef BACKWALL_DEBRIS_H
#define BACKWALL_DEBRIS_H

/* Pieces of the back layer falling away.
 *
 * When the world takes a part of its back layer away for having nothing left
 * to stand on (WorldBreakBackWalls), it publishes the pieces as
 * GAME_EVENT_BACK_WALL_FALL. This lets each fall: a picture of its blocks,
 * tipping as it goes, dropping out of the view behind the world and fading
 * to nothing over a few seconds. It is the background, so it touches nothing
 * and nothing touches it. Presentation only, a fixed pool: when it is full
 * the oldest piece gives up its slot.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "game_events.h"

#define BACKWALL_DEBRIS_CAPACITY 64

typedef struct BackWallDebrisPiece {
    Vector2 position;
    Vector2 velocity;
    float angle;
    float spin;
    float age;
    float life;
    bool active;
} BackWallDebrisPiece;

typedef struct BackWallDebris {
    /* One 16x16 slot of the atlas per piece, rewritten when a piece takes
       the slot. */
    Texture2D atlas;
    BackWallDebrisPiece pieces[BACKWALL_DEBRIS_CAPACITY];
    uint32_t serial;
    bool ready;
} BackWallDebris;

bool BackWallDebrisInit(BackWallDebris *debris);
void BackWallDebrisUnload(BackWallDebris *debris);
void BackWallDebrisClear(BackWallDebris *debris);
void BackWallDebrisShift(BackWallDebris *debris, float dx);
void BackWallDebrisConsumeEvents(BackWallDebris *debris, const GameEventBuffer *events);
void BackWallDebrisUpdate(BackWallDebris *debris, float deltaTime);
/* In world space, inside the camera, before the world's pages. */
void BackWallDebrisDraw(const BackWallDebris *debris);

#endif
