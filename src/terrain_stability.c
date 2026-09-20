/* Cave-ins. See terrain_stability.h for the model; this file records the
 * decisions inside it.
 */
#include "terrain_stability.h"

#include <stddef.h>

#include "materials.h"

void TerrainStabilityInit(TerrainStabilitySystem *system)
{
    TerrainStabilityStats empty = {0, 0, 0, 0, 0, 0};

    if (system == NULL) {
        return;
    }
    system->head = 0;
    system->count = 0;
    system->stats = empty;
}

/* A ceiling cell: solid, static, and with nothing under it. A dynamic cell
   is not asked — it falls on its own — and a liquid or gas is not a roof. */
static bool TerrainStabilityIsCeiling(const World *world, int x, int y)
{
    CellMaterial material = WorldGetCell(world, x, y);
    CellMaterial below = WorldGetCell(world, x, y + 1);

    if (!MaterialIsSolid(material) || MaterialIsDynamic(material) ||
        MaterialAt(material)->span <= 0) {
        return false;
    }
    return below == MATERIAL_EMPTY || !MaterialIsSolid(below);
}

static void TerrainStabilityQueue(TerrainStabilitySystem *system, int x, int y,
                                  int delay)
{
    int tail;

    if (system->count >= TERRAIN_STABILITY_QUEUE_CAPACITY) {
        ++system->stats.queueRefusals;
        return;
    }
    tail = (system->head + system->count) % TERRAIN_STABILITY_QUEUE_CAPACITY;
    system->queue[tail].x = x;
    system->queue[tail].y = y;
    system->queue[tail].delay = delay;
    ++system->count;
    ++system->stats.cellsQueued;
}

void TerrainStabilityNoteDestruction(TerrainStabilitySystem *system,
                                     const World *world)
{
    int index;

    if (system == NULL || world == NULL || world->cells == NULL) {
        return;
    }
    for (index = 0; index < world->destructionCount; ++index) {
        const WorldDestructionRegion *region = &world->destruction[index];
        int y;

        ++system->stats.regionsNoted;
        for (y = region->minimumY - TERRAIN_STABILITY_MARGIN;
             y <= region->maximumY + TERRAIN_STABILITY_MARGIN; ++y) {
            int x;

            for (x = region->minimumX - TERRAIN_STABILITY_MARGIN;
                 x <= region->maximumX + TERRAIN_STABILITY_MARGIN; ++x) {
                if (x < 0 || y < 0 || x >= world->width || y >= world->height) {
                    continue;
                }
                if (TerrainStabilityIsCeiling(world, x, y)) {
                    TerrainStabilityQueue(system, x, y, 0);
                }
            }
        }
    }
}

/* A support at the end of a ceiling run: solid ground that is itself held
   up, which is a wall. A hole, a gap, or rubble that has not fallen yet is a
   free end, and a run with a free end is a shelf hanging from one side. */
static bool TerrainStabilityIsAnchor(const World *world, int x, int y)
{
    CellMaterial material = WorldGetCell(world, x, y);

    return MaterialIsSolid(material) && !MaterialIsDynamic(material) &&
           MaterialIsSolid(WorldGetCell(world, x, y + 1));
}

static int TerrainStabilityRun(const World *world, int x, int y, int limit,
                               bool *leftAnchored, bool *rightAnchored)
{
    int left = 0;
    int right = 0;

    /* Along the ceiling either way until it ends. Ceiling of any material
       counts toward the run — a strip of rock in a dirt roof is carried by
       the dirt beside it, not the other way round. */
    while (left < limit && TerrainStabilityIsCeiling(world, x - left - 1, y)) {
        ++left;
    }
    while (right < limit && TerrainStabilityIsCeiling(world, x + right + 1, y)) {
        ++right;
    }
    *leftAnchored = TerrainStabilityIsAnchor(world, x - left - 1, y);
    *rightAnchored = TerrainStabilityIsAnchor(world, x + right + 1, y);
    return left + 1 + right;
}

