#include "backwall_debris.h"

#include <math.h>
#include <string.h>

#include "material_render.h"
#include "world.h"

/* Cells per side of one piece, and pieces across the atlas. */
#define DEBRIS_CELLS (WORLD_BACK_WALL_SCALE * WORLD_BACK_WALL_PIECE)
#define DEBRIS_ATLAS_COLUMNS 16

bool BackWallDebrisInit(BackWallDebris *debris)
{
    Image image;

    if (debris == NULL) {
        return false;
    }
    memset(debris, 0, sizeof(*debris));
    image = GenImageColor(DEBRIS_CELLS * DEBRIS_ATLAS_COLUMNS,
                          DEBRIS_CELLS * (BACKWALL_DEBRIS_CAPACITY / DEBRIS_ATLAS_COLUMNS),
                          BLANK);
    debris->atlas = LoadTextureFromImage(image);
    UnloadImage(image);
    if (debris->atlas.id != 0u) {
        SetTextureFilter(debris->atlas, TEXTURE_FILTER_POINT);
    }
    debris->ready = debris->atlas.id != 0u;
    return debris->ready;
}

void BackWallDebrisUnload(BackWallDebris *debris)
{
    if (debris == NULL) {
        return;
    }
    if (debris->atlas.id != 0u) {
        UnloadTexture(debris->atlas);
    }
    memset(debris, 0, sizeof(*debris));
}

void BackWallDebrisClear(BackWallDebris *debris)
{
    int index;

    if (debris == NULL) {
        return;
    }
    for (index = 0; index < BACKWALL_DEBRIS_CAPACITY; ++index) {
        debris->pieces[index].active = false;
    }
}

void BackWallDebrisShift(BackWallDebris *debris, float dx)
{
    int index;

    if (debris == NULL) {
        return;
    }
    for (index = 0; index < BACKWALL_DEBRIS_CAPACITY; ++index) {
        debris->pieces[index].position.x += dx;
    }
}

static Rectangle DebrisSlot(int index)
{
    return (Rectangle){(float)((index % DEBRIS_ATLAS_COLUMNS) * DEBRIS_CELLS),
                       (float)((index / DEBRIS_ATLAS_COLUMNS) * DEBRIS_CELLS),
                       (float)DEBRIS_CELLS, (float)DEBRIS_CELLS};
}

/* A free slot, or the one closest to its end. */
static int DebrisTakeSlot(const BackWallDebris *debris)
{
    int best = 0;
    float bestLeft = 1e9f;
    int index;

    for (index = 0; index < BACKWALL_DEBRIS_CAPACITY; ++index) {
        const BackWallDebrisPiece *piece = &debris->pieces[index];
        float left;

        if (!piece->active) {
            return index;
        }
        left = piece->life - piece->age;
        if (left < bestLeft) {
            bestLeft = left;
            best = index;
        }
    }
    return best;
}

static float DebrisUnit(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)(value & 0xffffu) / 65535.0f;
}

void BackWallDebrisConsumeEvents(BackWallDebris *debris, const GameEventBuffer *events)
{
    uint16_t event;

    if (debris == NULL || !debris->ready || events == NULL) {
        return;
    }
    for (event = 0u; event < events->count; ++event) {
        const GameEvent *fall = &events->events[event];
        Color pixels[DEBRIS_CELLS * DEBRIS_CELLS];
        uint32_t salt;
        int slot;
        int y;

        if (fall->type != GAME_EVENT_BACK_WALL_FALL || fall->count == 0) {
            continue;
        }
        slot = DebrisTakeSlot(debris);
        for (y = 0; y < DEBRIS_CELLS; ++y) {
            int x;

            for (x = 0; x < DEBRIS_CELLS; ++x) {
                int bit = (y / WORLD_BACK_WALL_SCALE) * WORLD_BACK_WALL_PIECE +
                          x / WORLD_BACK_WALL_SCALE;

                pixels[y * DEBRIS_CELLS + x] =
                    ((unsigned int)fall->count >> bit) & 1u
                        ? MaterialRenderBackWall(fall->material,
                                                 (int)fall->position.x + x,
                                                 (int)fall->position.y + y)
                              .scene
                        : BLANK;
            }
        }
        UpdateTextureRec(debris->atlas, DebrisSlot(slot), pixels);
        salt = ++debris->serial * 2654435761u;
        debris->pieces[slot] = (BackWallDebrisPiece){
            .position = {fall->position.x + (float)DEBRIS_CELLS * 0.5f,
                         fall->position.y + (float)DEBRIS_CELLS * 0.5f},
            .velocity = {(DebrisUnit(salt) - 0.5f) * 24.0f, DebrisUnit(salt + 1u) * 10.0f},
            .angle = 0.0f,
            .spin = (DebrisUnit(salt + 2u) - 0.5f) * 70.0f,
            .age = 0.0f,
            .life = 2.4f + DebrisUnit(salt + 3u) * 1.6f,
            .active = true,
        };
    }
}

void BackWallDebrisUpdate(BackWallDebris *debris, float deltaTime)
{
    int index;

    if (debris == NULL || deltaTime <= 0.0f) {
        return;
    }
    for (index = 0; index < BACKWALL_DEBRIS_CAPACITY; ++index) {
        BackWallDebrisPiece *piece = &debris->pieces[index];

        if (!piece->active) continue;
        piece->age += deltaTime;
        if (piece->age >= piece->life) {
            piece->active = false;
            continue;
        }
        piece->velocity.y += 260.0f * deltaTime;
        piece->position.x += piece->velocity.x * deltaTime;
        piece->position.y += piece->velocity.y * deltaTime;
        piece->angle += piece->spin * deltaTime;
    }
}

void BackWallDebrisDraw(const BackWallDebris *debris)
{
    int index;

    if (debris == NULL || !debris->ready) {
        return;
    }
    for (index = 0; index < BACKWALL_DEBRIS_CAPACITY; ++index) {
        const BackWallDebrisPiece *piece = &debris->pieces[index];
        float fade;
        /* Shrinking a little as it falls away from the wall it was part
           of: it is going back, not only down. */
        float size;

        if (!piece->active) continue;
        fade = 1.0f - piece->age / piece->life;
        size = (float)DEBRIS_CELLS * (0.7f + 0.3f * fade);
        DrawTexturePro(debris->atlas, DebrisSlot(index),
                       (Rectangle){piece->position.x, piece->position.y, size, size},
                       (Vector2){size * 0.5f, size * 0.5f}, piece->angle,
                       (Color){255, 255, 255, (unsigned char)(255.0f * fade * fade)});
    }
}
