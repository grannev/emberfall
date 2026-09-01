/* Terrain bodies against the static world. See terrain_physics.h for the
 * bounds; this file records the shape of the solution and why each piece is the
 * cheap one rather than the general one.
 *
 * There is no broad phase, and there is nothing for one to do. A broad phase
 * exists to avoid testing pairs that cannot touch, but the world is a grid and
 * asking it about a cell is O(1): there is no list of candidate obstacles to
 * cut down. What bounds the cost is the surface list, not a spatial reject.
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
 */
#include "terrain_physics.h"

#include <math.h>
#include <stddef.h>

#include "materials.h"

typedef struct TerrainContact {
    Vector2 point;
    Vector2 normal;
    float penetration;
    /* How fast the body was closing on this contact when it was found, before
       the solver touched anything. Restitution has to be measured against this
       rather than against the current velocity: the solver drains the approach
       as it goes, so by the last contact of the last iteration there would be
       nothing left for a bounce to act on, and restitution would do almost
       nothing however high it was set. */
    float approachSpeed;
} TerrainContact;

typedef struct TerrainContactSet {
    TerrainContact contacts[MAX_TERRAIN_CONTACTS_PER_BODY];
    int count;
    /* Deepest overlap seen this substep, including contacts that did not make
       it into the set. Positional correction uses it so that dropping shallow
       contacts can never make a body sink further. */
    float deepest;
} TerrainContactSet;

/* Below this closing speed a contact is treated as resting and does not bounce.
   Without it a body settling on the floor would be given a small kick every
   tick and would never stop, let alone sleep. */
#define TERRAIN_BOUNCE_THRESHOLD 6.0f

/* Penetration below this is left alone. Correcting every last thousandth is
   what makes a resting body jitter, and jitter is what stops it sleeping. */
#define TERRAIN_PENETRATION_SLOP 0.02f
/* Fraction of the excess penetration removed per substep. Removing all of it at
   once turns a deep overlap into a visible pop. */
#define TERRAIN_CORRECTION_RATE 0.6f

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
    return maximum.x < -config->killBoundsMargin ||
           maximum.y < -config->killBoundsMargin ||
           minimum.x > (float)world->width + config->killBoundsMargin ||
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
                                 int cellY, Vector2 *normal, float *penetration)
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

static void TerrainAddContact(TerrainContactSet *set, Vector2 point,
                              Vector2 normal, float penetration,
                              float approachSpeed)
{
    int shallowest = 0;
    int i;

    if (penetration > set->deepest) {
        set->deepest = penetration;
    }
    if (set->count < MAX_TERRAIN_CONTACTS_PER_BODY) {
        set->contacts[set->count].point = point;
        set->contacts[set->count].normal = normal;
        set->contacts[set->count].penetration = penetration;
        set->contacts[set->count].approachSpeed = approachSpeed;
        ++set->count;
        return;
    }
    /* Full: keep the deepest, since those are the ones the response has to
       answer. Overflow is a quality question, never a safety one. */
    for (i = 1; i < MAX_TERRAIN_CONTACTS_PER_BODY; ++i) {
        if (set->contacts[i].penetration < set->contacts[shallowest].penetration) {
            shallowest = i;
        }
    }
    if (penetration > set->contacts[shallowest].penetration) {
        set->contacts[shallowest].point = point;
        set->contacts[shallowest].normal = normal;
        set->contacts[shallowest].penetration = penetration;
        set->contacts[shallowest].approachSpeed = approachSpeed;
    }
}

static void TerrainCollectContacts(const DynamicTerrainSystem *system,
                                   const World *world, const TerrainBody *body,
                                   int slot, TerrainContactSet *set)
{
    size_t surfaceBase = (size_t)slot * (size_t)MAX_TERRAIN_BODY_CELLS;
    float cosine = cosf(body->angle);
    float sine = sinf(body->angle);
    int index;

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
                                  &penetration)) {
            continue;
        }
        {
            Vector2 pointVelocity = TerrainPointVelocity(body, sample);

            TerrainAddContact(set, sample, normal, penetration,
                              pointVelocity.x * normal.x +
                                  pointVelocity.y * normal.y);
        }
    }
}

