#ifndef TERRAIN_EXTRACTION_H
#define TERRAIN_EXTRACTION_H

/* Moving a proven-free piece of terrain out of the cellular world and into a
 * TerrainBody.
 *
 * This is the first thing that joins the two halves built so far: the bounded
 * component detector, which can prove a lump of terrain is attached to nothing,
 * and the body store, which can hold one. It lives in its own module so that
 * neither of them has to learn about the other — `DynamicTerrainSystem` still
 * never receives a `World`, and the world module still knows nothing about
 * bodies.
 *
 * The guarantee that matters is atomicity: an extraction either completes, or
 * the world is left exactly as it was. That is structural rather than
 * compensating. Every check that can fail runs before the first cell is
 * cleared, and the body's own raster doubles as the staging area, so a failure
 * is a free of a body nobody has seen yet rather than an undo of half a
 * mutation.
 *
 * Nothing calls this automatically. Wiring it to explosions and the drill is
 * EF-DYN-011; until then it is driven by tests.
 */

#include <stdbool.h>

#include "dynamic_terrain.h"
#include "world.h"
#include "world_components.h"

typedef enum TerrainExtractStatus {
    TERRAIN_EXTRACT_OK = 0,
    /* The detector did not prove the component free. Anchored, unknown,
       too-large and invalid all land here: only WORLD_COMPONENT_DETACHED may
       be torn out, and that conservatism is the whole point of the detector. */
    TERRAIN_EXTRACT_NOT_DETACHED,
    /* Every body slot is in use. */
    TERRAIN_EXTRACT_NO_BODY_SLOT,
    /* The component's bounding box does not fit a body's raster, or it holds
       more cells than a body may. */
    TERRAIN_EXTRACT_CELL_CAPACITY,
    /* The world no longer matches what the detector saw: at least one recorded
       cell has stopped being solid. Re-run the detector. */
    TERRAIN_EXTRACT_WORLD_CHANGED,
    /* Malformed request: missing world, system or workspace, or a component
       whose bounds and cell count do not describe anything real. */
    TERRAIN_EXTRACT_INVALID
} TerrainExtractStatus;

typedef struct TerrainExtractResult {
    TerrainExtractStatus status;
    /* Valid only when `status` is TERRAIN_EXTRACT_OK. Every other outcome
       returns the invalid handle and leaves no body allocated. */
    TerrainBodyHandle body;
    int cellCount;
} TerrainExtractResult;

/* Moves `component` out of `world` and into a new body.
 *
 * `component` and `workspace` must come from the same WorldFindComponent call:
 * the result carries the bounds and the count, the workspace carries the cell
 * coordinates. On success the listed cells are cleared from the world, the
 * chunks that held them are woken and marked dirty through the ordinary write
 * path, and the body is finalised with its bounds, mass, centre of mass and
 * inertia.
 *
 * On every failure the world is untouched, byte for byte.
 *
 * Cost is O(component cells) and allocates nothing. */
TerrainExtractResult TerrainExtractComponent(World *world,
                                             DynamicTerrainSystem *terrain,
                                             const WorldComponentWorkspace *workspace,
                                             WorldComponentResult component);

const char *TerrainExtractStatusName(TerrainExtractStatus status);

#endif
