#ifndef TERRAIN_DETACH_H
#define TERRAIN_DETACH_H

/* Automatic detachment: the bridge that turns a destructive world mutation into
 * falling terrain.
 *
 *     destructive world mutation
 *             |
 *     bounded changed region        (World records it; see WorldRecordDestruction)
 *             |
 *     candidate seed cells          (this module)
 *             |
 *     bounded detached detector     (world_components.h)
 *             |
 *     atomic extraction             (terrain_extraction.h)
 *             |
 *     TerrainBody                   (dynamic_terrain.h, terrain_physics.h)
 *
 * The one rule that shapes everything here: **Emberfall never scans the world
 * looking for detached terrain.** The production map is 16384x1440 and every
 * solid cell in the ground is one connected mass, so a search that is allowed
 * to wander is a fourteen-million-cell flood fill. Checks run only where a
 * known destructive operation just removed structural material, and only inside
 * a region whose size is a compile-time constant.
 *
 * The second rule is inherited from the detector and must not be weakened: a
 * component may be extracted only when it is *proven* free. Failing to notice a
 * loose fragment costs nothing — it stays static until the next blast near it.
 * Tearing out a piece of the main landmass would be unrecoverable.
 *
 * This module owns no world state. It reads the world's destruction log, asks
 * the detector, and calls the ordinary atomic extraction path; it never copies
 * or clears cells itself, so the "a failure leaves the world byte-for-byte
 * unchanged" contract of EF-DYN-003 holds here for free.
 */

#include <stdbool.h>
#include <stdint.h>

#include "dynamic_terrain.h"
#include "game_events.h"
#include "world.h"
#include "world_components.h"

/* Side of the square the detector is allowed to look inside, centred on the
   damage. It is the detector's own maximum: there is no reason to ask for less,
   the visited bitmap is sized for it either way, and a wider window is what
   would let a search wander. */
#define TERRAIN_DETACH_SEARCH_SPAN WORLD_COMPONENT_MAX_SPAN

/* A damaged box the window cannot cover is refused outright, so the world's
   aggregation limit has to leave room inside the window — otherwise ordinary
   damage would routinely produce regions nothing ever looks at. */
_Static_assert(WORLD_DESTRUCTION_MAX_SPAN <= TERRAIN_DETACH_SEARCH_SPAN,
               "damage can be aggregated wider than the detach search window");

typedef struct TerrainDetachConfig {
    /* Fragments smaller than this stay static. Without it a single blast
       leaves dozens of one- and two-cell bodies, each holding a body slot and a
       raster, none of them worth a rigid body. They are terrain chips, and
       terrain chips are what the particle system is for. */
    int minimumBodyCells;
    /* Fragments larger than this stay static. A large piece is exactly the case
       where a mistake is most expensive and the simulation cost is highest, and
       nothing about the current physics is tuned for a cliff-sized body. This
       also bounds the detector: a search gives up one cell past this, so a seed
       in the main landmass costs this much and not a flood fill. */
    int maximumBodyCells;
    /* Detections attempted per damaged region. Every candidate costs at most
       `maximumBodyCells` cell visits, so this is the second half of the work
       bound. */
    int maxCandidatesPerRegion;
    /* Extractions performed per call. Bodies are the expensive resource, and a
       tick that severs six pieces may take them over several ticks instead. */
    int maxExtractionsPerTick;
} TerrainDetachConfig;

typedef struct TerrainDetachStats {
    /* Damaged regions taken off the world's log, and the ones refused because
       the log had already overflowed or the damage was wider than a search
       region can cover. */
    int regionsProcessed;
    int regionsRefused;
    /* Detections attempted, and solid cells offered as seeds. */
    int detachChecks;
    int detachCandidates;
    int autoDetachSucceeded;
    /* Why the rest were left alone. Anchored and unknown are the detector's
       own conservatism; the remaining three are this module's policy. */
    int autoDetachRejectedAnchored;
    int autoDetachRejectedUnknown;
    int autoDetachRejectedTooSmall;
    int autoDetachRejectedTooLarge;
    int autoDetachRejectedBudget;
    /* Cells that left the static world this way, useful for telling a session
       that shed a little rubble from one that shed a hillside. */
    int autoDetachCells;
    /* Cells the detector walked, summed over every check. This is the number
       the whole design exists to bound: it must stay a function of the
       configuration and never of the world's size, and a test holds it to
       `maximumBodyCells + 1` per check. */
    int detachCellsExplored;
} TerrainDetachStats;

typedef struct TerrainDetachSystem {
    TerrainDetachConfig config;
    TerrainDetachStats stats;
    /* One workspace, owned here. At ~34 KiB it is far too large for the stack
       of a per-tick call, and there is exactly one caller, so a single
       long-lived copy is both the cheapest and the clearest arrangement. */
    WorldComponentWorkspace workspace;
    /* One bit per cell of the current search window: ground a previous search
       already covered. Seeds inside it are skipped, which is what stops a
       damaged hillside from being explored once per seed. Cleared per region,
       and only over the prefix that region uses. */
    uint32_t covered[(TERRAIN_DETACH_SEARCH_SPAN * TERRAIN_DETACH_SEARCH_SPAN + 31) / 32];
} TerrainDetachSystem;

TerrainDetachConfig TerrainDetachDefaultConfig(void);
void TerrainDetachInit(TerrainDetachSystem *system);
/* Zeroes the counters. The configuration is left alone: it is a tuning choice,
   not session state. */
void TerrainDetachResetStats(TerrainDetachSystem *system);

/* Drains the world's destruction log and extracts whatever it can prove loose.
   Returns the number of bodies created.

   Must be called with the world in a settled state — after the destructive
   operation has finished every one of its cell writes, and outside WorldUpdate.
   Reading connectivity from a half-applied mutation would answer a question
   about a world that never existed.

   `events` may be NULL; when it is not, one GAME_EVENT_TERRAIN_DETACHED is
   published per body created. */
int TerrainDetachProcess(TerrainDetachSystem *system, World *world,
                         DynamicTerrainSystem *terrain, GameEventBuffer *events);

#endif
