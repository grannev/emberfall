/* The coupled contact solver. See terrain_contact.h for why the world and
 * the pairs are solved in one loop; this file records the response itself.
 *
 * Both kinds of contact use the same rule: an impulse along the normal that
 * leaves the contact separating at restitution times the speed it was closing
 * at when found (nothing at all below a threshold, or a settling body would
 * be kicked awake every tick), then friction along the face bounded by
 * Coulomb's rule. A world contact moves one body; a pair contact moves two,
 * symmetrically in their inverse masses and inertias, so momentum is exchanged
 * rather than created.
 *
 * The passes are projected Gauss-Seidel over accumulated impulses. Each pass
 * gives every contact the impulse that would stop it now, and clamps the
 * running total delivered through that contact at zero rather than the step:
 * a later pass can take back what an earlier one over-delivered, but no
 * contact ever pulls. That is what lets sixteen contacts under a flat slab —
 * each of which would stop the whole slab on its own — settle on a sixteenth
 * each without the slab picking up a spin, and what lets a pile converge: the
 * first version of this solver gave each contact a fixed share and removed a
 * quarter of a three-body stack's residual per pass, which left every body in
 * it creeping above the sleep threshold for ever.
 */
#include "terrain_contact.h"

#include <math.h>
#include <stddef.h>

#include "dynamic_terrain.h"

/* Below this closing speed a contact is treated as resting and does not
   bounce. Without it a body settling on the floor would be given a small kick
   every tick and would never stop, let alone sleep. */
#define TERRAIN_BOUNCE_THRESHOLD 6.0f
/* Penetration below this is left alone. Correcting every last thousandth is
   what makes a resting body jitter, and jitter is what stops it sleeping. */
#define TERRAIN_PENETRATION_SLOP 0.02f
/* Fraction of the excess penetration removed per substep. Removing all of it
   at once turns a deep overlap into a visible pop. */
#define TERRAIN_CORRECTION_RATE 0.6f
#define TERRAIN_PAIR_CORRECTION_RATE 0.5f

void TerrainContactWorkspaceInit(TerrainContactWorkspace *workspace)
{
    int slot;

    if (workspace == NULL) {
        return;
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainContactClear(workspace->world[slot].contacts);
        workspace->world[slot].count = 0;
        workspace->world[slot].deepest = 0.0f;
        workspace->boundsValid[slot] = false;
    }
    workspace->manifoldCount = 0;
    workspace->stepTime = 0.0f;
    TerrainContactForget(workspace);
    TerrainContactStatsReset(workspace);
}

void TerrainContactStatsReset(TerrainContactWorkspace *workspace)
{
    TerrainContactStats empty = {0, 0, 0, 0, 0, 0, 0.0f};

    if (workspace == NULL) {
        return;
    }
    workspace->stats = empty;
}

void TerrainContactClear(TerrainContact *contacts)
{
    int index;

    for (index = 0; index < TERRAIN_CONTACT_CAPACITY; ++index) {
        contacts[index].penetration = -1.0f;
        contacts[index].sample = -1;
    }
}

static void TerrainWarmRemember(TerrainWarmContact *warm,
                                const TerrainContact *contacts)
{
    int index;

    for (index = 0; index < TERRAIN_CONTACT_CAPACITY; ++index) {
        const TerrainContact *contact = &contacts[index];

        if (TerrainContactIsEmpty(contact)) {
            warm[index].sample = -1;
            warm[index].normalImpulse = 0.0f;
            warm[index].tangentImpulse = 0.0f;
        } else {
            warm[index].sample = contact->sample;
            warm[index].normalImpulse = contact->normalImpulse;
            warm[index].tangentImpulse = contact->tangentImpulse;
        }
    }
}

void TerrainContactForget(TerrainContactWorkspace *workspace)
{
    int slot;

    if (workspace == NULL) {
        return;
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        workspace->warmWorldCount[slot] = 0;
    }
    workspace->warmManifoldCount = 0;
    workspace->warmStepTime = 0.0f;
}

void TerrainContactForgetBody(TerrainContactWorkspace *workspace, int slot)
{
    int index;

    if (workspace == NULL || slot < 0 || slot >= MAX_TERRAIN_BODIES) {
        return;
    }
    workspace->warmWorldCount[slot] = 0;
    for (index = 0; index < workspace->warmManifoldCount; ++index) {
        TerrainWarmManifold *warm = &workspace->warmManifolds[index];

        if (warm->sampler == slot || warm->raster == slot) {
            /* Naming a slot no body can have is what empties it: the lookup
               matches on both slots, and neither is ever this. */
            warm->sampler = -1;
            warm->raster = -1;
        }
    }
}

