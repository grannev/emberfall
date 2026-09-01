/* World -> TerrainBody extraction. See terrain_extraction.h for the contract;
 * this file records how atomicity is achieved and why the origin was chosen the
 * way it was.
 *
 * Atomicity is structural. The work is ordered so that everything able to fail
 * happens before anything is changed:
 *
 *     preflight   read-only: is the component provable, does it fit
 *     allocate    a body, which may fail; the world is still untouched
 *     populate    read the world, write the body, re-check every cell
 *     commit      clear the world cells — the first and only mutation
 *
 * There is no rollback path because there is nothing to roll back. A failure
 * during populate frees a body no caller has seen. This is worth more than a
 * compensating undo would be: an undo has to be correct in a case that is by
 * definition rare and therefore rarely exercised.
 */
#include "terrain_extraction.h"

#include <stddef.h>

#include "materials.h"

const char *TerrainExtractStatusName(TerrainExtractStatus status)
{
    switch (status) {
    case TERRAIN_EXTRACT_OK: return "OK";
    case TERRAIN_EXTRACT_NOT_DETACHED: return "NOT_DETACHED";
    case TERRAIN_EXTRACT_NO_BODY_SLOT: return "NO_BODY_SLOT";
    case TERRAIN_EXTRACT_CELL_CAPACITY: return "CELL_CAPACITY";
    case TERRAIN_EXTRACT_WORLD_CHANGED: return "WORLD_CHANGED";
    default: return "INVALID";
    }
}

static TerrainExtractResult ExtractFailure(DynamicTerrainSystem *terrain,
                                           TerrainExtractStatus status)
{
    TerrainExtractResult result;

    result.status = status;
    result.body = TerrainBodyInvalidHandle();
    result.cellCount = 0;
    if (terrain != NULL) {
        ++terrain->stats.extractionsFailed;
        /* Counted here rather than at each refusal site so that every way of
           running out of cell room — the fixed per-body limits and the shared
           budget alike — reports through one counter. */
        if (status == TERRAIN_EXTRACT_CELL_CAPACITY) {
            ++terrain->stats.cellCapacityFailures;
        }
    }
    return result;
}

TerrainExtractResult TerrainExtractComponent(World *world,
                                             DynamicTerrainSystem *terrain,
                                             const WorldComponentWorkspace *workspace,
                                             WorldComponentResult component)
{
    TerrainExtractResult result;
    TerrainBodyHandle handle;
    TerrainBody *body;
    int width;
    int height;
    int index;

    if (world == NULL || world->cells == NULL || terrain == NULL ||
        workspace == NULL) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_INVALID);
    }

    /* ---- preflight: nothing below this line writes to the world ---------- */

    /* Only a proven-free component may be torn out. The detector is
       deliberately conservative, and honouring that here is what makes the
       conservatism worth having. */
    if (component.status != WORLD_COMPONENT_DETACHED) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_NOT_DETACHED);
    }
    if (component.cellCount <= 0 ||
        component.cellCount > WORLD_COMPONENT_MAX_CELLS ||
        component.maximumX < component.minimumX ||
        component.maximumY < component.minimumY) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_INVALID);
    }
    if (component.minimumX < 0 || component.minimumY < 0 ||
        component.maximumX >= world->width || component.maximumY >= world->height) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_INVALID);
    }

    width = component.maximumX - component.minimumX + 1;
    height = component.maximumY - component.minimumY + 1;
    /* Asked before allocating, so that a shape the store cannot hold is named
       as a capacity problem rather than as a missing slot. */
    if (component.cellCount > MAX_TERRAIN_BODY_CELLS ||
        width > TERRAIN_BODY_MAX_SPAN || height > TERRAIN_BODY_MAX_SPAN ||
        width * height > TERRAIN_BODY_RASTER_CAPACITY) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_CELL_CAPACITY);
    }
    /* The shared budget, asked the same way and in the same place. A body that
       fits on its own can still be one body too many: what bounds collision
       work — and, later, drawing — is the total number of occupied cells, not
       any single body's size. Refusing here keeps the refusal free: the world
       is untouched and no slot has been taken. */
    if (terrain->stats.dynamicCellsUsed + component.cellCount >
        terrain->config.maxDynamicCells) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_CELL_CAPACITY);
    }

    /* ---- allocate: still no world mutation ------------------------------- */

    handle = DynamicTerrainAllocBody(terrain, width, height);
    body = DynamicTerrainGet(terrain, handle);
    if (body == NULL) {
        return ExtractFailure(terrain, TERRAIN_EXTRACT_NO_BODY_SLOT);
    }

    /* ---- populate: reads the world, writes only the body ----------------- */

    for (index = 0; index < component.cellCount; ++index) {
        int worldX = (int)workspace->cellX[index];
        int worldY = (int)workspace->cellY[index];
        CellMaterial material;

        /* The detector ran at some earlier moment, and a tick or a beam may
           have happened since. A cell that has stopped being solid is not part
           of the component any more, and copying it would put a hole — or a
           puddle — inside a body of rock. Checking here, before the commit,
           keeps the failure free. */
        if (worldX < component.minimumX || worldX > component.maximumX ||
            worldY < component.minimumY || worldY > component.maximumY) {
            DynamicTerrainFreeBody(terrain, handle);
            return ExtractFailure(terrain, TERRAIN_EXTRACT_INVALID);
        }
        material = WorldGetCell(world, worldX, worldY);
        if (!WorldMaterialIsSolid(material)) {
            DynamicTerrainFreeBody(terrain, handle);
            return ExtractFailure(terrain, TERRAIN_EXTRACT_WORLD_CHANGED);
        }

        DynamicTerrainSetCell(terrain, handle, worldX - component.minimumX,
                              worldY - component.minimumY, material,
                              WorldGetTemperature(world, worldX, worldY));
    }

    /* A component's cells are distinct by construction, so a short count means
       the caller's result and workspace do not describe the same search. */
    if (body->cellCount != component.cellCount) {
        DynamicTerrainFreeBody(terrain, handle);
        return ExtractFailure(terrain, TERRAIN_EXTRACT_INVALID);
    }

    /* ---- commit: the first and only mutation of the world ---------------- */

    /* WorldSetCell is the ordinary write path, so clearing goes through the
       same wake and dirty bookkeeping every other world mutation uses: the
       chunks that held these cells are scheduled, their pixels are marked for
       rebuild, and their light inputs are marked stale. Nothing here reaches
       for the renderer. */
    for (index = 0; index < component.cellCount; ++index) {
        WorldSetCell(world, (int)workspace->cellX[index],
                     (int)workspace->cellY[index], MATERIAL_EMPTY);
    }

    /* Local (0, 0) is the component's bounding-box corner, so the transform
       from a world cell to a body cell is a subtraction. The body's origin is
       its centre of mass, which is the point a rigid body rotates about, so
       nothing about this representation has to change when EF-DYN-004 gives
       bodies motion:

           world = position + rotate(local + (0.5, 0.5) - centerOfMass, angle)
    */
    body->sourceX = component.minimumX;
    body->sourceY = component.minimumY;
    DynamicTerrainFinalizeBody(terrain, handle);
    body->position = (Vector2){(float)component.minimumX + body->centerOfMass.x,
                               (float)component.minimumY + body->centerOfMass.y};
    body->angle = 0.0f;
    body->velocity = (Vector2){0.0f, 0.0f};
    body->angularVelocity = 0.0f;

    ++terrain->stats.extractionsSucceeded;

    result.status = TERRAIN_EXTRACT_OK;
    result.body = handle;
    result.cellCount = component.cellCount;
    return result;
}
