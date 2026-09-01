/* Carving and fracture. See terrain_damage.h for the contract; this file
 * records the two corrections that make the difference between a body that
 * loses material and a body that teleports.
 *
 * A body's `position` is the world location of its centre of mass. Remove some
 * of its cells and the centre of mass moves inside the raster, so leaving
 * `position` alone would drag every remaining cell across the world by exactly
 * that shift. Both operations here therefore end with:
 *
 *     position += rotate(newCentreOfMass - oldCentreOfMass, angle)
 *     velocity += cross(angularVelocity, newCentreWorld - oldCentreWorld)
 *
 * The first keeps the cells where they were. The second keeps the *motion*
 * where it was: a point of a spinning body that is not the centre of mass is
 * already moving, and a piece whose centre lands there has to leave with the
 * speed that point already had, or a fractured slab would suddenly orbit its
 * own missing half.
 */
#include "terrain_damage.h"

#include <math.h>

#include "materials.h"
#include <stddef.h>
#include <string.h>

TerrainDamageConfig TerrainDamageDefaultConfig(void)
{
    TerrainDamageConfig config;

    /* Six cells is about where a piece stops reading as a chip and starts
       reading as a chunk worth tumbling on its own. */
    config.minimumFractureCells = 6;
    /* Faster bites and a wider one than the first pass shipped with: the beam
       was cutting so slowly that a player could not tell it was working on
       rock at all. Still rate-limited, because a bite every frame evaporates a
       slab in well under a second. */
    config.beamCutInterval = 0.035f;
    config.beamCutRadius = 1.9f;
    return config;
}

void TerrainDamageInit(TerrainDamageSystem *system)
{
    if (system == NULL) {
        return;
    }
    system->config = TerrainDamageDefaultConfig();
    system->beamCooldown = 0.0f;
    TerrainDamageResetStats(system);
}

void TerrainDamageResetStats(TerrainDamageSystem *system)
{
    if (system == NULL) {
        return;
    }
    {
        TerrainDamageStats empty = {0, 0, 0, 0, 0, 0, 0, 0};

        system->stats = empty;
    }
}

bool TerrainDamageBeamReady(TerrainDamageSystem *system, float deltaTime)
{
    if (system == NULL || !(deltaTime >= 0.0f)) {
        return false;
    }
    system->beamCooldown -= deltaTime;
    if (system->beamCooldown > 0.0f) {
        return false;
    }
    system->beamCooldown = system->config.beamCutInterval;
    return true;
}

/* Re-derives the body from its raster and moves it so nothing appears to have
   happened to the cells that are still there. Frees a body with nothing left.
   Returns false when the body is gone. */
static bool TerrainDamageRefinalize(DynamicTerrainSystem *terrain,
                                    TerrainBodyHandle handle,
                                    Vector2 previousCentre, float previousAngle)
{
    TerrainBody *body = DynamicTerrainGet(terrain, handle);
    Vector2 shift;
    float cosine;
    float sine;
    Vector2 movedWorld;

    if (body == NULL) {
        return false;
    }
    DynamicTerrainFinalizeBody(terrain, handle);
    if (body->cellCount <= 0) {
        DynamicTerrainFreeBody(terrain, handle);
        return false;
    }

    shift.x = body->centerOfMass.x - previousCentre.x;
    shift.y = body->centerOfMass.y - previousCentre.y;
    cosine = cosf(previousAngle);
    sine = sinf(previousAngle);
    movedWorld.x = shift.x * cosine - shift.y * sine;
    movedWorld.y = shift.x * sine + shift.y * cosine;

    body->position.x += movedWorld.x;
    body->position.y += movedWorld.y;
    /* The velocity of the point the centre of mass moved to, which is what the
       body's linear velocity now has to mean. */
    body->velocity.x += -body->angularVelocity * movedWorld.y;
    body->velocity.y += body->angularVelocity * movedWorld.x;
    return true;
}