void TerrainContactBegin(TerrainContactWorkspace *workspace, float stepTime)
{
    int slot;
    int index;

    if (workspace == NULL) {
        return;
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainContactSet *set = &workspace->world[slot];

        /* An empty set was cleared when it was last emptied, and a slot with
           nothing remembered is never read: only bodies in contact cost. */
        workspace->warmWorldCount[slot] = set->count;
        if (set->count <= 0) {
            continue;
        }
        TerrainWarmRemember(workspace->warmWorld[slot], set->contacts);
        TerrainContactClear(set->contacts);
        set->count = 0;
        set->deepest = 0.0f;
    }
    for (index = 0; index < workspace->manifoldCount; ++index) {
        const TerrainPairManifold *manifold = &workspace->manifolds[index];
        TerrainWarmManifold *warm = &workspace->warmManifolds[index];

        warm->sampler = manifold->sampler;
        warm->raster = manifold->raster;
        TerrainWarmRemember(warm->contacts, manifold->contacts);
    }
    workspace->warmManifoldCount = workspace->manifoldCount;
    workspace->warmStepTime = workspace->stepTime;
    workspace->stepTime = stepTime;
    workspace->manifoldCount = 0;
}

static void TerrainContactStore(TerrainContact *contact, int *count, int sample,
                                Vector2 point, Vector2 normal,
                                float penetration, float approachSpeed)
{
    if (TerrainContactIsEmpty(contact)) {
        ++*count;
    }
    contact->point = point;
    contact->normal = normal;
    contact->penetration = penetration;
    contact->approachSpeed = approachSpeed;
    contact->normalImpulse = 0.0f;
    contact->tangentImpulse = 0.0f;
    contact->sample = sample;
}

/* Position of a point along a face: its projection on the face's tangent. */
static float TerrainAlongFace(Vector2 point, Vector2 normal)
{
    return -point.x * normal.y + point.y * normal.x;
}

