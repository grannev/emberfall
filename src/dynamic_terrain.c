/* Storage and bookkeeping for detached pieces of terrain. See
 * dynamic_terrain.h for the model; this file records the two decisions that
 * shape the implementation.
 *
 * Storage is a fixed raster per body slot, out of one arena allocated at init.
 * A shared arena with variable-size allocations would pack tighter, but freeing
 * bodies in arbitrary order fragments it, and the ways out of that —
 * compaction, free lists, slabs — all cost more code and more failure modes
 * than the memory they save. Thirty-two slots of 8192 cells is 1.25 MiB, which
 * next to a 167 MiB world is not worth a single line of allocator.
 *
 * Mass is derived from material density and cell area. The units are relative,
 * not kilograms: nothing here needs an absolute scale, only for a slab of rock
 * to fall harder and turn more slowly than the same slab of ice.
 */
#include "dynamic_terrain.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "materials.h"

/* A cell is one unit square, so its area is one and its mass is its density.
   Its own moment of inertia about its centre is m * (w^2 + h^2) / 12 = m / 6.
   Including that term is what stops a one-cell body having zero inertia and
   therefore infinite angular acceleration the moment anything touches it. */
#define TERRAIN_CELL_SELF_INERTIA (1.0f / 6.0f)

/* Generation zero is reserved as "never live" so that a zero-initialised
   handle cannot name body zero by accident. */
#define TERRAIN_BODY_FIRST_GENERATION 1u

static bool TerrainSlotIsLive(const DynamicTerrainSystem *system,
                              TerrainBodyHandle handle)
{
    if (system == NULL || handle.index >= MAX_TERRAIN_BODIES ||
        handle.generation == 0u) {
        return false;
    }
    return system->bodies[handle.index].active &&
           system->bodies[handle.index].generation == handle.generation;
}

static size_t TerrainRasterBase(uint16_t index)
{
    return (size_t)index * (size_t)TERRAIN_BODY_RASTER_CAPACITY;
}

bool DynamicTerrainInit(DynamicTerrainSystem *system)
{
    int index;

    if (system == NULL) {
        return false;
    }
    memset(system, 0, sizeof(*system));
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        system->bodies[index].generation = TERRAIN_BODY_FIRST_GENERATION;
    }
    system->material = calloc((size_t)MAX_TERRAIN_RASTER_CELLS,
                              sizeof(*system->material));
    system->temperature = calloc((size_t)MAX_TERRAIN_RASTER_CELLS,
                                 sizeof(*system->temperature));
    if (system->material == NULL || system->temperature == NULL) {
        DynamicTerrainUnload(system);
        return false;
    }
    return true;
}

void DynamicTerrainUnload(DynamicTerrainSystem *system)
{
    if (system == NULL) {
        return;
    }
    free(system->material);
    free(system->temperature);
    memset(system, 0, sizeof(*system));
}

void DynamicTerrainReset(DynamicTerrainSystem *system)
{
    int index;

    if (system == NULL || system->material == NULL) {
        return;
    }
    /* Generations are deliberately not reset. A handle held across a world
       regeneration must not come back to life pointing at a body that happens
       to land in the same slot. */
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        if (system->bodies[index].active) {
            DynamicTerrainFreeBody(system, (TerrainBodyHandle){
                (uint16_t)index, system->bodies[index].generation});
        }
    }
    system->stats.activeBodies = 0;
    system->stats.allocatedDynamicCells = 0;
}

