/* The back layer coming away where nothing holds it.
 *
 * The back layer is made with the world (world_biomes.c, world_structures.c)
 * and drawn wherever a cell is empty. It stands because something stands in
 * front of it: the rock round a cave, the walls round a room. Blow the room
 * away and its back wall would be left hanging in the sky, a picture of a
 * wall with nothing to be the wall of. So after every destruction the layer
 * near it is asked, block by block, whether it still touches a static solid
 * cell anywhere; a part that does not comes away in pieces, which
 * presentation lets fall and fade.
 *
 * The question is asked the way the terrain's detach check asks its own:
 * only where the destruction log says something was cut, inside a window of
 * fixed size, and a part that reaches the edge of the window is taken to be
 * held. A tunnel dug through the ground finds the rock beside it in a block
 * or two and stops there, which is almost always.
 */
#include "world_internal.h"

#include <math.h>
#include <string.h>

#include "materials.h"

/* Whether block (blockX, blockY) has a static solid cell in front of any of
   its cells — the thing that holds a wall up. Loose grains and plants do
   not. */
static bool BackWallBlockHeld(const World *world, int blockX, int blockY)
{
    int y;

    for (y = blockY * WORLD_BACK_WALL_SCALE; y < (blockY + 1) * WORLD_BACK_WALL_SCALE; ++y) {
        int x;

        if (y >= world->height) break;
        for (x = blockX * WORLD_BACK_WALL_SCALE; x < (blockX + 1) * WORLD_BACK_WALL_SCALE;
             ++x) {
            CellMaterial material = WorldMaterialAt(world, x, y);

            if (MaterialIsSolid(material) && !MaterialIsDynamic(material) &&
                !MaterialIsFlora(material)) {
                return true;
            }
        }
    }
    return false;
}

static uint8_t *BackWallSlot(World *world, int blockX, int blockY)
{
    int column = blockX % world->backWallColumns;

    if (column < 0) column += world->backWallColumns;
    return &world->backWalls[(size_t)blockY * (size_t)world->backWallColumns +
                             (size_t)column];
}

/* Marks the page chunks over a block for rebuilding. */
static void BackWallDirty(World *world, int blockX, int blockY)
{
    int x = WorldWrapColumn(blockX * WORLD_BACK_WALL_SCALE, world->width);
    int y = blockY * WORLD_BACK_WALL_SCALE;
    int chunkX = x / WORLD_CHUNK_SIZE;
    int chunkY = y / WORLD_CHUNK_SIZE;

    if (chunkY >= world->chunkRows) return;
    world->dirtyChunks[(size_t)chunkY * (size_t)world->chunkColumns + (size_t)chunkX] = 1u;
    /* The light tells air in the ground from air under the sky by the wall
       behind it. */
    world->lightDirtyChunks[(size_t)chunkY * (size_t)world->chunkColumns + (size_t)chunkX] = 1u;
}

int WorldBreakBackWalls(World *world, int minimumX, int minimumY, int maximumX,
                        int maximumY, WorldBackWallPiece *pieces, int capacity)
{
    const int window = WORLD_BACK_WALL_WINDOW;
    int written = 0;
    int originX;
    int originY;
    int seedY;

    if (world == NULL || world->backWalls == NULL || world->backWallVisit == NULL ||
        maximumX < minimumX || maximumY < minimumY) {
        return 0;
    }
    /* The window, centred on the cut, in blocks; unwrapped across, the rows
       clamped to the world. */
    originX = ((minimumX + maximumX) / 2) / WORLD_BACK_WALL_SCALE - window / 2;
    originY = ((minimumY + maximumY) / 2) / WORLD_BACK_WALL_SCALE - window / 2;
    if (originY < 0) originY = 0;
    if (originY + window > world->backWallRows) originY = world->backWallRows - window;
    if (originY < 0) originY = 0;
    memset(world->backWallVisit, 0, (size_t)window * (size_t)window);

    for (seedY = minimumY / WORLD_BACK_WALL_SCALE - 1;
         seedY <= maximumY / WORLD_BACK_WALL_SCALE + 1; ++seedY) {
        int seedX;

        if (seedY - originY < 0 || seedY - originY >= window || seedY >= world->backWallRows) {
            continue;
        }
        for (seedX = minimumX / WORLD_BACK_WALL_SCALE - 1;
             seedX <= maximumX / WORLD_BACK_WALL_SCALE + 1; ++seedX) {
            int head = 0;
            int tail = 0;
            bool held = false;
            int index;

            if (seedX - originX < 0 || seedX - originX >= window) continue;
            if (world->backWallVisit[(seedY - originY) * window + (seedX - originX)] != 0u ||
                *BackWallSlot(world, seedX, seedY) == 0u) {
                continue;
            }
            /* One part of the layer, breadth first, stopping the moment
               anything holds it. */
            world->backWallVisit[(seedY - originY) * window + (seedX - originX)] = 1u;
            world->backWallQueue[tail++] = (seedY - originY) * window + (seedX - originX);
            while (head < tail) {
                int local = world->backWallQueue[head++];
                int localX = local % window;
                int localY = local / window;
                static const int stepX[4] = {1, -1, 0, 0};
                static const int stepY[4] = {0, 0, 1, -1};
                int direction;

                if (localX == 0 || localY == 0 || localX == window - 1 ||
                    localY == window - 1 ||
                    BackWallBlockHeld(world, originX + localX, originY + localY)) {
                    held = true;
                    break;
                }
                for (direction = 0; direction < 4; ++direction) {
                    int nextX = localX + stepX[direction];
                    int nextY = localY + stepY[direction];
                    int next = nextY * window + nextX;

                    if (originY + nextY >= world->backWallRows ||
                        world->backWallVisit[next] != 0u ||
                        *BackWallSlot(world, originX + nextX, originY + nextY) == 0u) {
                        continue;
                    }
                    world->backWallVisit[next] = 1u;
                    world->backWallQueue[tail++] = next;
                }
            }
            if (held) {
                /* Whatever was queued and not reached stays marked: it is
                   part of the same held wall, and asking again from it
                   would only find the same answer. */
                continue;
            }
            /* It comes away: out of the layer, into pieces. */
            for (index = 0; index < tail; ++index) {
                int localX = world->backWallQueue[index] % window;
                int localY = world->backWallQueue[index] / window;
                int blockX = originX + localX;
                int blockY = originY + localY;
                uint8_t *slot = BackWallSlot(world, blockX, blockY);
                int pieceX = (int)floorf((float)blockX / (float)WORLD_BACK_WALL_PIECE) *
                             WORLD_BACK_WALL_PIECE;
                int pieceY = blockY / WORLD_BACK_WALL_PIECE * WORLD_BACK_WALL_PIECE;
                uint16_t bit = (uint16_t)(1u << ((blockY - pieceY) * WORLD_BACK_WALL_PIECE +
                                                 (blockX - pieceX)));
                int piece;

                for (piece = 0; piece < written; ++piece) {
                    if (pieces[piece].x == pieceX * WORLD_BACK_WALL_SCALE &&
                        pieces[piece].y == pieceY * WORLD_BACK_WALL_SCALE) {
                        pieces[piece].mask |= bit;
                        break;
                    }
                }
                if (piece == written && written < capacity) {
                    pieces[written++] = (WorldBackWallPiece){
                        .x = pieceX * WORLD_BACK_WALL_SCALE,
                        .y = pieceY * WORLD_BACK_WALL_SCALE,
                        .mask = bit,
                        .material = *slot,
                    };
                }
                *slot = 0u;
                BackWallDirty(world, blockX, blockY);
            }
        }
    }
    return written;
}
