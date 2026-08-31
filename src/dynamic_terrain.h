#ifndef DYNAMIC_TERRAIN_H
#define DYNAMIC_TERRAIN_H

/* Fixed-capacity store for pieces of terrain that have stopped being part of
 * the cellular world and become bodies in their own right.
 *
 * The shape of the model matters more than any of its details:
 *
 *     static cellular World
 *         +
 *     DynamicTerrainSystem
 *         └── TerrainBody[N]
 *
 * A TerrainBody is one large connected lump of terrain, not an entity per cell.
 * The fourteen million cells of the world stay a specialised data-oriented
 * simulation; only the handful of lumps that have been torn off it are ever
 * modelled individually. Anything that tempts a future change toward
 * one-entity-per-cell is a mistake in this file's terms.
 *
 * This subsystem is behaviour-neutral at present: nothing extracts a body from
 * the world, nothing moves, collides, draws or fractures one. It exists so that
 * the tasks which do those things have a stable place to stand. It never sees a
 * World and cannot modify one.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

/* ---- hard budgets -------------------------------------------------------
 *
 * Dynamic terrain must never be able to grow without bound: a player with a
 * force blast and some patience would otherwise turn a hillside into an
 * unbounded allocation. Every limit here is compile-time, and every one of them
 * is enforced by refusing work rather than by growing.
 */

/* Bodies alive at once. A single blast severs a handful of pieces; thirty-two
   leaves room for a chaotic scene without pretending the budget is infinite. */
#define MAX_TERRAIN_BODIES 32

/* Raster slots reserved for each body. A body's bounding box must fit in this
   many cells — not in a square, so a long thin slab is as welcome as a
   compact lump. 8192 holds a 64x128 shard, or any shape whose bounding box is
   at most twice the 4096-cell component limit the detector will hand over. */
#define TERRAIN_BODY_RASTER_CAPACITY 8192

/* Longest side of a body's bounding box, matching the detector's own region
   limit so that anything WorldFindComponent can report as detached is a shape
   this can hold. */
#define TERRAIN_BODY_MAX_SPAN 128

/* Occupied cells in one body, inherited from WORLD_COMPONENT_MAX_CELLS: a body
   can only ever be built from a component the detector proved free. */
#define MAX_TERRAIN_BODY_CELLS 4096

/* Total raster storage, and therefore the whole subsystem's memory:
   32 x 8192 x (1 byte material + 4 bytes temperature) = 1.25 MiB, allocated
   once and never resized. */
#define MAX_TERRAIN_RASTER_CELLS (MAX_TERRAIN_BODIES * TERRAIN_BODY_RASTER_CAPACITY)

/* ---- handles ------------------------------------------------------------
 *
 * Slots are reused, so a raw index would eventually name a different body than
 * the one its holder meant — the classic use-after-free that does not crash.
 * Collision pairs, render lists and ability impulses will all hold references
 * across frames, so the generation counter is worth its four bytes: a handle to
 * a freed body resolves to NULL instead of to whatever moved in afterwards.
 */
typedef struct TerrainBodyHandle {
    uint16_t index;
    /* Bumped every time the slot is freed, so a stale handle disagrees with the
       slot it names. Generation zero is never live, which is what makes a
       zero-initialised handle — `TerrainBodyHandle handle = {0};`, which
       callers write without thinking — fail every lookup instead of quietly
       naming body zero. The counter skips zero when it wraps, after 65 536
       reuses of one slot; a stale handle that survives that long and lands on
       the same slot is a risk taken knowingly. */
    uint16_t generation;
} TerrainBodyHandle;

/* Never returned by a successful allocation: the index is out of range and the
   generation is the reserved dead value, so both halves fail the lookup. */
#define TERRAIN_BODY_INVALID_INDEX 0xFFFFu

static inline TerrainBodyHandle TerrainBodyInvalidHandle(void)
{
    TerrainBodyHandle handle;

    handle.index = TERRAIN_BODY_INVALID_INDEX;
    handle.generation = 0u;
    return handle;
}

static inline bool TerrainBodyHandleEquals(TerrainBodyHandle a, TerrainBodyHandle b)
{
    return a.index == b.index && a.generation == b.generation;
}

/* ---- body ---------------------------------------------------------------- */