void TerrainContactAdd(TerrainContact *contacts, int *count, float *deepest,
                       int face, int sample, Vector2 point, Vector2 normal,
                       float penetration, float approachSpeed)
{
    TerrainContact *group;
    float along;

    if (penetration < 0.0f) {
        penetration = 0.0f;
    }
    if (penetration > *deepest) {
        *deepest = penetration;
    }
    if (face < 0) {
        face = 0;
    } else if (face >= TERRAIN_CONTACT_FACES) {
        face = TERRAIN_CONTACT_FACES - 1;
    }
    group = &contacts[face * TERRAIN_CONTACTS_PER_FACE];
    along = TerrainAlongFace(point, normal);

    /* Slot 0 is the deepest, slot 1 the nearest end of the face, slot 2 the
       farthest. Ties keep what is there, so the answer does not depend on
       how many equal samples the surface list happened to hold. */
    if (TerrainContactIsEmpty(&group[0]) || penetration > group[0].penetration) {
        TerrainContactStore(&group[0], count, sample, point, normal,
                            penetration, approachSpeed);
    }
    if (TerrainContactIsEmpty(&group[1]) ||
        along < TerrainAlongFace(group[1].point, group[1].normal)) {
        TerrainContactStore(&group[1], count, sample, point, normal,
                            penetration, approachSpeed);
    }
    if (TerrainContactIsEmpty(&group[2]) ||
        along > TerrainAlongFace(group[2].point, group[2].normal)) {
        TerrainContactStore(&group[2], count, sample, point, normal,
                            penetration, approachSpeed);
    }
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

static float TerrainBounce(const TerrainContact *contact,
                           const DynamicTerrainConfig *config)
{
    return fabsf(contact->approachSpeed) > TERRAIN_BOUNCE_THRESHOLD
               ? config->restitution * contact->approachSpeed
               : 0.0f;
}

/* The relative velocity at the contact of the body being pushed along the
   normal over the body being pushed against it. The world stands still. */
static Vector2 TerrainRelativeVelocity(const TerrainBody *along,
                                       const TerrainBody *against,
                                       Vector2 point)
{
    Vector2 velocity = TerrainPointVelocity(along, point);

    if (against != NULL) {
        Vector2 other = TerrainPointVelocity(against, point);

        velocity.x -= other.x;
        velocity.y -= other.y;
    }
    return velocity;
}

/* Effective mass of the pair along a direction: the sum of what each body
   yields to an impulse there, linearly and by turning about its centre. */
static float TerrainEffectiveMass(const TerrainBody *along, float alongInverseMass,
                                  float alongInverseInertia,
                                  const TerrainBody *against,
                                  float againstInverseMass,
                                  float againstInverseInertia, Vector2 point,
                                  Vector2 direction)
{
    float leverX = point.x - along->position.x;
    float leverY = point.y - along->position.y;
    float lever = leverX * direction.y - leverY * direction.x;
    float mass = alongInverseMass + lever * lever * alongInverseInertia;

    if (against != NULL) {
        leverX = point.x - against->position.x;
        leverY = point.y - against->position.y;
        lever = leverX * direction.y - leverY * direction.x;
        mass += againstInverseMass + lever * lever * againstInverseInertia;
    }
    return mass;
}

/* Delivers an impulse of `magnitude` along `direction` at `point`: to `along`
   as given and to `against`, when there is one, reversed. */
static void TerrainDeliver(TerrainBody *along, float alongInverseMass,
                           float alongInverseInertia, TerrainBody *against,
                           float againstInverseMass, float againstInverseInertia,
                           Vector2 point, Vector2 direction, float magnitude)
{
    float leverX = point.x - along->position.x;
    float leverY = point.y - along->position.y;

    along->velocity.x += magnitude * direction.x * alongInverseMass;
    along->velocity.y += magnitude * direction.y * alongInverseMass;
    along->angularVelocity += (leverX * direction.y - leverY * direction.x) *
                              magnitude * alongInverseInertia;
    if (against != NULL) {
        leverX = point.x - against->position.x;
        leverY = point.y - against->position.y;
        against->velocity.x -= magnitude * direction.x * againstInverseMass;
        against->velocity.y -= magnitude * direction.y * againstInverseMass;
        against->angularVelocity -= (leverX * direction.y - leverY * direction.x) *
                                    magnitude * againstInverseInertia;
    }
}

/* One pass of one contact. `along` is pushed along the normal; `against` is
   pushed the other way, or is the world when NULL. A body with zero inverse
   mass is a wall.

   The impulse is accumulated across the passes and the accumulation, not the
   step, is what is clamped: a pass may take back what an earlier pass
   delivered through this contact — that is how sixteen contacts under a flat
   slab, each of which would stop the whole slab on its own, settle on one
   sixteenth each and the slab lands without a spin — but a contact never
   pulls, so the total stays a push. */
static void TerrainApplyContact(TerrainContact *contact, TerrainBody *along,
                                float alongInverseMass, float alongInverseInertia,
                                TerrainBody *against, float againstInverseMass,
                                float againstInverseInertia,
                                const DynamicTerrainConfig *config)
{
    Vector2 normal = contact->normal;
    Vector2 tangent = {-normal.y, normal.x};
    Vector2 relative = TerrainRelativeVelocity(along, against, contact->point);
    float normalSpeed = relative.x * normal.x + relative.y * normal.y;
    float effectiveMass;
    float accumulated;
    float step;
    float limit;

    effectiveMass = TerrainEffectiveMass(along, alongInverseMass,
                                         alongInverseInertia, against,
                                         againstInverseMass, againstInverseInertia,
                                         contact->point, normal);
    if (!(effectiveMass > 0.0f)) {
        return;
    }
    step = -(normalSpeed + TerrainBounce(contact, config)) / effectiveMass;
    accumulated = contact->normalImpulse + step;
    if (accumulated < 0.0f) {
        accumulated = 0.0f;
    }
    step = accumulated - contact->normalImpulse;
    contact->normalImpulse = accumulated;
    if (step != 0.0f) {
        TerrainDeliver(along, alongInverseMass, alongInverseInertia, against,
                       againstInverseMass, againstInverseInertia,
                       contact->point, normal, step);
    }

    /* Friction along the face, bounded by Coulomb's rule on the accumulated
       push. The velocity is re-read because the normal impulse just changed
       both bodies' spin, and a resting contact's tangential motion is mostly
       spin. */
    effectiveMass = TerrainEffectiveMass(along, alongInverseMass,
                                         alongInverseInertia, against,
                                         againstInverseMass, againstInverseInertia,
                                         contact->point, tangent);
    if (!(effectiveMass > 0.0f)) {
        return;
    }
    relative = TerrainRelativeVelocity(along, against, contact->point);
    step = -(relative.x * tangent.x + relative.y * tangent.y) / effectiveMass;
    limit = config->friction * contact->normalImpulse;
    accumulated = contact->tangentImpulse + step;
    if (accumulated > limit) {
        accumulated = limit;
    } else if (accumulated < -limit) {
        accumulated = -limit;
    }
    step = accumulated - contact->tangentImpulse;
    contact->tangentImpulse = accumulated;
    if (step != 0.0f) {
        TerrainDeliver(along, alongInverseMass, alongInverseInertia, against,
                       againstInverseMass, againstInverseInertia,
                       contact->point, tangent, step);
    }
}

/* Asleep is a wall. A sleeper struck hard enough was woken before the solve
   began and is a body here. */
static float TerrainInverseMass(const TerrainBody *body)
{
    return body->awake && body->mass > 0.0f ? 1.0f / body->mass : 0.0f;
}

static float TerrainInverseInertia(const TerrainBody *body)
{
    return body->awake && body->inertia > 0.0f ? 1.0f / body->inertia : 0.0f;
}

static void TerrainSolveWorldPass(TerrainContactWorkspace *workspace,
                                  DynamicTerrainSystem *system)
{
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainContactSet *set = &workspace->world[slot];
        TerrainBody *body = &system->bodies[slot];
        float inverseMass;
        int index;

        if (set->count <= 0 || !body->active) {
            continue;
        }
        inverseMass = TerrainInverseMass(body);
        if (inverseMass <= 0.0f) {
            continue;
        }
        for (index = 0; index < TERRAIN_CONTACT_CAPACITY; ++index) {
            if (TerrainContactIsEmpty(&set->contacts[index])) {
                continue;
            }
            TerrainApplyContact(&set->contacts[index], body, inverseMass,
                                TerrainInverseInertia(body), NULL, 0.0f, 0.0f,
                                &system->config);
        }
    }
}