int TerrainDamageCarveCircle(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius)
{
    TerrainBody *body = DynamicTerrainGet(terrain, handle);
    Vector2 local;
    Vector2 previousCentre;
    float previousAngle;
    float radiusSquared;
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    int removed = 0;
    int localY;

    if (system == NULL || body == NULL || !(radius > 0.0f) ||
        !TerrainFiniteSample(worldCentre)) {
        return 0;
    }
    ++system->stats.carveCalls;
    previousCentre = body->centerOfMass;
    previousAngle = body->angle;

    /* Rotation preserves distance, so a circle in the world is the same circle
       in the body's frame and the whole test can be done in local coordinates
       with one transform instead of one per cell. */
    local = TerrainBodyWorldToLocal(body, worldCentre.x, worldCentre.y);
    radiusSquared = radius * radius;
    firstX = (int)floorf(local.x - radius);
    firstY = (int)floorf(local.y - radius);
    lastX = (int)ceilf(local.x + radius);
    lastY = (int)ceilf(local.y + radius);
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > body->width - 1) lastX = body->width - 1;
    if (lastY > body->height - 1) lastY = body->height - 1;

    for (localY = firstY; localY <= lastY; ++localY) {
        int localX;

        for (localX = firstX; localX <= lastX; ++localX) {
            /* Cell centres, the same convention the transform documents. */
            float dx = (float)localX + 0.5f - local.x;
            float dy = (float)localY + 0.5f - local.y;

            if (dx * dx + dy * dy > radiusSquared) {
                continue;
            }
            if (DynamicTerrainCellAt(terrain, handle, localX, localY) ==
                MATERIAL_EMPTY) {
                continue;
            }
            DynamicTerrainSetCell(terrain, handle, localX, localY,
                                  MATERIAL_EMPTY, 0.0f);
            ++removed;
        }
    }
    if (removed == 0) {
        return 0;
    }
    system->stats.cellsCarved += removed;
    if (!TerrainDamageRefinalize(terrain, handle, previousCentre, previousAngle)) {
        ++system->stats.bodiesEmptied;
    }
    return removed;
}

/* --- fracture ------------------------------------------------------------ */

/* Labels every occupied cell with the piece it belongs to, breadth first from
   the first unlabelled cell in row-major order. That order is the whole of the
   determinism story: the same raster always yields the same pieces with the
   same numbers, whatever else the frame did. Returns the number of pieces, or
   -1 when there are more than the pass will track. */
static int TerrainDamageLabelComponents(TerrainDamageSystem *system,
                                        const DynamicTerrainSystem *terrain,
                                        TerrainBodyHandle handle,
                                        const TerrainBody *body,
                                        int *sizes)
{
    int cells = body->width * body->height;
    int count = 0;
    int index;

    memset(system->component, 0, (size_t)cells * sizeof(*system->component));
    for (index = 0; index < cells; ++index) {
        int seedX = index % body->width;
        int seedY = index / body->width;
        int head = 0;
        int tail = 0;
        int size = 0;

        if (system->component[index] != 0u ||
            DynamicTerrainCellAt(terrain, handle, seedX, seedY) == MATERIAL_EMPTY) {
            continue;
        }
        if (count >= TERRAIN_FRACTURE_MAX_COMPONENTS) {
            return -1;
        }
        ++count;
        system->component[index] = (uint8_t)count;
        system->queue[tail++] = (uint16_t)index;
        while (head < tail) {
            static const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            int current = (int)system->queue[head++];
            int x = current % body->width;
            int y = current / body->width;
            int i;

            ++size;
            for (i = 0; i < 4; ++i) {
                int neighbourX = x + offsets[i][0];
                int neighbourY = y + offsets[i][1];
                int neighbour;

                if (neighbourX < 0 || neighbourY < 0 ||
                    neighbourX >= body->width || neighbourY >= body->height) {
                    continue;
                }
                neighbour = neighbourY * body->width + neighbourX;
                if (system->component[neighbour] != 0u) {
                    continue;
                }
                if (DynamicTerrainCellAt(terrain, handle, neighbourX,
                                         neighbourY) == MATERIAL_EMPTY) {
                    continue;
                }
                system->component[neighbour] = (uint8_t)count;
                system->queue[tail++] = (uint16_t)neighbour;
            }
        }
        sizes[count - 1] = size;
    }
    return count;
}

