/* Terrain bodies against the static world, and the order of one fixed step.
 * See terrain_physics.h for the bounds; this file records the shape of the
 * solution and why each piece is the cheap one rather than the general one.
 *
 * There is no broad phase against the world, and there is nothing for one to
 * do. A broad phase exists to avoid testing pairs that cannot touch, but the
 * world is a grid and asking it about a cell is O(1): there is no list of
 * candidate obstacles to cut down. What bounds the cost is the surface list,
 * not a spatial reject.
 *
 * Narrow phase walks only the body's surface cells — an interior cell is walled in by
 * its own body and can never make first contact — transforms each to world
 * space and asks the world one question about one cell. There is no polygon
 * clipping, no SAT and no general solver, because a rotating raster against a
 * grid of unit squares does not need any of them.
 *
 * A body cell is treated as a point at its centre rather than as a square. The
 * consequence is exact and worth stating: a body comes to rest with its cell
 * centres just outside the solid cells, so its outline overlaps the terrain by
 * up to half a cell. At one cell per pixel that is not visible, and the cheap
 * version is one world read per surface cell where the square version is four.
 * If it ever does look wrong once bodies are drawn, testing the 2x2
 * neighbourhood a body cell can span is the known upgrade — it is a cost
 * decision, not an oversight.
 *
 * The response lives in terrain_contact.c, shared with the body-body narrow
 * phase, because a pile is only stable when both are solved together.
 */
#include "terrain_physics.h"

#include <math.h>
#include <stddef.h>

#include "materials.h"
#include "terrain_body_collision.h"

bool TerrainPhysicsConfigIsSafe(const DynamicTerrainConfig *config,
                                float boundingRadius, float deltaTime)
{
    float travel;

    if (config == NULL || deltaTime <= 0.0f) {
        return false;
    }
    /* The fastest point of the body is its edge: linear speed plus what the
       spin adds at the bounding radius. */
    travel = (config->maximumSpeed +
              config->maximumAngularSpeed * boundingRadius) * deltaTime;
    return travel <= TERRAIN_COLLISION_SUBSTEP_DISTANCE *
                         (float)TERRAIN_MAX_SUBSTEPS;
}

/* True when every corner of the body's world box lies outside the map by more
   than the configured margin. Whole-body, not any-corner: a body straddling the
   border is still in play. */
static bool TerrainBodyIsLost(const TerrainBody *body, const World *world,
                              const DynamicTerrainConfig *config)
{
    Vector2 minimum;
    Vector2 maximum;

    if (!TerrainBodyWorldBounds(body, &minimum, &maximum)) {
        return false;
    }
    if (!TerrainFiniteSample(minimum) || !TerrainFiniteSample(maximum)) {
        /* A transform that has gone non-finite cannot be reasoned about and
           will never recover, so the body is lost in the way that matters. */
        return true;
    }
    /* Above and below only. Across, the world wraps and nothing can leave
       it; the game keeps every body within half a turn of the player. */
    (void)minimum.x;
    (void)maximum.x;
    return maximum.y < -config->killBoundsMargin ||
           minimum.y > (float)world->height + config->killBoundsMargin;
}

static bool TerrainWorldCellIsSolid(const World *world, int x, int y)
{
    /* Outside the map reads as rock, exactly as it does for the player and for
       every beam: the border is a wall, and a body must not sail through it. */
    return WorldMaterialIsSolid(WorldGetCell(world, x, y));
}

/* Chooses which way to push a sample out of the solid cell it landed in.
 *
 * The candidate directions are the four faces of that cell, and the natural
 * choice is the nearest one. But a body resting on a floor is nearest to a side
 * face as often as not, and pushing sideways along a floor is wrong, so a
 * direction is only taken if the cell it leads to is not itself solid. That one
 * extra question is what makes a flat floor behave like a floor. */
