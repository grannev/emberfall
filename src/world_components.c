/* Bounded detection of connected solid components. See world_components.h for
 * what this is for; this file records how it stays bounded and why the rules
 * are the way they are.
 *
 * Connectivity is four-neighbour. Two solid cells meeting only at a corner are
 * not one component. That is the standard pairing for a solid set (the empty
 * set is then eight-connected, so a diagonal gap you can see through is a gap
 * the detector agrees is a gap), and it matches the physics being modelled: a
 * slab of rock touching a hill at one corner is not attached to it. The
 * trade-off is real and worth stating — a piece joined to the ground by a
 * single-cell diagonal staircase reads as detached — but such a join is one
 * pixel thick and would not hold anything up. A caller that wants a minimum
 * thickness before tearing terrain off should impose it itself.
 *
 * Membership is WorldMaterialIsSolid, the same notion of solid that stops the
 * player, a beam and a force blast. That includes sand, which is loose rather
 * than structural. Including it is the conservative choice: more cells in a
 * component means more chances to reach an anchor or the region edge, and so a
 * greater tendency toward "not detached", which is the direction this detector
 * is allowed to err in.
 */
#include "world_components.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "world_internal.h"

/* Region-local index of a cell, used for both the visited bitmap and nothing
   else; component cells are recorded in world coordinates. */
static inline int ComponentLocalIndex(int localX, int localY, int regionWidth)
{
    return localY * regionWidth + localX;
}

static inline bool ComponentVisited(const WorldComponentWorkspace *workspace,
                                    int index)
{
    return (workspace->visited[index >> 5] & (1u << (index & 31))) != 0u;
}

static inline void ComponentMarkVisited(WorldComponentWorkspace *workspace,
                                        int index)
{
    workspace->visited[index >> 5] |= 1u << (index & 31);
}

static WorldComponentResult ComponentFailure(WorldComponentStatus status,
                                             int exploredCells)
{
    WorldComponentResult result = {0};

    result.status = status;
    result.exploredCells = exploredCells;
    return result;
}

WorldComponentResult WorldFindComponent(const World *world,
                                        WorldComponentWorkspace *workspace,
                                        Rectangle region, int seedX, int seedY,
                                        int maximumCells)
{
    WorldComponentResult result = {0};
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    int regionWidth;
    int regionHeight;
    int head;

    if (world == NULL || world->cells == NULL || workspace == NULL) {
        return ComponentFailure(WORLD_COMPONENT_INVALID, 0);
    }
    if (maximumCells > WORLD_COMPONENT_MAX_CELLS) {
        maximumCells = WORLD_COMPONENT_MAX_CELLS;
    }
    if (maximumCells <= 0) {
        return ComponentFailure(WORLD_COMPONENT_INVALID, 0);
    }

    /* The region is given in cells, like WorldActivateRegion. */
    firstX = (int)floorf(region.x);
    firstY = (int)floorf(region.y);
    lastX = (int)ceilf(region.x + region.width) - 1;
    lastY = (int)ceilf(region.y + region.height) - 1;

    /* Judged on what the caller asked for, before any clipping. Rejecting
       rather than shrinking is the promise this function makes, and testing
       after the clip would quietly break it near the map border: the same
       oversized region would be refused in open ground and accepted at the
       edge, purely because the world happened to trim it. */
    if (lastX - firstX + 1 > WORLD_COMPONENT_MAX_SPAN ||
        lastY - firstY + 1 > WORLD_COMPONENT_MAX_SPAN) {
        return ComponentFailure(WORLD_COMPONENT_INVALID, 0);
    }

    /* Clipping to the world costs no meaning: everything past the edge reads
       as rock, so a search that reached it would stop with ANCHORED anyway. */
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > world->width - 1) lastX = world->width - 1;
    if (lastY > world->height - 1) lastY = world->height - 1;
    if (firstX > lastX || firstY > lastY) {
        return ComponentFailure(WORLD_COMPONENT_INVALID, 0);
    }

    regionWidth = lastX - firstX + 1;
    regionHeight = lastY - firstY + 1;

    if (seedX < firstX || seedX > lastX || seedY < firstY || seedY > lastY) {
        return ComponentFailure(WORLD_COMPONENT_INVALID, 0);
    }
    if (!WorldMaterialIsSolid(WorldMaterialAt(world, seedX, seedY))) {
        return ComponentFailure(WORLD_COMPONENT_INVALID, 0);
    }

    /* Only the prefix this region uses is cleared, so the cost is the region's
       area rather than the workspace's maximum. There is no full-world visited
       array to clear, which is the whole point of indexing region-locally. */
    memset(workspace->visited, 0,
           (size_t)(((regionWidth * regionHeight) + 31) / 32) *
               sizeof(*workspace->visited));

    result.minimumX = seedX;
    result.maximumX = seedX;
    result.minimumY = seedY;
    result.maximumY = seedY;
    workspace->cellX[0] = (int32_t)seedX;
    workspace->cellY[0] = (int32_t)seedY;
    result.cellCount = 1;
    ComponentMarkVisited(workspace,
                         ComponentLocalIndex(seedX - firstX, seedY - firstY,
                                             regionWidth));

    /* Breadth-first over an explicit queue. No recursion: a component of a few
       thousand cells would be a few thousand stack frames deep. */
    for (head = 0; head < result.cellCount; ++head) {
        static const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        int x = (int)workspace->cellX[head];
        int y = (int)workspace->cellY[head];
        int i;

        for (i = 0; i < 4; ++i) {
            int neighbourX = x + offsets[i][0];
            int neighbourY = y + offsets[i][1];
            int localIndex;

            /* Outside the world. The simulation already treats everything past
               the edge as immovable rock — WorldMaterialAt returns ROCK there,
               which is what makes the map's border an unbreakable wall — so a
               component touching it is attached, and proven so. */
            if (!WorldInBounds(world, neighbourX, neighbourY)) {
                return ComponentFailure(WORLD_COMPONENT_ANCHORED, result.cellCount);
            }

            if (!WorldMaterialIsSolid(WorldMaterialAt(world, neighbourX,
                                                      neighbourY))) {
                continue;
            }

            /* Solid, and outside the region. The component genuinely continues
               somewhere this query is not allowed to look, so the honest answer
               is that we do not know. Note that reaching the region edge is not
               by itself a reason to give up: the peek above already established
               whether anything solid is actually there. */
            if (neighbourX < firstX || neighbourX > lastX ||
                neighbourY < firstY || neighbourY > lastY) {
                return ComponentFailure(WORLD_COMPONENT_UNKNOWN, result.cellCount);
            }

            localIndex = ComponentLocalIndex(neighbourX - firstX,
                                             neighbourY - firstY, regionWidth);
            if (ComponentVisited(workspace, localIndex)) {
                continue;
            }
            if (result.cellCount >= maximumCells) {
                return ComponentFailure(WORLD_COMPONENT_TOO_LARGE, result.cellCount);
            }
            ComponentMarkVisited(workspace, localIndex);
            workspace->cellX[result.cellCount] = (int32_t)neighbourX;
            workspace->cellY[result.cellCount] = (int32_t)neighbourY;
            ++result.cellCount;

            if (neighbourX < result.minimumX) result.minimumX = neighbourX;
            if (neighbourX > result.maximumX) result.maximumX = neighbourX;
            if (neighbourY < result.minimumY) result.minimumY = neighbourY;
            if (neighbourY > result.maximumY) result.maximumY = neighbourY;
        }
    }

    result.status = WORLD_COMPONENT_DETACHED;
    result.exploredCells = result.cellCount;
    return result;
}
