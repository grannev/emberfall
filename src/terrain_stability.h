#ifndef TERRAIN_STABILITY_H
#define TERRAIN_STABILITY_H

/* Ground that can no longer hold itself up.
 *
 * A ceiling — solid cells with nothing under them — holds between the two
 * supports at its ends by the strength of what it is made of, and every
 * material has a span (`MaterialInfo.span`): dirt bridges a burrow, rock
 * bridges a cavern, and what is asked to bridge more than that crumbles. A
 * crumbled cell becomes rubble, which falls and piles like sand, and the cell
 * above it is a ceiling now and is asked the same question. That is the whole
 * of a cave-in: it starts where the ground was opened, climbs as far as the
 * spans keep failing, and stops at the first layer that can bear its own
 * width — a slab of rock over a burrow of dirt is the roof the dirt never was.
 *
 * Nothing here ever scans the world. The only thing that starts a check is
 * the destruction log the destructive powers already write, the same log that
 * starts a detach check: an opening that was just made is looked at, once,
 * and what crumbles from it is followed cell by cell through a queue with a
 * fixed capacity and a fixed number of cells examined per tick. Ground that
 * was generated unsupported stays where the generator put it until something
 * disturbs it, exactly as it did before this existed, because the question
 * is only ever asked where something changed.
 *
 * A collapse costs no mass: a cell of dirt becomes a cell of rubble, and it
 * lands somewhere. A cell with a support under it is never crumbled,
 * whatever is above it. What a failed run was holding up is cracked off its
 * supports — a column of rubble cut up from each supported end of the run —
 * so that the roof between the cracks is joined to nothing, and the detach
 * check that already reads the destruction log extracts it as a body. That
 * is the difference between a cave-in and a roof turning to sand: the slab
 * falls in one piece, and cracks where it lands.
 */

#include <stdbool.h>

#include "world.h"

/* Cells waiting to be asked whether they still hold. A large cave-in queues
   its whole ceiling, a layer at a time; two thousand is a hundred-cell cave
   twenty layers deep, and a bigger one is bounded by being asked in pieces:
   what falls exposes what is above it, and that is queued when it falls. */
#define TERRAIN_STABILITY_QUEUE_CAPACITY 2048
/* Cells taken from the queue in one tick, and the most that may crumble in
   one tick. A cave-in is meant to be watched happening. */
#define TERRAIN_STABILITY_CHECKS_PER_TICK 96
#define TERRAIN_STABILITY_CRUMBLES_PER_TICK 96
/* The most rows a crack cut at the end of a failed run climbs. A roof thicker
   than this is cut this deep and crumbles the rest a layer at a time. */
#define TERRAIN_STABILITY_CRACK_HEIGHT 64
/* How far either side of a destroyed region the ceilings are looked at: a
   cut at the edge of a burrow shortens the span of a ceiling it never touched. */
#define TERRAIN_STABILITY_MARGIN 4

typedef struct TerrainStabilityStats {
    /* Since init. */
    int regionsNoted;
    int cellsQueued;
    int queueRefusals;
    int checks;
    int crumbles;
    /* Cracks cut up from the ends of failed runs, and the cells they cut. */
    int cracks;
    int crackCells;
    /* Refreshed by every update. */
    int crumblesThisTick;
} TerrainStabilityStats;

typedef struct TerrainStabilityEntry {
    int32_t x;
    int32_t y;
    /* Ticks to wait before asking. The cell over one that just crumbled is
       asked once the rubble has had time to fall out from under it; asked at
       once, it found rubble under it and called that support. */
    int32_t delay;
} TerrainStabilityEntry;

typedef struct TerrainStabilitySystem {
    TerrainStabilityEntry queue[TERRAIN_STABILITY_QUEUE_CAPACITY];
    int head;
    int count;
    TerrainStabilityStats stats;
} TerrainStabilitySystem;

void TerrainStabilityInit(TerrainStabilitySystem *system);

/* Queues every ceiling cell in the world's destruction log, plus a margin.
   Reads the log without clearing it; whoever drains the log does that. */
void TerrainStabilityNoteDestruction(TerrainStabilitySystem *system,
                                     const World *world);

/* Asks a bounded number of queued cells whether they still hold, crumbles the
   ones that do not, and queues what their fall exposed. Returns the number
   crumbled this tick. */
int TerrainStabilityProcess(TerrainStabilitySystem *system, World *world);

/* The unsupported run of ceiling that (x, y) belongs to, in cells, up to
   `limit`; zero when the cell is not a ceiling cell. Exposed for tests. */
int TerrainStabilitySpanAt(const World *world, int x, int y, int limit);

#endif