TerrainBodyHandle DynamicTerrainAllocBody(DynamicTerrainSystem *system,
                                          int width, int height)
{
    TerrainBodyHandle handle = TerrainBodyInvalidHandle();
    TerrainBody *body;
    int index;

    if (system == NULL || system->material == NULL) {
        return handle;
    }
    if (width <= 0 || height <= 0 || width > TERRAIN_BODY_MAX_SPAN ||
        height > TERRAIN_BODY_MAX_SPAN ||
        width * height > TERRAIN_BODY_RASTER_CAPACITY) {
        return handle;
    }

    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        if (!system->bodies[index].active) {
            break;
        }
    }
    if (index >= MAX_TERRAIN_BODIES) {
        /* Full. Refusing is the whole point of a hard budget: the caller
           decides what to do without a body, and nothing grows. */
        return handle;
    }

    body = &system->bodies[index];
    /* Keep the generation; everything else starts clean. */
    {
        uint16_t generation = body->generation;

        memset(body, 0, sizeof(*body));
        body->generation = generation;
    }
    body->active = true;
    body->awake = true;
    body->width = width;
    body->height = height;
    /* An empty body has no extent. Finalize will set real bounds once cells
       arrive; until then the inverted range says "nothing here". */
    body->minimumX = 0;
    body->minimumY = 0;
    body->maximumX = -1;
    body->maximumY = -1;

    memset(system->material + TerrainRasterBase((uint16_t)index), 0,
           (size_t)(width * height) * sizeof(*system->material));
    memset(system->temperature + TerrainRasterBase((uint16_t)index), 0,
           (size_t)(width * height) * sizeof(*system->temperature));

    ++system->stats.activeBodies;
    system->stats.allocatedDynamicCells += width * height;
    if (system->stats.activeBodies > system->stats.peakBodies) {
        system->stats.peakBodies = system->stats.activeBodies;
    }
    if (system->stats.allocatedDynamicCells > system->stats.peakDynamicCells) {
        system->stats.peakDynamicCells = system->stats.allocatedDynamicCells;
    }

    handle.index = (uint16_t)index;
    handle.generation = body->generation;
    return handle;
}

void DynamicTerrainFreeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle)
{
    TerrainBody *body;

    if (!TerrainSlotIsLive(system, handle)) {
        return;
    }
    body = &system->bodies[handle.index];
    system->stats.allocatedDynamicCells -= body->width * body->height;
    --system->stats.activeBodies;
    body->active = false;
    /* Bumping on free is what makes every outstanding handle to this body stale
       from this moment, including the one that did the freeing. Zero is
       skipped: it is the value a zero-initialised handle carries. */
    ++body->generation;
    if (body->generation == 0u) {
        body->generation = TERRAIN_BODY_FIRST_GENERATION;
    }
}

TerrainBody *DynamicTerrainGet(DynamicTerrainSystem *system,
                               TerrainBodyHandle handle)
{
    if (!TerrainSlotIsLive(system, handle)) {
        return NULL;
    }
    return &system->bodies[handle.index];
}

const TerrainBody *DynamicTerrainGetConst(const DynamicTerrainSystem *system,
                                          TerrainBodyHandle handle)
{
    if (!TerrainSlotIsLive(system, handle)) {
        return NULL;
    }
    return &system->bodies[handle.index];
}

/* Resolves a local coordinate to an arena index, or returns false when the
   handle is dead or the coordinate is outside the body's raster. */
static bool TerrainCellIndex(const DynamicTerrainSystem *system,
                             TerrainBodyHandle handle, int localX, int localY,
                             size_t *index)
{
    const TerrainBody *body;

    if (!TerrainSlotIsLive(system, handle)) {
        return false;
    }
    body = &system->bodies[handle.index];
    if (localX < 0 || localY < 0 || localX >= body->width ||
        localY >= body->height) {
        return false;
    }
    *index = TerrainRasterBase(handle.index) +
             (size_t)localY * (size_t)body->width + (size_t)localX;
    return true;
}

void DynamicTerrainSetCell(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                           int localX, int localY, CellMaterial material,
                           float temperature)
{
    TerrainBody *body;
    size_t index;
    bool wasOccupied;
    bool isOccupied;

    if (!TerrainCellIndex(system, handle, localX, localY, &index)) {
        return;
    }
    if (material < 0 || material >= MATERIAL_COUNT) {
        return;
    }
    body = &system->bodies[handle.index];
    wasOccupied = system->material[index] != (uint8_t)MATERIAL_EMPTY;
    isOccupied = material != MATERIAL_EMPTY;

    system->material[index] = (uint8_t)material;
    system->temperature[index] = isOccupied ? temperature : 0.0f;
    if (isOccupied && !wasOccupied) {
        ++body->cellCount;
    } else if (!isOccupied && wasOccupied) {
        --body->cellCount;
    }
}