/* Copies one labelled piece into a body of its own, placed and moving exactly
   as that part of the parent already was. Returns false when no body could be
   allocated, which leaves the piece where it is. */
static bool TerrainDamageSpawnPiece(DynamicTerrainSystem *terrain,
                                    TerrainBodyHandle parentHandle,
                                    const TerrainBody *parent, int label,
                                    const uint8_t *component,
                                    TerrainBodyHandle *created)
{
    TerrainBodyHandle handle;
    TerrainBody *piece;
    Vector2 origin;
    Vector2 centreWorld;
    float leverX;
    float leverY;
    int localY;

    /* Same raster dimensions as the parent, so a cell keeps its local
       coordinates and the copy is a straight transfer. Slack costs nothing: the
       arena slot is a fixed size whatever the body puts in it. */
    handle = DynamicTerrainAllocBody(terrain, parent->width, parent->height);
    piece = DynamicTerrainGet(terrain, handle);
    if (piece == NULL) {
        return false;
    }
    for (localY = 0; localY < parent->height; ++localY) {
        int localX;

        for (localX = 0; localX < parent->width; ++localX) {
            if (component[localY * parent->width + localX] != (uint8_t)label) {
                continue;
            }
            DynamicTerrainSetCell(terrain, handle, localX, localY,
                                  DynamicTerrainCellAt(terrain, parentHandle,
                                                       localX, localY),
                                  DynamicTerrainTemperatureAt(terrain,
                                                              parentHandle,
                                                              localX, localY));
        }
    }
    DynamicTerrainFinalizeBody(terrain, handle);
    if (piece->cellCount <= 0) {
        DynamicTerrainFreeBody(terrain, handle);
        return false;
    }

    /* Where the piece's own centre of mass sits in the parent's world, which is
       exactly where the new body has to be for nothing to appear to move. */
    piece->angle = parent->angle;
    origin = TerrainBodyLocalToWorld(parent, piece->centerOfMass.x,
                                     piece->centerOfMass.y);
    piece->position = origin;
    piece->sourceX = parent->sourceX;
    piece->sourceY = parent->sourceY;

    /* The parent's motion, read at the point this piece is leaving from. */
    centreWorld = TerrainBodyLocalToWorld(parent, parent->centerOfMass.x,
                                          parent->centerOfMass.y);
    leverX = origin.x - centreWorld.x;
    leverY = origin.y - centreWorld.y;
    piece->angularVelocity = parent->angularVelocity;
    piece->velocity.x = parent->velocity.x - parent->angularVelocity * leverY;
    piece->velocity.y = parent->velocity.y + parent->angularVelocity * leverX;

    *created = handle;
    return true;
}