typedef struct TerrainBody {
    /* Slot bookkeeping. `generation` mirrors the live handle's and is never
       zero. */
    bool active;
    uint16_t generation;
    /* A settled body still exists and still collides, but costs nothing to
       integrate. Nothing sleeps a body yet; the flag is here so that the task
       which adds motion has somewhere to put the decision. */
    bool awake;

    /* Local raster. Cells are addressed row-major as ly * width + lx, in the
       body's own frame, and `width * height` never exceeds
       TERRAIN_BODY_RASTER_CAPACITY. */
    int width;
    int height;
    /* Occupied slots, maintained by DynamicTerrainSetCell. */
    int cellCount;
    /* Bounding box of the occupied cells in local coordinates, inclusive.
       Computed by DynamicTerrainFinalizeBody; meaningless while cellCount is
       zero. The raster may be larger than this: a body is allowed to keep slack
       around its contents. */
    int minimumX;
    int minimumY;
    int maximumX;
    int maximumY;

    /* Placement. `position` is the world position of the centre of mass, which
       is what a rigid body rotates about; `centerOfMass` says where that point
       sits in the local raster, so a cell can be placed without ambiguity:

           world = position + rotate(local + (0.5, 0.5) - centerOfMass, angle)
    */
    Vector2 position;
    float angle;
    Vector2 velocity;
    float angularVelocity;

    /* Derived from the raster by DynamicTerrainFinalizeBody. */
    float mass;
    /* Moment of inertia about the centre of mass. */
    float inertia;
    Vector2 centerOfMass;

    /* World cell that local (0, 0) came from. Extraction records it so a body
       can be put back where it belongs, and so a failed extraction knows
       exactly which cells to restore. */
    int sourceX;
    int sourceY;
} TerrainBody;

typedef struct DynamicTerrainStats {
    int activeBodies;
    /* Raster slots reserved by live bodies, not occupied cells: this is the
       number that has to stay inside the budget. */
    int allocatedDynamicCells;
    int peakBodies;
    int peakDynamicCells;
    /* Extraction outcomes, counted by terrain_extraction.c. Structural rather
       than timed, so they mean the same thing on every machine. */
    int extractionsSucceeded;
    int extractionsFailed;
} DynamicTerrainStats;

typedef struct DynamicTerrainSystem {
    TerrainBody bodies[MAX_TERRAIN_BODIES];
    /* Two parallel arrays rather than one array of structs. A struct holding a
       byte and a float pads to eight; split, the same data is five, and the
       queries that will run most often — is this slot occupied, what stops the
       player — read only the material plane and so touch a fifth of the memory.
       Both are indexed identically: body slot i owns
       [i * TERRAIN_BODY_RASTER_CAPACITY, ...). */
    uint8_t *material;
    /* Kept as a float, exactly as World stores it. The alternative was fixed
       point, which would have halved a megabyte and introduced a way for a cell
       sitting a fraction below a phase threshold to cross it purely by being
       torn off and put back. At this scale that trade is not worth making. */
    float *temperature;
    DynamicTerrainStats stats;
} DynamicTerrainSystem;

/* One allocation at init, freed at unload, nothing in between: the same
   lifecycle World uses. Returns false if the raster could not be allocated. */
bool DynamicTerrainInit(DynamicTerrainSystem *system);
void DynamicTerrainUnload(DynamicTerrainSystem *system);
/* Frees every body and clears the live statistics. Peak figures survive, since
   their value is telling you what the session has demanded. */
void DynamicTerrainReset(DynamicTerrainSystem *system);

/* Reserves a slot with a `width` x `height` raster, cleared to empty. Returns
   an invalid handle when no slot is free or the shape does not fit the
   budgets. */
TerrainBodyHandle DynamicTerrainAllocBody(DynamicTerrainSystem *system,
                                          int width, int height);
/* Safe on an already-free or stale handle. */
void DynamicTerrainFreeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle);

/* NULL for an invalid, stale or freed handle. */
TerrainBody *DynamicTerrainGet(DynamicTerrainSystem *system,
                               TerrainBodyHandle handle);
const TerrainBody *DynamicTerrainGetConst(const DynamicTerrainSystem *system,
                                          TerrainBodyHandle handle);

/* Writes one local cell and keeps `cellCount` correct. Out-of-range
   coordinates and dead handles are ignored. */
void DynamicTerrainSetCell(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                           int localX, int localY, CellMaterial material,
                           float temperature);
/* MATERIAL_EMPTY for an unoccupied slot, and for any out-of-range read. */
CellMaterial DynamicTerrainCellAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY);
float DynamicTerrainTemperatureAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY);

/* Recomputes bounds, mass, centre of mass and inertia from the raster. Call it
   once a body has been populated or edited; until then those fields describe
   the body as it was. A body whose cells all have zero density reports no
   extent and no mass, and every consumer should treat it as empty. */
void DynamicTerrainFinalizeBody(DynamicTerrainSystem *system,
                                TerrainBodyHandle handle);

const DynamicTerrainStats *DynamicTerrainStatistics(const DynamicTerrainSystem *system);

#endif