static void TerrainSolvePairPass(TerrainContactWorkspace *workspace,
                                 DynamicTerrainSystem *system)
{
    int index;

    for (index = 0; index < workspace->manifoldCount; ++index) {
        TerrainPairManifold *manifold = &workspace->manifolds[index];
        TerrainBody *sampler = &system->bodies[manifold->sampler];
        TerrainBody *raster = &system->bodies[manifold->raster];
        float samplerInverseMass;
        float rasterInverseMass;
        int contact;

        if (!sampler->active || !raster->active) {
            continue;
        }
        samplerInverseMass = TerrainInverseMass(sampler);
        rasterInverseMass = TerrainInverseMass(raster);
        if (samplerInverseMass <= 0.0f && rasterInverseMass <= 0.0f) {
            continue;
        }
        for (contact = 0; contact < TERRAIN_CONTACT_CAPACITY; ++contact) {
            if (TerrainContactIsEmpty(&manifold->contacts[contact])) {
                continue;
            }
            TerrainApplyContact(&manifold->contacts[contact], sampler,
                                samplerInverseMass,
                                TerrainInverseInertia(sampler), raster,
                                rasterInverseMass,
                                TerrainInverseInertia(raster),
                                &system->config);
        }
    }
}

/* What each body was hit with this substep: the impulses delivered through
   all of its contacts, summed, and the contact that carried the most of it.
   Summed rather than the largest single contact because a slab landing flat
   on sixteen cells was stopped as hard as one landing on a corner, and that
   is what a fracture rule asks. */
static void TerrainRecordImpacts(const TerrainContactWorkspace *workspace,
                                 DynamicTerrainSystem *system)
{
    float total[MAX_TERRAIN_BODIES] = {0.0f};
    float strongest[MAX_TERRAIN_BODIES] = {0.0f};
    Vector2 point[MAX_TERRAIN_BODIES];
    Vector2 normal[MAX_TERRAIN_BODIES];
    int slot;
    int index;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainContactSet *set = &workspace->world[slot];

        for (index = 0; index < TERRAIN_CONTACT_CAPACITY; ++index) {
            const TerrainContact *contact = &set->contacts[index];

            if (TerrainContactIsEmpty(contact)) {
                continue;
            }
            total[slot] += contact->normalImpulse;
            if (contact->normalImpulse > strongest[slot]) {
                strongest[slot] = contact->normalImpulse;
                point[slot] = contact->point;
                normal[slot] = contact->normal;
            }
        }
    }
    for (index = 0; index < workspace->manifoldCount; ++index) {
        const TerrainPairManifold *manifold = &workspace->manifolds[index];
        int contact;

        for (contact = 0; contact < TERRAIN_CONTACT_CAPACITY; ++contact) {
            const TerrainContact *pair = &manifold->contacts[contact];

            if (TerrainContactIsEmpty(pair)) {
                continue;
            }
            total[manifold->sampler] += pair->normalImpulse;
            total[manifold->raster] += pair->normalImpulse;
            if (pair->normalImpulse > strongest[manifold->sampler]) {
                strongest[manifold->sampler] = pair->normalImpulse;
                point[manifold->sampler] = pair->point;
                normal[manifold->sampler] = pair->normal;
            }
            if (pair->normalImpulse > strongest[manifold->raster]) {
                strongest[manifold->raster] = pair->normalImpulse;
                point[manifold->raster] = pair->point;
                normal[manifold->raster] = (Vector2){-pair->normal.x,
                                                     -pair->normal.y};
            }
        }
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &system->bodies[slot];

        if (total[slot] > body->impactImpulse) {
            body->impactImpulse = total[slot];
            body->impactPoint = point[slot];
            body->impactNormal = normal[slot];
        }
    }
}

