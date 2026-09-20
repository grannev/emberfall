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
 * Membership is the structural solids: WorldMaterialIsSolid, the notion of
 * solid that stops the player, a beam and a force blast, less the dynamic
 * ones — sand, ash, rubble — which fall on their own and hold nothing up. A
 * grain is never a link: a slab cut free that happened to touch a pile of
 * sand, or the rubble of the cave-in that freed it, read as attached to the
 * ground through the pile, and the pile was what should have been ignored.
 * The one thing a grain does is ride along: a grain resting directly on a
 * member cell is a member too, but the search never continues through it,
 * so a pile beside the piece stays in the world and a dusting on top of it
 * goes with it.
 *
 * A structural cell joined to the component only at a corner is absorbed
 * when it has no other structural neighbour on any side: with the component
 * gone it would hang in the air by nothing, and one pixel of leaf left
 * behind where a tree was torn out is the kind of thing a player notices.
 * The rule is one pass and never chains — an orphan's own corner neighbours
 * are not asked — so the cost is a bounded number of reads per member cell.
 */
#include "world_components.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "materials.h"
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

/* Solid and not loose: what the component is made of and searched through. */
static inline bool ComponentIsStructural(CellMaterial material)
{
    return WorldMaterialIsSolid(material) && !MaterialIsDynamic(material);
}

/* Adds a cell to the component when it is unvisited, inside the region and
   within the cell budget. Returns false only on the budget. */
static bool ComponentAdd(WorldComponentWorkspace *workspace,
                         WorldComponentResult *result, int x, int y,
                         int firstX, int firstY, int regionWidth,
                         int maximumCells)
{
    int localIndex = ComponentLocalIndex(x - firstX, y - firstY, regionWidth);

    if (ComponentVisited(workspace, localIndex)) {
        return true;
    }
    if (result->cellCount >= maximumCells) {
        return false;
    }
    ComponentMarkVisited(workspace, localIndex);
    workspace->cellX[result->cellCount] = (int32_t)x;
    workspace->cellY[result->cellCount] = (int32_t)y;
    ++result->cellCount;
    if (x < result->minimumX) result->minimumX = x;
    if (x > result->maximumX) result->maximumX = x;
    if (y < result->minimumY) result->minimumY = y;
    if (y > result->maximumY) result->maximumY = y;
    return true;
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
    if (!ComponentIsStructural(WorldMaterialAt(world, seedX, seedY))) {
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

        /* A grain that rides along is a leaf: nothing is reached through it. */
        if (MaterialIsDynamic(WorldMaterialAt(world, x, y))) {
            continue;
        }
        for (i = 0; i < 4; ++i) {
            int neighbourX = x + offsets[i][0];
            int neighbourY = y + offsets[i][1];
            CellMaterial material;

            /* Outside the world. The simulation already treats everything past
               the edge as immovable rock — WorldMaterialAt returns ROCK there,
               which is what makes the map's border an unbreakable wall — so a
               component touching it is attached, and proven so. */
            if (!WorldInBounds(world, neighbourX, neighbourY)) {
                return ComponentFailure(WORLD_COMPONENT_ANCHORED, result.cellCount);
            }

            material = WorldMaterialAt(world, neighbourX, neighbourY);
            if (!WorldMaterialIsSolid(material)) {
                continue;
            }
            /* Loose material is not a link, and only the grains resting on
               the component ride with it. */
            if (MaterialIsDynamic(material) && i != 0) {
                continue;
            }

            /* Solid, and outside the region. The component genuinely continues
               somewhere this query is not allowed to look, so the honest answer
               is that we do not know. Note that reaching the region edge is not
               by itself a reason to give up: the peek above already established
               whether anything solid is actually there. */
            if (neighbourX < firstX || neighbourX > lastX ||
                neighbourY < firstY || neighbourY > lastY) {
                if (MaterialIsDynamic(material)) {
                    continue;
                }
                return ComponentFailure(WORLD_COMPONENT_UNKNOWN, result.cellCount);
            }

            if (!ComponentAdd(workspace, &result, neighbourX, neighbourY, firstX,
                              firstY, regionWidth, maximumCells)) {
                return ComponentFailure(WORLD_COMPONENT_TOO_LARGE, result.cellCount);
            }
        }
    }

    /* Orphans: structural cells touching the component at a corner only,
       with no structural cell on any side. The member list is walked as it
       was before this pass, so an orphan's own corners are never asked. */
    {
        static const int corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        static const int sides[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        int members = result.cellCount;

        for (head = 0; head < members; ++head) {
            int x = (int)workspace->cellX[head];
            int y = (int)workspace->cellY[head];
            int i;

            if (MaterialIsDynamic(WorldMaterialAt(world, x, y))) {
                continue;
            }
            for (i = 0; i < 4; ++i) {
                int cornerX = x + corners[i][0];
                int cornerY = y + corners[i][1];
                int held = 0;
                int j;

                if (cornerX < firstX || cornerX > lastX || cornerY < firstY ||
                    cornerY > lastY ||
                    !ComponentIsStructural(WorldMaterialAt(world, cornerX, cornerY)) ||
                    ComponentVisited(workspace,
                                     ComponentLocalIndex(cornerX - firstX,
                                                         cornerY - firstY,
                                                         regionWidth))) {
                    continue;
                }
                for (j = 0; j < 4; ++j) {
                    int sideX = cornerX + sides[j][0];
                    int sideY = cornerY + sides[j][1];

                    if (!WorldInBounds(world, sideX, sideY) ||
                        (ComponentIsStructural(WorldMaterialAt(world, sideX, sideY)) &&
                         (sideX < firstX || sideX > lastX || sideY < firstY ||
                          sideY > lastY ||
                          !ComponentVisited(workspace,
                                            ComponentLocalIndex(sideX - firstX,
                                                                sideY - firstY,
                                                                regionWidth))))) {
                        ++held;
                        break;
                    }
                }
                if (held > 0) {
                    continue;
                }
                if (!ComponentAdd(workspace, &result, cornerX, cornerY, firstX,
                                  firstY, regionWidth, maximumCells)) {
                    return ComponentFailure(WORLD_COMPONENT_TOO_LARGE,
                                            result.cellCount);
                }
            }
        }
    }

    result.status = WORLD_COMPONENT_DETACHED;
    result.exploredCells = result.cellCount;
    return result;
}