int TerrainDamageFracture(TerrainDamageSystem *system,
                          DynamicTerrainSystem *terrain,
                          TerrainBodyHandle handle)
{
    TerrainBody *body = DynamicTerrainGet(terrain, handle);
    TerrainBody snapshot;
    int sizes[TERRAIN_FRACTURE_MAX_COMPONENTS];
    Vector2 previousCentre;
    float previousAngle;
    int components;
    int largest = 1;
    int created = 0;
    bool detached = false;
    int label;
    int index;

    if (system == NULL || body == NULL || body->cellCount <= 0) {
        return 0;
    }
    ++system->stats.fractureChecks;
    memset(sizes, 0, sizeof(sizes));
    components = TerrainDamageLabelComponents(system, terrain, handle, body,
                                              sizes);
    if (components <= 1) {
        return 0;
    }

    /* The largest piece keeps the slot. A caller holding this handle — the
       player dragging the slab, the renderer's cache — keeps naming the piece
       it would recognise as the one it had. Ties go to the lower label, which
       is the one whose first cell comes earlier in row-major order. */
    for (label = 2; label <= components; ++label) {
        if (sizes[label - 1] > sizes[largest - 1]) {
            largest = label;
        }
    }

    /* A copy, because spawning pieces mutates the parent's raster and the
       placement of every piece has to be read from the body as it was. */
    snapshot = *body;
    previousCentre = body->centerOfMass;
    previousAngle = body->angle;

    for (label = 1; label <= components; ++label) {
        TerrainBodyHandle piece;

        if (label == largest) {
            continue;
        }
        if (sizes[label - 1] < system->config.minimumFractureCells) {
            ++system->stats.fragmentsTooSmall;
        } else if (TerrainDamageSpawnPiece(terrain, handle, &snapshot, label,
                                           system->component, &piece)) {
            ++created;
            ++system->stats.fragmentsCreated;
            (void)piece;
        } else {
            /* No slot or no cell budget. The piece stays part of the body it
               was already part of: nothing is lost, nothing moves, and the
               world is exactly as valid as it was a moment ago. */
            ++system->stats.fragmentsRefusedByBudget;
            continue;
        }
        /* Whether it became a body or was too small to be worth one, these
           cells are no longer part of the parent. */
        detached = true;
        for (index = 0; index < snapshot.width * snapshot.height; ++index) {
            if (system->component[index] != (uint8_t)label) {
                continue;
            }
            DynamicTerrainSetCell(terrain, handle, index % snapshot.width,
                                  index / snapshot.width, MATERIAL_EMPTY, 0.0f);
        }
    }

    if (detached) {
        (void)TerrainDamageRefinalize(terrain, handle, previousCentre,
                                      previousAngle);
    }
    if (created > 0) {
        ++system->stats.fractureSplits;
    }
    return created;
}

void TerrainDamageHeatAround(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius, float strength)
{
    TerrainBody *body = DynamicTerrainGet(terrain, handle);
    Vector2 local;
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    int localY;

    if (system == NULL || body == NULL || !(strength > 0.0f) ||
        !(radius > 0.0f)) {
        return;
    }
    /* Local space, for the same reason the carve uses it: rotation preserves
       distance, so the ring around the cut is the same ring in either frame. */
    local = TerrainBodyWorldToLocal(body, worldCentre.x, worldCentre.y);
    firstX = (int)floorf(local.x - radius) - 3;
    firstY = (int)floorf(local.y - radius) - 3;
    lastX = (int)ceilf(local.x + radius) + 3;
    lastY = (int)ceilf(local.y + radius) + 3;
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > body->width - 1) lastX = body->width - 1;
    if (lastY > body->height - 1) lastY = body->height - 1;

    for (localY = firstY; localY <= lastY; ++localY) {
        int localX;

        for (localX = firstX; localX <= lastX; ++localX) {
            CellMaterial material = DynamicTerrainCellAt(terrain, handle, localX,
                                                         localY);
            const MaterialInfo *info = MaterialAt(material);
            float dx = (float)localX + 0.5f - local.x;
            float dy = (float)localY + 0.5f - local.y;
            float distance = sqrtf(dx * dx + dy * dy);
            float band;
            float target;

            if (material == MATERIAL_EMPTY || !info->onHeat.enabled ||
                info->onHeat.threshold <= 60.0f || distance > radius + 3.0f) {
                continue;
            }
            band = 1.0f - (distance - radius) / 3.0f;
            if (band > 1.0f) band = 1.0f;
            if (band <= 0.0f) continue;
            target = info->onHeat.threshold * strength * band;
            if (target > DynamicTerrainTemperatureAt(terrain, handle, localX,
                                                     localY)) {
                DynamicTerrainSetCell(terrain, handle, localX, localY, material,
                                      target);
            }
        }
    }
}

int TerrainDamageApplyCircle(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius)
{
    int removed = TerrainDamageCarveCircle(system, terrain, handle, worldCentre,
                                           radius);

    /* Only after a cut: connectivity cannot change without one, and a scan on
       every tick of every body is exactly the cost this design refuses. */
    if (removed > 0) {
        (void)TerrainDamageFracture(system, terrain, handle);
    }
    return removed;
}
