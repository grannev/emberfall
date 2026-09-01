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
 * Extraction and collision live in separate modules, while presentation reads
 * this store through TerrainBodyRenderer. This subsystem itself still never
 * sees a World, GPU resource or draw call and cannot modify either of them.
 */

#include <math.h>
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
   can only ever be built from a component the detector proved free. It is also
   the cap on a body's surface list, because a filigree body can have every one
   of its cells on the surface. */
#define MAX_TERRAIN_BODY_CELLS 4096

/* Total material/temperature raster storage:
   32 x 8192 x (1 byte material + 4 bytes temperature) = 1.25 MiB, allocated
   once and never resized. Collision also owns a fixed 0.50 MiB surface list. */
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
       integrate: DynamicTerrainUpdate skips it entirely. */
    bool awake;
    /* Seconds this body has been below both sleep thresholds. Reset the moment
       it moves again, so a body only sleeps after a continuous quiet spell
       rather than after one lucky tick. */
    float sleepTimer;

    /* Local raster. Cells are addressed row-major as ly * width + lx, in the
       body's own frame, and `width * height` never exceeds
       TERRAIN_BODY_RASTER_CAPACITY. */
    int width;
    int height;
    /* Occupied slots, maintained by DynamicTerrainSetCell. */
    int cellCount;
    /* Monotonic content version for read-only consumers such as the renderer.
       Translation and rotation do not touch it; a material or temperature edit
       does. Generation distinguishes slot reuse, revision distinguishes a
       changed raster inside the same live body. Zero is skipped on wrap so a
       zero-initialised cache can never look current by accident. */
    uint32_t rasterRevision;
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
       sits in the local raster. The two together define the one transform every
       other system must agree on — see TerrainBodyLocalToWorld below, which is
       that definition in executable form:

           world = position + rotate(local + (0.5, 0.5) - centerOfMass, angle)

       `angle` is in radians and is kept in (-PI, PI]. */
    Vector2 position;
    float angle;
    Vector2 velocity;
    float angularVelocity;

    /* Derived from the raster by DynamicTerrainFinalizeBody. */
    float mass;
    /* Moment of inertia about the centre of mass. */
    float inertia;
    Vector2 centerOfMass;
    /* Distance from the centre of mass to the farthest occupied cell corner.
       Collision uses it to bound how far rotation can carry the body's edge in
       one step, which is what stops a spinning body tunnelling. */
    float boundingRadius;
    /* Occupied cells with at least one empty four-neighbour in the raster —
       everything that can actually touch the world. An interior cell is walled
       in by its own body and can never make first contact, so testing it would
       be pure cost: a solid 64x64 block has 4096 cells and 252 of them here. */
    int surfaceCount;

    /* World cell that local (0, 0) came from. Extraction records it so a body
       can be put back where it belongs, and so a failed extraction knows
       exactly which cells to restore. */
    int sourceX;
    int sourceY;
} TerrainBody;

/* Tuning for how bodies move. Gathered in one struct rather than scattered
   across the integrator so a change is a change to one value, and so tests can
   ask for a world without gravity without pretending. */
typedef struct DynamicTerrainConfig {
    /* Cells per second squared, positive downward because world Y grows
       downward. Applied as an acceleration, so it is independent of mass, as
       gravity is. */
    float gravity;
    /* Fraction of speed shed per second, applied as exp(-damping * dt) so the
       result depends on elapsed time and not on how many times update was
       called. */
    float linearDamping;
    float angularDamping;
    /* Ceilings, not tuning: they stop one bad impulse turning into a body that
       crosses the map in a tick, which is also what will keep collision from
       having to solve tunnelling later. */
    float maximumSpeed;
    float maximumAngularSpeed;
    /* A body sleeps once it has stayed below both of these for `sleepDelay`
       seconds. `linearSleepSpeed` must stay below `gravity * dt` or a body in
       free fall would doze off in mid-air; the default pair satisfies that at
       the 60 Hz fixed step, and a test holds it. */
    float linearSleepSpeed;
    float angularSleepSpeed;
    float sleepDelay;
    /* Collision response. Rock is not rubber: the default bounce is small
       enough that a slab lands and stays landed. */
    float restitution;
    float friction;
} DynamicTerrainConfig;