/* Direction of the summed normals, or false when they cancel. Averaging
   keeps a body wedged in a corner from being shoved along one wall. */
static bool TerrainAverageNormal(const TerrainContact *contacts,
                                 Vector2 *direction)
{
    Vector2 push = {0.0f, 0.0f};
    float length;
    int index;

    for (index = 0; index < TERRAIN_CONTACT_CAPACITY; ++index) {
        if (TerrainContactIsEmpty(&contacts[index])) {
            continue;
        }
        push.x += contacts[index].normal.x;
        push.y += contacts[index].normal.y;
    }
    length = sqrtf(push.x * push.x + push.y * push.y);
    if (length < 0.0001f) {
        return false;
    }
    direction->x = push.x / length;
    direction->y = push.y / length;
    return true;
}

static void TerrainCorrectPositions(TerrainContactWorkspace *workspace,
                                    DynamicTerrainSystem *system)
{
    int slot;
    int index;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainContactSet *set = &workspace->world[slot];
        TerrainBody *body = &system->bodies[slot];
        Vector2 push;
        float correction;

        if (set->count <= 0 || !body->active || !body->awake ||
            set->deepest <= TERRAIN_PENETRATION_SLOP ||
            !TerrainAverageNormal(set->contacts, &push)) {
            continue;
        }
        correction = (set->deepest - TERRAIN_PENETRATION_SLOP) *
                     TERRAIN_CORRECTION_RATE;
        body->position.x += push.x * correction;
        body->position.y += push.y * correction;
    }

    /* Split by inverse mass, so a chip is moved out of a slab rather than the
       slab out of the chip, and a wall not at all. */
    for (index = 0; index < workspace->manifoldCount; ++index) {
        const TerrainPairManifold *manifold = &workspace->manifolds[index];
        TerrainBody *sampler = &system->bodies[manifold->sampler];
        TerrainBody *raster = &system->bodies[manifold->raster];
        Vector2 push;
        float samplerInverseMass;
        float rasterInverseMass;
        float total;
        float correction;

        if (!sampler->active || !raster->active ||
            manifold->deepest <= TERRAIN_PENETRATION_SLOP) {
            continue;
        }
        samplerInverseMass = TerrainInverseMass(sampler);
        rasterInverseMass = TerrainInverseMass(raster);
        total = samplerInverseMass + rasterInverseMass;
        if (!(total > 0.0f) ||
            !TerrainAverageNormal(manifold->contacts, &push)) {
            continue;
        }
        correction = (manifold->deepest - TERRAIN_PENETRATION_SLOP) *
                     TERRAIN_PAIR_CORRECTION_RATE;
        sampler->position.x += push.x * correction * samplerInverseMass / total;
        sampler->position.y += push.y * correction * samplerInverseMass / total;
        raster->position.x -= push.x * correction * rasterInverseMass / total;
        raster->position.y -= push.y * correction * rasterInverseMass / total;
    }
}

/* Delivers a contact's previous impulses, scaled to this substep, and makes
   them its running totals. The passes then correct the difference. */