int TerrainStabilitySpanAt(const World *world, int x, int y, int limit)
{
    bool leftAnchored;
    bool rightAnchored;

    if (world == NULL || world->cells == NULL || !TerrainStabilityIsCeiling(world, x, y)) {
        return 0;
    }
    return TerrainStabilityRun(world, x, y, limit, &leftAnchored, &rightAnchored);
}

/* Whether the ceiling run through (x, y) is longer than its weakest material
   bears. A run held at both ends is a beam and bears its material's span; a
   run held at one end is a shelf and bears half of it — which is what makes
   a hole in the middle of a roof bring the rest of the roof down rather than
   leave two short roofs either side of it; a run held at neither end is
   hanging from the layer above alone and gives way at any length. The run is
   bounded by the strongest span any material has, so the walk is bounded
   whatever the cave looks like. */
static bool TerrainStabilityFails(const World *world, int x, int y)
{
    int weakest = MaterialAt(WorldGetCell(world, x, y))->span;
    int longest = 0;
    int material;
    int span;
    int probe;
    bool leftAnchored;
    bool rightAnchored;

    if (!TerrainStabilityIsCeiling(world, x, y)) {
        return false;
    }
    for (material = 0; material < MATERIAL_COUNT; ++material) {
        if (MATERIALS[material].span > longest) {
            longest = MATERIALS[material].span;
        }
    }
    span = TerrainStabilityRun(world, x, y, longest + 1, &leftAnchored,
                               &rightAnchored);
    for (probe = 1; probe < span && TerrainStabilityIsCeiling(world, x - probe, y); ++probe) {
        int candidate = MaterialAt(WorldGetCell(world, x - probe, y))->span;

        if (candidate < weakest) weakest = candidate;
    }
    for (probe = 1; probe < span && TerrainStabilityIsCeiling(world, x + probe, y); ++probe) {
        int candidate = MaterialAt(WorldGetCell(world, x + probe, y))->span;

        if (candidate < weakest) weakest = candidate;
    }
    if (leftAnchored && rightAnchored) {
        return span > weakest;
    }
    if (leftAnchored || rightAnchored) {
        return span > weakest / 2;
    }
    return true;
}

int TerrainStabilityProcess(TerrainStabilitySystem *system, World *world)
{
    int checks = 0;

    if (system == NULL || world == NULL || world->cells == NULL) {
        return 0;
    }
    system->stats.crumblesThisTick = 0;
    while (system->count > 0 && checks < TERRAIN_STABILITY_CHECKS_PER_TICK &&
           system->stats.crumblesThisTick < TERRAIN_STABILITY_CRUMBLES_PER_TICK) {
        TerrainStabilityEntry entry = system->queue[system->head];

        system->head = (system->head + 1) % TERRAIN_STABILITY_QUEUE_CAPACITY;
        --system->count;
        ++checks;
        if (entry.delay > 0) {
            /* Not yet: back of the queue, a tick closer. Counts as a check
               so a queue of waiting entries still bounds the tick. */
            TerrainStabilityQueue(system, entry.x, entry.y, entry.delay - 1);
            --system->stats.cellsQueued;
            continue;
        }
        ++system->stats.checks;
        if (!TerrainStabilityFails(world, entry.x, entry.y)) {
            continue;
        }
        /* It gives way: the cell becomes rubble, which falls on its own from
           here, and what it was holding up — the cell above, and the ceiling
           either side, whose run just lost a cell — is asked next. */
        WorldSetCell(world, entry.x, entry.y, MATERIAL_RUBBLE);
        /* Logged like any other cut: what the crumbled cell was holding up
           may have been holding on by it alone, and the detach check is what
           finds out. */
        WorldRecordDestruction(world, entry.x, entry.y, entry.x, entry.y);
        ++system->stats.crumbles;
        ++system->stats.crumblesThisTick;
        TerrainStabilityQueue(system, entry.x, entry.y - 1, 3);
        TerrainStabilityQueue(system, entry.x - 1, entry.y, 0);
        TerrainStabilityQueue(system, entry.x + 1, entry.y, 0);
    }
    return system->stats.crumblesThisTick;
}
