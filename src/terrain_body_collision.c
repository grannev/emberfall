/* Terrain bodies against each other: the narrow phase and the wake rules.
 * See terrain_body_collision.h for the shape of the work; this file records
 * the decisions inside it.
 *
 * A contact is found by carrying one body's surface cell centre into the other
 * body's frame and asking its raster one question. The contact normal is then
 * the way out of the cell that was entered: of its four faces, the nearest one
 * that does not lead into more of the same body. That is the world narrow
 * phase's rule applied to a raster instead of the world, and it is what makes
 * a flat face behave as a face rather than pushing along it.
 */
#include "terrain_body_collision.h"

#include <math.h>
#include <stddef.h>

#include "dynamic_terrain.h"

/* Bounding boxes are grown by this before the pair test, so two bodies that
   are about to touch in this substep are found by it. */
#define TERRAIN_PAIR_BOUNDS_MARGIN 1.0f

/* The world box of every live body, once. A box needs the four rotated
   corners of the raster's extent, and the pair test below asks about two
   thousand pairs: asking each pair to rebuild both boxes was most of what the
   pair phase cost in a scene where nothing touched. */
static void TerrainPairCacheBounds(TerrainContactWorkspace *workspace,
                                   const DynamicTerrainSystem *system)
{
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *body = &system->bodies[slot];

        workspace->boundsValid[slot] =
            body->active && body->cellCount > 0 &&
            TerrainBodyWorldBounds(body, &workspace->boundsMinimum[slot],
                                   &workspace->boundsMaximum[slot]);
    }
}

static bool TerrainPairBoundsTouch(const TerrainContactWorkspace *workspace,
                                   int a, int b)
{
    if (!workspace->boundsValid[a] || !workspace->boundsValid[b]) {
        return false;
    }
    return workspace->boundsMinimum[a].x - TERRAIN_PAIR_BOUNDS_MARGIN <=
               workspace->boundsMaximum[b].x &&
           workspace->boundsMaximum[a].x + TERRAIN_PAIR_BOUNDS_MARGIN >=
               workspace->boundsMinimum[b].x &&
           workspace->boundsMinimum[a].y - TERRAIN_PAIR_BOUNDS_MARGIN <=
               workspace->boundsMaximum[b].y &&
           workspace->boundsMaximum[a].y + TERRAIN_PAIR_BOUNDS_MARGIN >=
               workspace->boundsMinimum[b].y;
}

static Vector2 TerrainPairPointVelocity(const TerrainBody *body, Vector2 point)
{
    float leverX = point.x - body->position.x;
    float leverY = point.y - body->position.y;

    return (Vector2){body->velocity.x - body->angularVelocity * leverY,
                     body->velocity.y + body->angularVelocity * leverX};
}

static bool TerrainPairCellOccupied(const DynamicTerrainSystem *system, int slot,
                                    const TerrainBody *body, int localX,
                                    int localY)
{
    size_t base;

    if (localX < 0 || localY < 0 || localX >= body->width ||
        localY >= body->height) {
        return false;
    }
    base = TerrainSlotRasterBase(slot);
    return system->material[base + (size_t)localY * (size_t)body->width +
                            (size_t)localX] != (uint8_t)MATERIAL_EMPTY;
}

/* The way out of an occupied raster cell for a point inside it: the nearest
   face that does not open onto another occupied cell. Returns false when the
   point is buried, in which case the sample is dropped and the others
   decide. Both outputs are in the raster body's local frame. */
