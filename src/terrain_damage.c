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
    /* A hundred and forty cells a second taken away in one contact: a slab
       dropped from a house's height, or thrown by a blast into a wall. A
       slab merely dropped from a hand lands well under it. */
    config.fractureSpeed = 140.0f;
    config.fractureMinimumCells = 48;
    config.fracturesPerStep = 2;
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
        TerrainDamageStats empty = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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
    /* A body with a new shape has to be judged again: what was balanced may
       now topple, and what rested on the part that is gone has to fall. If the
       awake budget refuses, the body stays where it is, which is the same
       answer the budget gives everywhere else. */
    (void)DynamicTerrainWakeBody(terrain, handle);
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
            /* The rest stays unlabelled, and so stays with the body. */
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
            DynamicTerrainSetShade(terrain, handle, localX, localY,
                                   DynamicTerrainShadeAt(terrain, parentHandle,
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
    bool refused = false;
    int label;
    int index;

    if (system == NULL || body == NULL || body->cellCount <= 0) {
        return 0;
    }
    ++system->stats.fractureChecks;
    memset(sizes, 0, sizeof(sizes));
    components = TerrainDamageLabelComponents(system, terrain, handle, body,
                                              sizes);
    if (components < 0) {
        /* More pieces than one pass tracks: the pieces it did label are
           split off now, and the body is asked again for the rest. */
        components = TERRAIN_FRACTURE_MAX_COMPONENTS;
        refused = true;
    }
    if (components <= 1) {
        body->fracturePending = false;
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
            refused = true;
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
    /* The body may have been refinalized, but it is the same slot. */
    body = DynamicTerrainGet(terrain, handle);
    if (body != NULL) {
        body->fracturePending = refused;
    }
    return created;
}

static void TerrainDamageDriveHeat(TerrainDamageSystem *system,
                                   DynamicTerrainSystem *terrain,
                                   TerrainBodyHandle handle, Vector2 worldCentre,
                                   float radius, float strength, bool lower);

void TerrainDamageHeatAround(TerrainDamageSystem *system,
                             DynamicTerrainSystem *terrain,
                             TerrainBodyHandle handle, Vector2 worldCentre,
                             float radius, float strength)
{
    TerrainDamageDriveHeat(system, terrain, handle, worldCentre, radius, strength,
                           false);
}

void TerrainDamageTemperAround(TerrainDamageSystem *system,
                               DynamicTerrainSystem *terrain,
                               TerrainBodyHandle handle, Vector2 worldCentre,
                               float radius, float strength)
{
    TerrainDamageDriveHeat(system, terrain, handle, worldCentre, radius, strength,
                           true);
}

static void TerrainDamageDriveHeat(TerrainDamageSystem *system,
                                   DynamicTerrainSystem *terrain,
                                   TerrainBodyHandle handle, Vector2 worldCentre,
                                   float radius, float strength, bool lower)
{
    TerrainBody *body = DynamicTerrainGet(terrain, handle);
    Vector2 local;
    int firstX;
    int firstY;
    int lastX;
    int lastY;
    int localY;

    if (system == NULL || body == NULL || !(strength >= 0.0f) ||
        !(radius > 0.0f) || (!lower && !(strength > 0.0f))) {
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
            } else if (lower && target < DynamicTerrainTemperatureAt(terrain, handle,
                                                                     localX, localY)) {
                /* Tempering: the face cools toward what the air asks now, a
                   step at a time, so a slab out of the corridor fades rather
                   than snapping cold. */
                float current = DynamicTerrainTemperatureAt(terrain, handle,
                                                            localX, localY);
                float next = current - (current - target) * 0.35f;

                if (next < 1.0f) next = 0.0f;
                DynamicTerrainSetCell(terrain, handle, localX, localY, material,
                                      next);
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

int TerrainDamageCrack(TerrainDamageSystem *system, DynamicTerrainSystem *terrain,
                       TerrainBodyHandle handle, Vector2 worldPoint,
                       Vector2 direction, float length)
{
    TerrainBody *body = DynamicTerrainGet(terrain, handle);
    Vector2 previousCentre;
    float previousAngle;
    Vector2 local;
    Vector2 localDirection;
    float cosine;
    float sine;
    float travelled;
    int removed = 0;
    uint32_t wobble;

    if (system == NULL || body == NULL || !(length > 0.0f) ||
        !TerrainFiniteSample(worldPoint) || !TerrainFiniteSample(direction)) {
        return 0;
    }
    previousCentre = body->centerOfMass;
    previousAngle = body->angle;
    /* The crack runs in the body's frame, so a body that is turning is
       cracked along the same line of its own material whatever its angle. */
    local = TerrainBodyWorldToLocal(body, worldPoint.x, worldPoint.y);
    cosine = cosf(-body->angle);
    sine = sinf(-body->angle);
    localDirection.x = direction.x * cosine - direction.y * sine;
    localDirection.y = direction.x * sine + direction.y * cosine;
    /* Deterministic, from the body's own numbers: the same blow on the same
       slab cracks it the same way. */
    wobble = (uint32_t)body->generation * 0x9e3779b9u ^ (uint32_t)body->cellCount;

    {
        int previousX = 0;
        int previousY = 0;
        bool first = true;

        for (travelled = 0.0f; travelled <= length; travelled += 0.7f) {
            float side;
            int cellX;
            int cellY;

            wobble ^= wobble << 13;
            wobble ^= wobble >> 17;
            wobble ^= wobble << 5;
            side = ((float)(wobble & 0xffu) / 255.0f - 0.5f) * 1.6f;
            cellX = (int)floorf(local.x + localDirection.x * travelled -
                                localDirection.y * side);
            cellY = (int)floorf(local.y + localDirection.y * travelled +
                                localDirection.x * side);
            /* A crack that steps diagonally leaves the two cells across the
               corner touching, and touching is connected: the fracture pass
               would find one piece. The corner cell goes too. */
            if (!first && cellX != previousX && cellY != previousY &&
                DynamicTerrainCellAt(terrain, handle, previousX, cellY) !=
                    MATERIAL_EMPTY) {
                DynamicTerrainSetCell(terrain, handle, previousX, cellY,
                                      MATERIAL_EMPTY, 0.0f);
                ++removed;
            }
            first = false;
            previousX = cellX;
            previousY = cellY;
            if (DynamicTerrainCellAt(terrain, handle, cellX, cellY) == MATERIAL_EMPTY) {
                continue;
            }
            DynamicTerrainSetCell(terrain, handle, cellX, cellY, MATERIAL_EMPTY, 0.0f);
            ++removed;
        }
    }
    if (removed == 0) {
        return 0;
    }
    system->stats.cellsCarved += removed;
    if (!TerrainDamageRefinalize(terrain, handle, previousCentre, previousAngle)) {
        ++system->stats.bodiesEmptied;
        return 0;
    }
    return TerrainDamageFracture(system, terrain, handle);
}

int TerrainDamageImpactFractures(TerrainDamageSystem *system,
                                 DynamicTerrainSystem *terrain)
{
    int cracked = 0;
    int slot;

    if (system == NULL || terrain == NULL || terrain->material == NULL) {
        return 0;
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &terrain->bodies[slot];
        float stopped;

        if (!body->active || body->cellCount < system->config.fractureMinimumCells ||
            !(body->mass > 0.0f) || !(body->impactImpulse > 0.0f)) {
            continue;
        }
        stopped = body->impactImpulse / body->mass;
        if (stopped < system->config.fractureSpeed) {
            continue;
        }
        if (cracked >= system->config.fracturesPerStep) {
            /* The impulse is kept for the next step's look, so a blow that
               waited is not a blow that never happened. */
            ++system->stats.impactCracksDeferred;
            continue;
        }
        (void)TerrainDamageCrack(system, terrain,
                                 (TerrainBodyHandle){(uint16_t)slot, body->generation},
                                 body->impactPoint, body->impactNormal,
                                 body->boundingRadius * 2.0f + 2.0f);
        body->impactImpulse = 0.0f;
        ++system->stats.impactCracks;
        ++cracked;
    }
    /* One body left in pieces by a refused split is asked again, when there
       is a slot to give a piece to. One a step: a labelling is a walk over
       the whole raster, and a session with every slot taken would otherwise
       walk every pending body every step for nothing. */
    if (terrain->stats.activeBodies < MAX_TERRAIN_BODIES) {
        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            TerrainBody *body = &terrain->bodies[slot];

            if (!body->active || !body->fracturePending) {
                continue;
            }
            ++system->stats.fractureRetries;
            (void)TerrainDamageFracture(system, terrain,
                                        (TerrainBodyHandle){(uint16_t)slot,
                                                            body->generation});
            break;
        }
    }
    return cracked;
}