static bool TerrainResolveSample(const World *world, Vector2 sample, int cellX,
                                 int cellY, Vector2 *normal, float *penetration,
                                 int *face)
{
    static const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    float depths[4];
    bool blocked[4];
    int best = -1;
    int i;

    depths[0] = sample.y - (float)cellY;              /* out through the top */
    depths[1] = (float)(cellX + 1) - sample.x;        /* right */
    depths[2] = (float)(cellY + 1) - sample.y;        /* bottom */
    depths[3] = sample.x - (float)cellX;              /* left */

    for (i = 0; i < 4; ++i) {
        blocked[i] = TerrainWorldCellIsSolid(world, cellX + offsets[i][0],
                                             cellY + offsets[i][1]);
    }
    for (i = 0; i < 4; ++i) {
        if (blocked[i]) {
            continue;
        }
        if (best < 0 || depths[i] < depths[best]) {
            best = i;
        }
    }
    if (best < 0) {
        /* Buried: every way out leads into more solid. Pushing in an arbitrary
           direction would fling the body through the terrain, so the sample is
           dropped and the others decide. */
        return false;
    }
    /* `offsets[best]` already points from the cell toward the face the sample
       leaves through, which is the direction the body has to move. Negating it
       here pointed every normal into the terrain, and a body then read as
       separating at the exact moment it was sinking. */
    *normal = (Vector2){(float)offsets[best][0], (float)offsets[best][1]};
    *penetration = depths[best];
    *face = best;
    return true;
}

/* Velocity of the body at a world point, including what the spin contributes.
   In 2D the cross product of an angular velocity with a lever is
   (-w * r.y, w * r.x). */
static Vector2 TerrainPointVelocity(const TerrainBody *body, Vector2 point)
{
    float leverX = point.x - body->position.x;
    float leverY = point.y - body->position.y;

    return (Vector2){body->velocity.x - body->angularVelocity * leverY,
                     body->velocity.y + body->angularVelocity * leverX};
}

static void TerrainCollectContacts(const DynamicTerrainSystem *system,
                                   const World *world, const TerrainBody *body,
                                   int slot, TerrainContactSet *set)
{
    size_t surfaceBase = (size_t)slot * (size_t)MAX_TERRAIN_BODY_CELLS;
    float cosine = cosf(body->angle);
    float sine = sinf(body->angle);
    int index;

    TerrainContactClear(set->contacts);
    set->count = 0;
    set->deepest = 0.0f;

    for (index = 0; index < body->surfaceCount; ++index) {
        float localX = (float)system->surfaceX[surfaceBase + (size_t)index] + 0.5f;
        float localY = (float)system->surfaceY[surfaceBase + (size_t)index] + 0.5f;
        float offsetX = localX - body->centerOfMass.x;
        float offsetY = localY - body->centerOfMass.y;
        Vector2 sample;
        Vector2 normal;
        float penetration;
        int face;
        int cellX;
        int cellY;

        sample.x = body->position.x + offsetX * cosine - offsetY * sine;
        sample.y = body->position.y + offsetX * sine + offsetY * cosine;
        /* floorf before the cast: a plain truncation folds -0.4 and 0.4 onto
           the same cell, which is how a body half outside the map starts
           reading cells it never touched. */
        if (!TerrainFiniteSample(sample)) {
            continue;
        }
        cellX = (int)floorf(sample.x);
        cellY = (int)floorf(sample.y);
        if (!TerrainWorldCellIsSolid(world, cellX, cellY)) {
            continue;
        }
        if (!TerrainResolveSample(world, sample, cellX, cellY, &normal,
                                  &penetration, &face)) {
            continue;
        }
        {
            Vector2 pointVelocity = TerrainPointVelocity(body, sample);

            TerrainContactAdd(set->contacts, &set->count, &set->deepest, face,
                              index, sample, normal, penetration,
                              pointVelocity.x * normal.x +
                                  pointVelocity.y * normal.y);
        }
    }
}

/* How many substeps this body's motion needs. The fastest point of a body is
   its edge, so the spin counts as well as the travel. */
static int TerrainSubstepCount(const TerrainBody *body, float deltaTime)
{
    float speed = sqrtf(body->velocity.x * body->velocity.x +
                        body->velocity.y * body->velocity.y);
    float travel = (speed + fabsf(body->angularVelocity) * body->boundingRadius) *
                   deltaTime;
    int substeps;

    if (!(travel > TERRAIN_COLLISION_SUBSTEP_DISTANCE)) {
        return 1;
    }
    substeps = (int)ceilf(travel / TERRAIN_COLLISION_SUBSTEP_DISTANCE);
    if (substeps > TERRAIN_MAX_SUBSTEPS) {
        substeps = TERRAIN_MAX_SUBSTEPS;
    }
    return substeps;
}