static bool TerrainPairResolveSample(const DynamicTerrainSystem *system, int slot,
                                     const TerrainBody *body, Vector2 local,
                                     int cellX, int cellY, Vector2 *normal,
                                     float *penetration, int *face)
{
    static const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    float depths[4];
    int best = -1;
    int i;

    depths[0] = local.y - (float)cellY;
    depths[1] = (float)(cellX + 1) - local.x;
    depths[2] = (float)(cellY + 1) - local.y;
    depths[3] = local.x - (float)cellX;
    for (i = 0; i < 4; ++i) {
        if (TerrainPairCellOccupied(system, slot, body, cellX + offsets[i][0],
                                    cellY + offsets[i][1])) {
            continue;
        }
        if (best < 0 || depths[i] < depths[best]) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    *normal = (Vector2){(float)offsets[best][0], (float)offsets[best][1]};
    *penetration = depths[best];
    *face = best;
    return true;
}

/* Narrow phase for one pair: the sampler's surface cells against the raster
   body. Returns whether any contact was found. */
static bool TerrainPairSample(const TerrainContactWorkspace *workspace,
                              const DynamicTerrainSystem *system, int samplerSlot,
                              int rasterSlot, TerrainPairManifold *manifold)
{
    const TerrainBody *sampler = &system->bodies[samplerSlot];
    const TerrainBody *raster = &system->bodies[rasterSlot];
    size_t surfaceBase = TerrainSlotSurfaceBase(samplerSlot);
    float samplerCosine = cosf(sampler->angle);
    float samplerSine = sinf(sampler->angle);
    float rasterCosine = cosf(raster->angle);
    float rasterSine = sinf(raster->angle);
    Vector2 rasterMinimum;
    Vector2 rasterMaximum;
    int stride;
    int index;

    manifold->sampler = samplerSlot;
    manifold->raster = rasterSlot;
    TerrainContactClear(manifold->contacts);
    manifold->count = 0;
    manifold->deepest = 0.0f;
    rasterMinimum = workspace->boundsMinimum[rasterSlot];
    rasterMaximum = workspace->boundsMaximum[rasterSlot];
    stride = sampler->surfaceCount / TERRAIN_PAIR_MAX_SAMPLES + 1;

    for (index = 0; index < sampler->surfaceCount; index += stride) {
        float localX = (float)system->surfaceX[surfaceBase + (size_t)index] + 0.5f;
        float localY = (float)system->surfaceY[surfaceBase + (size_t)index] + 0.5f;
        float offsetX = localX - sampler->centerOfMass.x;
        float offsetY = localY - sampler->centerOfMass.y;
        Vector2 world;
        Vector2 local;
        Vector2 localNormal;
        Vector2 normal;
        float penetration;
        int face;
        int cellX;
        int cellY;

        world.x = sampler->position.x + offsetX * samplerCosine -
                  offsetY * samplerSine;
        world.y = sampler->position.y + offsetX * samplerSine +
                  offsetY * samplerCosine;
        /* Most surface cells of a body lie nowhere near the other body; the
           box says so before the second transform. */
        if (world.x < rasterMinimum.x || world.x > rasterMaximum.x ||
            world.y < rasterMinimum.y || world.y > rasterMaximum.y) {
            continue;
        }
        offsetX = world.x - raster->position.x;
        offsetY = world.y - raster->position.y;
        local.x = raster->centerOfMass.x + offsetX * rasterCosine +
                  offsetY * rasterSine;
        local.y = raster->centerOfMass.y - offsetX * rasterSine +
                  offsetY * rasterCosine;
        if (!TerrainFiniteSample(local)) {
            continue;
        }
        cellX = (int)floorf(local.x);
        cellY = (int)floorf(local.y);
        if (!TerrainPairCellOccupied(system, rasterSlot, raster, cellX, cellY)) {
            continue;
        }
        if (!TerrainPairResolveSample(system, rasterSlot, raster, local, cellX,
                                      cellY, &localNormal, &penetration, &face)) {
            continue;
        }
        /* A direction out of the raster body, rotated into the world. */
        normal.x = localNormal.x * rasterCosine - localNormal.y * rasterSine;
        normal.y = localNormal.x * rasterSine + localNormal.y * rasterCosine;
        {
            Vector2 samplerVelocity = TerrainPairPointVelocity(sampler, world);
            Vector2 rasterVelocity = TerrainPairPointVelocity(raster, world);
            float approach = (samplerVelocity.x - rasterVelocity.x) * normal.x +
                             (samplerVelocity.y - rasterVelocity.y) * normal.y;

            TerrainContactAdd(manifold->contacts, &manifold->count,
                              &manifold->deepest, face, index, world, normal,
                              penetration, approach);
        }
    }
    return manifold->count > 0;
}

/* Which of a pair does the sampling: the body with the fewer surface cells,
   which is the cheaper narrow phase and finds a small body entering a large
   one, or a large one landing on a small one, equally well. Ties go to the
   lower slot, which keeps the choice deterministic. */
static void TerrainPairChooseSampler(const DynamicTerrainSystem *system, int a,
                                     int b, int *sampler, int *raster)
{
    if (system->bodies[b].surfaceCount < system->bodies[a].surfaceCount) {
        *sampler = b;
        *raster = a;
    } else {
        *sampler = a;
        *raster = b;
    }
}

/* How fast the fastest point of a body is moving: what decides whether a
   sleeping body it touches has to wake. */
static float TerrainPairEdgeSpeed(const TerrainBody *body)
{
    return sqrtf(body->velocity.x * body->velocity.x +
                 body->velocity.y * body->velocity.y) +
           fabsf(body->angularVelocity) * body->boundingRadius;
}

/* A sleeping body whose box touches a body moving faster than the wake speed
   is woken, if the awake budget allows; otherwise it stays a wall. The rule is
   about the moving body's speed rather than the closing speed at a contact, so
   it also covers a base sliding out from under what rests on it: a sleeper
   that is being left behind has to fall, not hang. Below the wake speed the
   sleeper is not disturbed, which is what lets a pile settle from the bottom
   up while the pieces above are still creeping. */
static void TerrainPairWakeTouched(TerrainContactWorkspace *workspace,
                                   DynamicTerrainSystem *system, int awakeSlot,
                                   int sleepingSlot)
{
    TerrainBody *sleeper = &system->bodies[sleepingSlot];

    if (TerrainPairEdgeSpeed(&system->bodies[awakeSlot]) < TERRAIN_PAIR_WAKE_SPEED) {
        return;
    }
    if (DynamicTerrainWakeBody(system, (TerrainBodyHandle){
                                           (uint16_t)sleepingSlot,
                                           sleeper->generation})) {
        ++workspace->stats.bodiesWokenByContact;
    }
}

void TerrainPairCollect(TerrainContactWorkspace *workspace,
                        DynamicTerrainSystem *system)
{
    int a;

    if (workspace == NULL || system == NULL || system->material == NULL) {
        return;
    }
    workspace->manifoldCount = 0;
    TerrainPairCacheBounds(workspace, system);

    for (a = 0; a < MAX_TERRAIN_BODIES; ++a) {
        const TerrainBody *bodyA = &system->bodies[a];
        int b;

        if (!workspace->boundsValid[a]) {
            continue;
        }
        for (b = a + 1; b < MAX_TERRAIN_BODIES; ++b) {
            const TerrainBody *bodyB = &system->bodies[b];
            TerrainPairManifold *manifold;
            int sampler;
            int raster;

            if (!workspace->boundsValid[b]) {
                continue;
            }
            /* Two sleepers cannot have moved into each other. */
            if (!bodyA->awake && !bodyB->awake) {
                continue;
            }
            ++workspace->stats.pairsBroadPhase;
            if (!TerrainPairBoundsTouch(workspace, a, b)) {
                continue;
            }
            /* Woken before the narrow phase, so the solve sees the struck
               body's real mass rather than a wall. */
            if (!bodyA->awake) {
                TerrainPairWakeTouched(workspace, system, b, a);
            } else if (!bodyB->awake) {
                TerrainPairWakeTouched(workspace, system, a, b);
            }
            ++workspace->stats.pairsNarrowPhase;
            if (workspace->manifoldCount >= TERRAIN_PAIR_MAX_MANIFOLDS) {
                ++workspace->stats.manifoldsDropped;
                continue;
            }
            manifold = &workspace->manifolds[workspace->manifoldCount];
            TerrainPairChooseSampler(system, a, b, &sampler, &raster);
            if (!TerrainPairSample(workspace, system, sampler, raster, manifold)) {
                continue;
            }
            workspace->stats.pairContacts += manifold->count;
            ++workspace->manifoldCount;
        }
    }
}

void TerrainPairWakeNeighbours(TerrainContactWorkspace *workspace,
                               DynamicTerrainSystem *system)
{
    int a;

    if (workspace == NULL || system == NULL) {
        return;
    }
    TerrainPairCacheBounds(workspace, system);
    for (a = 0; a < MAX_TERRAIN_BODIES; ++a) {
        TerrainBody *bodyA = &system->bodies[a];
        int b;

        if (!bodyA->active || !bodyA->wokeRecently) {
            continue;
        }
        /* A body that went back to sleep before this ran has nothing to
           tell its neighbours. A body without cells yet keeps its flag: it
           was allocated awake and is still being filled, and its neighbours
           are looked at once it has a shape to touch them with. */
        if (!bodyA->awake) {
            bodyA->wokeRecently = false;
            continue;
        }
        if (!workspace->boundsValid[a]) {
            continue;
        }
        bodyA->wokeRecently = false;
        for (b = 0; b < MAX_TERRAIN_BODIES; ++b) {
            TerrainBody *bodyB = &system->bodies[b];

            if (b == a || !bodyB->active || bodyB->awake) {
                continue;
            }
            if (!TerrainPairBoundsTouch(workspace, a, b)) {
                continue;
            }
            /* Woken bodies wake their own neighbours on the next update, so a
               tall pile comes awake one layer at a time rather than all at
               once, and every layer is counted. */
            if (DynamicTerrainWakeBody(system, (TerrainBodyHandle){
                                                   (uint16_t)b, bodyB->generation})) {
                ++workspace->stats.bodiesWokenByNeighbour;
            }
        }
    }
}