static void TerrainApplyContactImpulse(TerrainBody *body, const TerrainContact *contact,
                                       const DynamicTerrainConfig *config, float share)
{
    Vector2 pointVelocity = TerrainPointVelocity(body, contact->point);
    Vector2 tangent = {-contact->normal.y, contact->normal.x};
    float leverX = contact->point.x - body->position.x;
    float leverY = contact->point.y - body->position.y;
    float inverseMass = 1.0f / body->mass;
    float inverseInertia = 1.0f / body->inertia;
    float normalSpeed = pointVelocity.x * contact->normal.x +
                        pointVelocity.y * contact->normal.y;
    float leverNormal;
    float effectiveMass;
    float normalImpulse;
    float tangentSpeed;
    float leverTangent;
    float tangentImpulse;
    float limit;

    /* Already separating: a contact that is coming apart needs no help. */
    if (normalSpeed >= 0.0f) {
        return;
    }

    leverNormal = leverX * contact->normal.y - leverY * contact->normal.x;
    effectiveMass = inverseMass + leverNormal * leverNormal * inverseInertia;
    if (!(effectiveMass > 0.0f)) {
        return;
    }
    /* Target: leave the contact separating at restitution times the speed it
       was closing at when it was found. With restitution zero this reduces to
       simply cancelling the approach. A contact that was barely moving does not
       bounce at all, or a settling body would be kicked awake every tick. */
    {
        float bounce = fabsf(contact->approachSpeed) > TERRAIN_BOUNCE_THRESHOLD
                           ? config->restitution * contact->approachSpeed
                           : 0.0f;

        normalImpulse = -(normalSpeed + bounce) / effectiveMass * share;
    }
    if (normalImpulse < 0.0f) {
        return;
    }

    body->velocity.x += normalImpulse * contact->normal.x * inverseMass;
    body->velocity.y += normalImpulse * contact->normal.y * inverseMass;
    body->angularVelocity += leverNormal * normalImpulse * inverseInertia;

    /* Friction along the contact face, bounded by Coulomb's rule. Recomputing
       the point velocity would be more correct and, at these speeds, buys
       nothing worth the second transform. */
    tangentSpeed = pointVelocity.x * tangent.x + pointVelocity.y * tangent.y;
    leverTangent = leverX * tangent.y - leverY * tangent.x;
    effectiveMass = inverseMass + leverTangent * leverTangent * inverseInertia;
    if (!(effectiveMass > 0.0f)) {
        return;
    }
    tangentImpulse = -tangentSpeed / effectiveMass * share;
    limit = config->friction * fabsf(normalImpulse);
    if (tangentImpulse > limit) {
        tangentImpulse = limit;
    } else if (tangentImpulse < -limit) {
        tangentImpulse = -limit;
    }

    body->velocity.x += tangentImpulse * tangent.x * inverseMass;
    body->velocity.y += tangentImpulse * tangent.y * inverseMass;
    body->angularVelocity += leverTangent * tangentImpulse * inverseInertia;
}

static void TerrainResolveContacts(TerrainBody *body, const TerrainContactSet *set,
                                   const DynamicTerrainConfig *config)
{
    Vector2 push = {0.0f, 0.0f};
    float pushLength;
    float share;
    int iteration;
    int index;

    if (set->count <= 0 || !(body->mass > 0.0f) || !(body->inertia > 0.0f)) {
        return;
    }

    /* Contacts share one body's worth of response. This is not a sequential
       impulse solver: without the share, a slab resting on twenty cells would
       receive twenty times the push it needs and leap off the ground. */
    share = 1.0f / (float)set->count;
    for (iteration = 0; iteration < TERRAIN_SOLVER_ITERATIONS; ++iteration) {
        for (index = 0; index < set->count; ++index) {
            TerrainApplyContactImpulse(body, &set->contacts[index], config, share);
        }
    }

    /* Positional correction along the averaged contact normal. Averaging keeps
       a body wedged in a corner from being shoved along one wall. */
    for (index = 0; index < set->count; ++index) {
        push.x += set->contacts[index].normal.x;
        push.y += set->contacts[index].normal.y;
    }
    pushLength = sqrtf(push.x * push.x + push.y * push.y);
    if (pushLength < 0.0001f || set->deepest <= TERRAIN_PENETRATION_SLOP) {
        return;
    }
    push.x /= pushLength;
    push.y /= pushLength;
    body->position.x += push.x * (set->deepest - TERRAIN_PENETRATION_SLOP) *
                        TERRAIN_CORRECTION_RATE;
    body->position.y += push.y * (set->deepest - TERRAIN_PENETRATION_SLOP) *
                        TERRAIN_CORRECTION_RATE;
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

void TerrainPhysicsUpdate(DynamicTerrainSystem *system, const World *world,
                          float deltaTime)
{
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

    system->stats.collisionBodies = 0;
    system->stats.collisionContacts = 0;
    system->stats.collisionSubsteps = 0;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &system->bodies[slot];
        TerrainContactSet set;
        int substeps;
        int substep;

        if (!body->active) {
            continue;
        }
        /* A body that has left the map can never touch anything again, so it
           would fall for ever, never satisfy the sleep condition, and hold an
           awake slot nothing could reclaim. Destroying it is a world-safety
           decision and deliberately nothing to do with the camera: a body that
           has merely scrolled off screen is left exactly where it is. */
        if (world != NULL && TerrainBodyIsLost(body, world, &system->config)) {
            DynamicTerrainFreeBody(system, (TerrainBodyHandle){
                (uint16_t)slot, body->generation});
            ++system->stats.bodiesRemovedOutOfBounds;
            continue;
        }
        if (!body->awake) {
            continue;
        }

        substeps = world != NULL ? TerrainSubstepCount(body, deltaTime) : 1;
        system->stats.collisionSubsteps += substeps;
        for (substep = 0; substep < substeps; ++substep) {
            DynamicTerrainIntegrateBody(system, body,
                                        deltaTime / (float)substeps);
            if (world == NULL) {
                continue;
            }
            TerrainCollectContacts(system, world, body, slot, &set);
            if (set.count == 0) {
                continue;
            }
            system->stats.collisionContacts += set.count;
            if (set.count > system->stats.maxContactsObserved) {
                system->stats.maxContactsObserved = set.count;
            }
            TerrainResolveContacts(body, &set, &system->config);
        }
        if (world != NULL && system->stats.collisionContacts > 0) {
            ++system->stats.collisionBodies;
        }

        /* Sleep is judged once per fixed step, not once per substep, so the
           quiet time a body accumulates means the same thing however fast it
           happened to be moving. */
        DynamicTerrainSettleBody(system, body, deltaTime);
    }
    /* Derived from the invariant, not counted by this loop, so they are equally
       correct for a caller that never calls update. */
    (void)DynamicTerrainStatistics(system);
}