static void TerrainWarmStart(TerrainContact *contact,
                             const TerrainWarmContact *warm, float scale,
                             TerrainBody *along, float alongInverseMass,
                             float alongInverseInertia, TerrainBody *against,
                             float againstInverseMass, float againstInverseInertia)
{
    Vector2 tangent = {-contact->normal.y, contact->normal.x};

    if (warm->sample != contact->sample || !(warm->normalImpulse > 0.0f)) {
        return;
    }
    contact->normalImpulse = warm->normalImpulse * scale;
    contact->tangentImpulse = warm->tangentImpulse * scale;
    TerrainDeliver(along, alongInverseMass, alongInverseInertia, against,
                   againstInverseMass, againstInverseInertia, contact->point,
                   contact->normal, contact->normalImpulse);
    TerrainDeliver(along, alongInverseMass, alongInverseInertia, against,
                   againstInverseMass, againstInverseInertia, contact->point,
                   tangent, contact->tangentImpulse);
}

static const TerrainWarmManifold *TerrainWarmManifoldFor(
    const TerrainContactWorkspace *workspace, int sampler, int raster)
{
    int index;

    for (index = 0; index < workspace->warmManifoldCount; ++index) {
        const TerrainWarmManifold *warm = &workspace->warmManifolds[index];

        if (warm->sampler == sampler && warm->raster == raster) {
            return warm;
        }
    }
    return NULL;
}

static void TerrainWarmStartAll(TerrainContactWorkspace *workspace,
                                DynamicTerrainSystem *system)
{
    float scale;
    int slot;
    int index;

    if (!(workspace->warmStepTime > 0.0f) || !(workspace->stepTime > 0.0f)) {
        return;
    }
    scale = workspace->stepTime / workspace->warmStepTime;
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainContactSet *set = &workspace->world[slot];
        TerrainBody *body = &system->bodies[slot];
        float inverseMass;

        if (set->count <= 0 || workspace->warmWorldCount[slot] <= 0 ||
            !body->active) {
            continue;
        }
        inverseMass = TerrainInverseMass(body);
        if (inverseMass <= 0.0f) {
            continue;
        }
        for (index = 0; index < TERRAIN_CONTACT_CAPACITY; ++index) {
            if (TerrainContactIsEmpty(&set->contacts[index])) {
                continue;
            }
            TerrainWarmStart(&set->contacts[index],
                             &workspace->warmWorld[slot][index], scale, body,
                             inverseMass, TerrainInverseInertia(body), NULL,
                             0.0f, 0.0f);
        }
    }
    for (index = 0; index < workspace->manifoldCount; ++index) {
        TerrainPairManifold *manifold = &workspace->manifolds[index];
        const TerrainWarmManifold *warm =
            TerrainWarmManifoldFor(workspace, manifold->sampler, manifold->raster);
        TerrainBody *sampler = &system->bodies[manifold->sampler];
        TerrainBody *raster = &system->bodies[manifold->raster];
        float samplerInverseMass;
        float rasterInverseMass;
        int contact;

        if (warm == NULL || !sampler->active || !raster->active) {
            continue;
        }
        samplerInverseMass = TerrainInverseMass(sampler);
        rasterInverseMass = TerrainInverseMass(raster);
        if (samplerInverseMass <= 0.0f && rasterInverseMass <= 0.0f) {
            continue;
        }
        for (contact = 0; contact < TERRAIN_CONTACT_CAPACITY; ++contact) {
            if (TerrainContactIsEmpty(&manifold->contacts[contact])) {
                continue;
            }
            TerrainWarmStart(&manifold->contacts[contact],
                             &warm->contacts[contact], scale, sampler,
                             samplerInverseMass, TerrainInverseInertia(sampler),
                             raster, rasterInverseMass,
                             TerrainInverseInertia(raster));
        }
    }
}

void TerrainContactSolve(TerrainContactWorkspace *workspace,
                         DynamicTerrainSystem *system)
{
    int iteration;

    if (workspace == NULL || system == NULL) {
        return;
    }
    TerrainWarmStartAll(workspace, system);
    for (iteration = 0; iteration < TERRAIN_SOLVER_ITERATIONS; ++iteration) {
        TerrainSolveWorldPass(workspace, system);
        TerrainSolvePairPass(workspace, system);
    }
    TerrainRecordImpacts(workspace, system);
    {
        int index;

        for (index = 0; index < workspace->manifoldCount; ++index) {
            const TerrainPairManifold *manifold = &workspace->manifolds[index];
            int contact;

            for (contact = 0; contact < TERRAIN_CONTACT_CAPACITY; ++contact) {
                float impulse = manifold->contacts[contact].normalImpulse;

                if (TerrainContactIsEmpty(&manifold->contacts[contact])) {
                    continue;
                }
                if (impulse > workspace->stats.largestImpulse) {
                    workspace->stats.largestImpulse = impulse;
                }
            }
        }
    }
    TerrainCorrectPositions(workspace, system);
}