CellMaterial DynamicTerrainCellAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY)
{
    size_t index;

    if (!TerrainCellIndex(system, handle, localX, localY, &index)) {
        return MATERIAL_EMPTY;
    }
    return (CellMaterial)system->material[index];
}

float DynamicTerrainTemperatureAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY)
{
    size_t index;

    if (!TerrainCellIndex(system, handle, localX, localY, &index)) {
        return 0.0f;
    }
    return system->temperature[index];
}

void DynamicTerrainFinalizeBody(DynamicTerrainSystem *system,
                                TerrainBodyHandle handle)
{
    TerrainBody *body;
    size_t base;
    float mass = 0.0f;
    float momentX = 0.0f;
    float momentY = 0.0f;
    float inertia = 0.0f;
    int occupied = 0;
    int localY;

    if (!TerrainSlotIsLive(system, handle)) {
        return;
    }
    body = &system->bodies[handle.index];
    base = TerrainRasterBase(handle.index);
    body->minimumX = body->width;
    body->minimumY = body->height;
    body->maximumX = -1;
    body->maximumY = -1;

    /* First pass: extent, total mass and first moments. */
    for (localY = 0; localY < body->height; ++localY) {
        int localX;

        for (localX = 0; localX < body->width; ++localX) {
            size_t index = base + (size_t)localY * (size_t)body->width +
                           (size_t)localX;
            CellMaterial material = (CellMaterial)system->material[index];
            float cellMass;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            ++occupied;
            if (localX < body->minimumX) body->minimumX = localX;
            if (localX > body->maximumX) body->maximumX = localX;
            if (localY < body->minimumY) body->minimumY = localY;
            if (localY > body->maximumY) body->maximumY = localY;

            cellMass = MaterialAt(material)->density;
            mass += cellMass;
            /* Cell centres, so a single cell's centre of mass is its middle
               rather than its corner. */
            momentX += cellMass * ((float)localX + 0.5f);
            momentY += cellMass * ((float)localY + 0.5f);
        }
    }

    body->cellCount = occupied;
    body->mass = mass;
    if (occupied == 0 || mass <= 0.0f) {
        body->minimumX = 0;
        body->minimumY = 0;
        body->maximumX = -1;
        body->maximumY = -1;
        body->centerOfMass = (Vector2){0.0f, 0.0f};
        body->inertia = 0.0f;
        return;
    }
    body->centerOfMass = (Vector2){momentX / mass, momentY / mass};

    /* Second pass: inertia needs the centre of mass, so it cannot share the
       first. Parallel axis theorem per cell, including each cell's own moment
       about its centre. */
    for (localY = 0; localY < body->height; ++localY) {
        int localX;

        for (localX = 0; localX < body->width; ++localX) {
            size_t index = base + (size_t)localY * (size_t)body->width +
                           (size_t)localX;
            CellMaterial material = (CellMaterial)system->material[index];
            float cellMass;
            float dx;
            float dy;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            cellMass = MaterialAt(material)->density;
            dx = (float)localX + 0.5f - body->centerOfMass.x;
            dy = (float)localY + 0.5f - body->centerOfMass.y;
            inertia += cellMass * (dx * dx + dy * dy + TERRAIN_CELL_SELF_INERTIA);
        }
    }
    body->inertia = inertia;
}

const DynamicTerrainStats *DynamicTerrainStatistics(const DynamicTerrainSystem *system)
{
    static const DynamicTerrainStats empty = {0};

    return system != NULL ? &system->stats : &empty;
}