DynamicTerrainConfig DynamicTerrainDefaultConfig(void);

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
    /* Refreshed by every TerrainPhysicsUpdate. */
    int awakeBodies;
    int sleepingBodies;
    int collisionBodies;
    int collisionContacts;
    int collisionSubsteps;
    /* Highest contact count any one body has produced, so the cap can be
       judged against what actually happens rather than guessed at. */
    int maxContactsObserved;
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
    /* Surface cells, in local raster coordinates, laid out per body slot the
       same way the raster is: slot i owns [i * MAX_TERRAIN_BODY_CELLS, ...).
       int16_t is ample — a local coordinate never exceeds TERRAIN_BODY_MAX_SPAN. */
    int16_t *surfaceX;
    int16_t *surfaceY;
    DynamicTerrainConfig config;
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

/* The pieces of a step, exposed so terrain_physics.c can interleave them with
   collision. They act on one body and do no bookkeeping of their own; callers
   outside that module want TerrainPhysicsUpdate instead. */
void DynamicTerrainIntegrateBody(DynamicTerrainSystem *system, TerrainBody *body,
                                 float deltaTime);
void DynamicTerrainSettleBody(DynamicTerrainSystem *system, TerrainBody *body,
                              float deltaTime);

/* A step this module will act on. Non-finite, non-positive or absurdly large
   values are refused rather than integrated, because a bad step is a caller bug
   and silently scaling it would hide one. */
static inline bool TerrainStepIsUsable(float deltaTime)
{
    return deltaTime == deltaTime && deltaTime > 0.0f && deltaTime <= 0.25f;
}

static inline bool TerrainFiniteSample(Vector2 value)
{
    return value.x == value.x && value.y == value.y &&
           value.x > -1.0e9f && value.x < 1.0e9f &&
           value.y > -1.0e9f && value.y < 1.0e9f;
}

/* Puts a body back into integration and restarts its quiet spell. Safe on a
   dead handle. */
void DynamicTerrainWakeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle);
/* Sets both velocities and wakes the body. Non-finite values are refused. */
void DynamicTerrainSetVelocity(DynamicTerrainSystem *system,
                               TerrainBodyHandle handle, Vector2 velocity,
                               float angularVelocity);
/* Applies an impulse at a world point: linear velocity changes by J/m, angular
   velocity by (r x J)/I where r runs from the centre of mass to the point. One
   function rather than separate linear and angular ones because that is the
   whole of the rigid-body rule — an impulse through the centre of mass turns
   nothing, and the cross product says so without a special case. Wakes the
   body. A body with no mass or no inertia cannot be pushed. */
void DynamicTerrainApplyImpulse(DynamicTerrainSystem *system,
                                TerrainBodyHandle handle, Vector2 impulse,
                                Vector2 worldPoint);

const DynamicTerrainStats *DynamicTerrainStatistics(const DynamicTerrainSystem *system);

/* The transform, in executable form. Every other system — renderer, collision,
   fracture — must go through these two rather than re-derive the convention,
   because a second derivation is a second chance to get it wrong.

   `localX`/`localY` are raster coordinates; add 0.5 to address a cell's centre
   rather than its corner. */
static inline Vector2 TerrainBodyLocalToWorld(const TerrainBody *body,
                                              float localX, float localY)
{
    float offsetX = localX - body->centerOfMass.x;
    float offsetY = localY - body->centerOfMass.y;
    float cosine = cosf(body->angle);
    float sine = sinf(body->angle);

    return (Vector2){body->position.x + offsetX * cosine - offsetY * sine,
                     body->position.y + offsetX * sine + offsetY * cosine};
}

/* The exact inverse. Collision will need it to ask which of a body's cells a
   world point falls in. */
static inline Vector2 TerrainBodyWorldToLocal(const TerrainBody *body,
                                              float worldX, float worldY)
{
    float offsetX = worldX - body->position.x;
    float offsetY = worldY - body->position.y;
    float cosine = cosf(body->angle);
    float sine = sinf(body->angle);

    return (Vector2){body->centerOfMass.x + offsetX * cosine + offsetY * sine,
                     body->centerOfMass.y - offsetX * sine + offsetY * cosine};
}

#endif