/* Frees every body that has left the map. A body that has left can never touch
   anything again, so it would fall for ever, never satisfy the sleep condition,
   and hold an awake slot nothing could reclaim. Destroying it is a world-safety
   decision and deliberately nothing to do with the camera: a body that has
   merely scrolled off screen is left exactly where it is. */
static void TerrainRemoveLostBodies(DynamicTerrainSystem *system,
                                    const World *world)
{
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &system->bodies[slot];

        if (!body->active) {
            continue;
        }
        if (TerrainBodyIsLost(body, world, &system->config)) {
            DynamicTerrainFreeBody(system, (TerrainBodyHandle){
                (uint16_t)slot, body->generation});
            ++system->stats.bodiesRemovedOutOfBounds;
        }
    }
}

void TerrainPhysicsUpdate(DynamicTerrainSystem *system, const World *world,
                          float deltaTime)
{
    TerrainContactWorkspace *contacts;
    bool touched[MAX_TERRAIN_BODIES] = {false};
    int substeps = 1;
    int substep;
    int slot;

    if (system == NULL || system->material == NULL) {
        return;
    }
    if (!TerrainStepIsUsable(deltaTime)) {
        return;
    }
    if (world != NULL && world->cells == NULL) {
        world = NULL;
    }
    contacts = &system->contacts;

    system->stats.collisionBodies = 0;
    system->stats.collisionContacts = 0;
    system->stats.collisionSubsteps = 0;
    TerrainContactStatsReset(contacts);

    if (world != NULL) {
        TerrainRemoveLostBodies(system, world);
    }
    /* Before anything moves: a body that woke since the last update takes
       whatever was resting on it along, so the pieces above a kicked base fall
       with it rather than hanging where the base was. */
    TerrainPairWakeNeighbours(contacts, system);

    /* Every awake body takes the same number of substeps, set by the fastest
       of them. Bodies collide with each other, and two bodies can only be
       compared at the same moment; a per-body count would have one body at
       the end of the step while its neighbour was still at the start. The
       bound is unchanged — the slowest body was always allowed to take as many
       steps as the fastest — and a resting body walks its surface a few more
       times when something nearby is quick. */
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &system->bodies[slot];
        int needed;

        if (!body->active) {
            continue;
        }
        body->impactImpulse = 0.0f;
        if (!body->awake) {
            continue;
        }
        needed = TerrainSubstepCount(body, deltaTime);
        if (needed > substeps) {
            substeps = needed;
        }
    }
    system->stats.collisionSubsteps = substeps;

    for (substep = 0; substep < substeps; ++substep) {
        float stepTime = deltaTime / (float)substeps;

        TerrainContactBegin(contacts, stepTime);
        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            TerrainBody *body = &system->bodies[slot];
            TerrainContactSet *set = &contacts->world[slot];

            if (!body->active || !body->awake) {
                continue;
            }
            /* Re-asked every substep, so a body climbing through the band
               loses its weight while it climbs rather than at the end. */
            DynamicTerrainIntegrateBody(system, body, stepTime,
                                        WorldGravityScaleAt(world,
                                                            body->position.y));
            if (world == NULL) {
                continue;
            }
            TerrainCollectContacts(system, world, body, slot, set);
            if (set->count == 0) {
                continue;
            }
            system->stats.collisionContacts += set->count;
            if (set->count > system->stats.maxContactsObserved) {
                system->stats.maxContactsObserved = set->count;
            }
            touched[slot] = true;
        }
        /* Bodies against each other, then everything solved together. */
        TerrainPairCollect(contacts, system);
        TerrainContactSolve(contacts, system);
    }

    /* Sleep is judged once per fixed step, not once per substep, so the quiet
       time a body accumulates means the same thing however fast it happened to
       be moving. */
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &system->bodies[slot];

        if (touched[slot]) {
            ++system->stats.collisionBodies;
        }
        if (body->active && body->awake) {
            DynamicTerrainSettleBody(system, body, deltaTime);
        }
    }
    /* Derived from the invariant, not counted by this loop, so they are equally
       correct for a caller that never calls update. */
    (void)DynamicTerrainStatistics(system);
}
