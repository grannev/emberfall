/* Player against terrain bodies. See terrain_interaction.h for the contract.
 *
 * The collision test is the player's own world test moved into the body's
 * frame. Rotation preserves distance, so the player's circle is still a circle
 * once transformed, and one transform of the centre buys a test against a plain
 * axis-aligned raster — no rotated-box mathematics, and no second collision
 * convention to keep in step with the first.
 *
 * Cost per frame is a flat pass over the live slots, each rejected on its world
 * box before anything is transformed. The body budget is a hard 32, so this is
 * cheaper than the broad phase that would replace it, and nothing here
 * allocates.
 */
#include "terrain_interaction.h"

#include <math.h>
#include <stddef.h>

TerrainInteractionConfig TerrainInteractionDefaultConfig(void)
{
    TerrainInteractionConfig config;

    /* About the mass of a forty-cell block of rock, so running into a chip
       shoves it well and running into a slab mostly stops the player. */
    config.playerMass = 110.0f;
    config.contactRestitution = 0.05f;
    config.maxContactImpulse = 9000.0f;

    config.grabDistance = 26.0f;
    config.holdDistance = 15.0f;
    /* Strong enough to lift a slab of a few hundred cells against gravity, and
       capped so a boulder can still be too heavy to pick up: the cap divided by
       mass is the most the hold can accelerate anything, and once that falls
       below gravity the body can only be dragged along the ground. That is the
       whole difficulty curve, and it comes out of mass rather than out of a
       rule about what may be carried. */
    config.pullStrength = 12000.0f;
    config.pullDamping = 1200.0f;
    config.maxPullForce = 400000.0f;
    config.throwImpulse = 14000.0f;
    return config;
}

void TerrainInteractionInit(TerrainInteractionSystem *system)
{
    if (system == NULL) {
        return;
    }
    system->config = TerrainInteractionDefaultConfig();
    system->held = TerrainBodyInvalidHandle();
    system->hovered = TerrainBodyInvalidHandle();
    system->holdLocalPoint = (Vector2){0.0f, 0.0f};
    system->holdWorldPoint = (Vector2){0.0f, 0.0f};
    system->previousPosition = (Vector2){0.0f, 0.0f};
    system->hasPreviousPosition = false;
    system->holding = false;
    TerrainInteractionResetStats(system);
}

void TerrainInteractionResetStats(TerrainInteractionSystem *system)
{
    if (system == NULL) {
        return;
    }
    {
        TerrainInteractionStats empty = {0, 0, 0, 0, 0, 0, 0, 0, 0};

        system->stats = empty;
    }
}

bool TerrainInteractionIsHolding(const TerrainInteractionSystem *system,
                                 const DynamicTerrainSystem *terrain)
{
    return system != NULL && system->holding &&
           DynamicTerrainGetConst(terrain, system->held) != NULL;
}

static float TerrainInteractionClamp(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/* Velocity of the world point `at` on a rigid body: its own, plus the part that
   comes from turning about the centre of mass. */
static Vector2 TerrainBodyPointVelocity(const TerrainBody *body, Vector2 at)
{
    float leverX = at.x - body->position.x;
    float leverY = at.y - body->position.y;

    return (Vector2){body->velocity.x - body->angularVelocity * leverY,
                     body->velocity.y + body->angularVelocity * leverX};
}

/* Deepest overlap between the player's circle and one body's occupied cells,
   all in the body's frame. Returns false when they do not touch.

   `localContact` comes back as the point on the cell nearest the player, and
   `localNormal` as the direction that separates them, both local. */
static bool TerrainInteractionProbe(const DynamicTerrainSystem *terrain,
                                    TerrainBodyHandle handle,
                                    const TerrainBody *body, Vector2 localCentre,
                                    float radius, Vector2 *localContact,
                                    Vector2 *localNormal, float *depth)
{
    float best = 0.0f;
    int firstX = (int)floorf(localCentre.x - radius);
    int firstY = (int)floorf(localCentre.y - radius);
    int lastX = (int)ceilf(localCentre.x + radius);
    int lastY = (int)ceilf(localCentre.y + radius);
    int localY;

    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > body->width - 1) lastX = body->width - 1;
    if (lastY > body->height - 1) lastY = body->height - 1;

    for (localY = firstY; localY <= lastY; ++localY) {
        int localX;

        for (localX = firstX; localX <= lastX; ++localX) {
            float nearestX;
            float nearestY;
            float dx;
            float dy;
            float distance;
            float overlap;

            if (DynamicTerrainCellAt(terrain, handle, localX, localY) ==
                MATERIAL_EMPTY) {
                continue;
            }
            nearestX = TerrainInteractionClamp(localCentre.x, (float)localX,
                                               (float)localX + 1.0f);
            nearestY = TerrainInteractionClamp(localCentre.y, (float)localY,
                                               (float)localY + 1.0f);
            dx = localCentre.x - nearestX;
            dy = localCentre.y - nearestY;
            distance = sqrtf(dx * dx + dy * dy);
            overlap = radius - distance;
            if (overlap <= best) {
                continue;
            }
            best = overlap;
            *localContact = (Vector2){nearestX, nearestY};
            if (distance > 0.0001f) {
                *localNormal = (Vector2){dx / distance, dy / distance};
            } else {
                /* The centre is inside the cell, so there is no direction to
                   read off the offset. Leave along the shortest way out of the
                   cell box: it is the smallest correction that frees the
                   player, and it is a function of the geometry rather than of
                   whatever the last frame happened to do. */
                float left = localCentre.x - (float)localX;
                float right = (float)localX + 1.0f - localCentre.x;
                float up = localCentre.y - (float)localY;
                float down = (float)localY + 1.0f - localCentre.y;
                float least = left;

                *localNormal = (Vector2){-1.0f, 0.0f};
                if (right < least) { least = right; *localNormal = (Vector2){1.0f, 0.0f}; }
                if (up < least) { least = up; *localNormal = (Vector2){0.0f, -1.0f}; }
                if (down < least) { *localNormal = (Vector2){0.0f, 1.0f}; }
                best = radius + least;
            }
        }
    }
    if (best <= 0.0f) {
        return false;
    }
    *depth = best;
    return true;
}

/* Rotates a local direction into the world. The transform helpers move points;
   a direction is the same rotation without the translation. */
static Vector2 TerrainInteractionRotate(const TerrainBody *body, Vector2 local)
{
    float cosine = cosf(body->angle);
    float sine = sinf(body->angle);

    return (Vector2){local.x * cosine - local.y * sine,
                     local.x * sine + local.y * cosine};
}

/* One body against the player: separate them, then trade momentum. */
static bool TerrainInteractionResolve(TerrainInteractionSystem *system,
                                      Player *player,
                                      DynamicTerrainSystem *terrain, int slot)
{
    TerrainBody *body = &terrain->bodies[slot];
    TerrainBodyHandle handle = {(uint16_t)slot, body->generation};
    Vector2 minimum;
    Vector2 maximum;
    Vector2 localCentre;
    /* Initialised because the compiler cannot see that the probe writes both
       whenever it reports a contact, and a warning left standing is a warning
       nobody reads. */
    Vector2 localContact = {0.0f, 0.0f};
    Vector2 localNormal = {0.0f, -1.0f};
    Vector2 normal;
    Vector2 contact;
    Vector2 pointVelocity;
    float depth = 0.0f;
    float approach;
    float leverX;
    float leverY;
    float angularTerm;
    float impulse;

    if (!TerrainBodyWorldBounds(body, &minimum, &maximum)) {
        return false;
    }
    /* Rejected on the box before a single trigonometric call. */
    if (player->position.x + player->radius < minimum.x ||
        player->position.x - player->radius > maximum.x ||
        player->position.y + player->radius < minimum.y ||
        player->position.y - player->radius > maximum.y) {
        return false;
    }

    localCentre = TerrainBodyWorldToLocal(body, player->position.x,
                                          player->position.y);
    if (!TerrainInteractionProbe(terrain, handle, body, localCentre,
                                 player->radius, &localContact, &localNormal,
                                 &depth)) {
        return false;
    }

    normal = TerrainInteractionRotate(body, localNormal);
    contact = TerrainBodyLocalToWorld(body, localContact.x, localContact.y);
    ++system->stats.contacts;

    /* The player moves out, never the body. A slab the player is standing on
       must not be shoved aside to make room for them. */
    player->position.x += normal.x * depth;
    player->position.y += normal.y * depth;

    pointVelocity = TerrainBodyPointVelocity(body, contact);
    approach = (player->velocity.x - pointVelocity.x) * normal.x +
               (player->velocity.y - pointVelocity.y) * normal.y;
    if (approach >= 0.0f) {
        /* Already separating: pushing them apart again would be inventing
           energy. */
        return true;
    }

    leverX = contact.x - body->position.x;
    leverY = contact.y - body->position.y;
    angularTerm = (leverX * normal.y - leverY * normal.x);
    angularTerm = angularTerm * angularTerm / body->inertia;
    impulse = -(1.0f + system->config.contactRestitution) * approach /
              (1.0f / system->config.playerMass + 1.0f / body->mass +
               angularTerm);
    if (impulse > system->config.maxContactImpulse) {
        impulse = system->config.maxContactImpulse;
    }

    player->velocity.x += normal.x * impulse / system->config.playerMass;
    player->velocity.y += normal.y * impulse / system->config.playerMass;
    DynamicTerrainApplyImpulse(terrain, handle,
                               (Vector2){-normal.x * impulse,
                                         -normal.y * impulse}, contact);
    ++system->stats.pushImpulses;
    return true;
}

/* Snaps the aim onto an occupied cell of `body`, searching outward from where
   it landed. Returns false when there is no material within reach of the aim,
   which is the honest answer for a body whose bounding box the aim crossed
   through a hole. */
static bool TerrainInteractionGrabPoint(const DynamicTerrainSystem *terrain,
                                        TerrainBodyHandle handle,
                                        const TerrainBody *body, Vector2 aim,
                                        Vector2 *localPoint)
{
    /* Four cells: enough to forgive a cursor on the rim of a rock, small enough
       that it can never wander to the far side of one. */
    const int reach = 4;
    Vector2 local = TerrainBodyWorldToLocal(body, aim.x, aim.y);
    int aimX = (int)floorf(local.x);
    int aimY = (int)floorf(local.y);
    float best = 0.0f;
    bool found = false;
    int offsetY;

    for (offsetY = -reach; offsetY <= reach; ++offsetY) {
        int offsetX;

        for (offsetX = -reach; offsetX <= reach; ++offsetX) {
            int cellX = aimX + offsetX;
            int cellY = aimY + offsetY;
            float centreX;
            float centreY;
            float distance;

            if (cellX < 0 || cellY < 0 || cellX >= body->width ||
                cellY >= body->height) {
                continue;
            }
            if (DynamicTerrainCellAt(terrain, handle, cellX, cellY) ==
                MATERIAL_EMPTY) {
                continue;
            }
            centreX = (float)cellX + 0.5f;
            centreY = (float)cellY + 0.5f;
            distance = (centreX - local.x) * (centreX - local.x) +
                       (centreY - local.y) * (centreY - local.y);
            if (found && distance >= best) {
                continue;
            }
            found = true;
            best = distance;
            *localPoint = (Vector2){centreX, centreY};
        }
    }
    return found;
}

/* One pass of every live body against the player where they now stand. Returns
   true when anything touched. */
static bool TerrainInteractionResolveAll(TerrainInteractionSystem *system,
                                         Player *player,
                                         DynamicTerrainSystem *terrain)
{
    bool touched = false;
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (!terrain->bodies[slot].active) {
            continue;
        }
        if (TerrainInteractionResolve(system, player, terrain, slot)) {
            touched = true;
        }
    }
    return touched;
}

/* Walks the player along the path they took this frame instead of only asking
   where they ended up.

   A boosting player crosses ten cells in a frame. A slab three cells thick that
   happens to lie between where they were and where they are would never appear
   in a test of the final position, and they would pass through it without ever
   touching it. Splitting the movement into steps no longer than half the
   player's own radius means consecutive probes overlap, so nothing thin can lie
   between two of them.

   The first step that touches something is where the player stops: the
   remainder of the movement is exactly what the collision was there to
   prevent. */
static void TerrainInteractionSweep(TerrainInteractionSystem *system,
                                    Player *player,
                                    DynamicTerrainSystem *terrain)
{
    Vector2 target = player->position;
    float deltaX;
    float deltaY;
    float distance;
    int substeps;
    int substep;

    if (!system->hasPreviousPosition) {
        (void)TerrainInteractionResolveAll(system, player, terrain);
        system->previousPosition = player->position;
        system->hasPreviousPosition = true;
        return;
    }
    deltaX = target.x - system->previousPosition.x;
    deltaY = target.y - system->previousPosition.y;
    distance = sqrtf(deltaX * deltaX + deltaY * deltaY);

    substeps = 1;
    if (distance > player->radius * 0.5f) {
        substeps = (int)ceilf(distance / (player->radius * 0.5f));
        if (substeps > TERRAIN_INTERACTION_MAX_SUBSTEPS) {
            substeps = TERRAIN_INTERACTION_MAX_SUBSTEPS;
        }
        ++system->stats.sweptFrames;
        if (substeps > system->stats.maximumSubsteps) {
            system->stats.maximumSubsteps = substeps;
        }
    }

    for (substep = 1; substep <= substeps; ++substep) {
        float amount = (float)substep / (float)substeps;

        player->position = (Vector2){
            system->previousPosition.x + deltaX * amount,
            system->previousPosition.y + deltaY * amount};
        if (TerrainInteractionResolveAll(system, player, terrain)) {
            /* Stopped here. The resolve has already moved them clear and taken
               its share of the momentum; carrying on to the end of the path
               would undo both. */
            break;
        }
    }
    system->previousPosition = player->position;
}

/* Nearest live body to the aim point, within reach of the player. Slot order
   breaks ties, so the same standing position always offers the same rock. */
static TerrainBodyHandle TerrainInteractionPick(const DynamicTerrainSystem *terrain,
                                                const TerrainInteractionConfig *config,
                                                Vector2 playerAt, Vector2 aim,
                                                Vector2 *localPoint)
{
    TerrainBodyHandle best = TerrainBodyInvalidHandle();
    float bestDistance = 0.0f;
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *body = &terrain->bodies[slot];
        Vector2 minimum;
        Vector2 maximum;
        float nearestX;
        float nearestY;
        float reachX;
        float reachY;
        float toAim;

        if (!body->active || !TerrainBodyWorldBounds(body, &minimum, &maximum)) {
            continue;
        }
        nearestX = TerrainInteractionClamp(playerAt.x, minimum.x, maximum.x);
        nearestY = TerrainInteractionClamp(playerAt.y, minimum.y, maximum.y);
        reachX = nearestX - playerAt.x;
        reachY = nearestY - playerAt.y;
        if (reachX * reachX + reachY * reachY >
            config->grabDistance * config->grabDistance) {
            continue;
        }
        nearestX = TerrainInteractionClamp(aim.x, minimum.x, maximum.x);
        nearestY = TerrainInteractionClamp(aim.y, minimum.y, maximum.y);
        toAim = (nearestX - aim.x) * (nearestX - aim.x) +
                (nearestY - aim.y) * (nearestY - aim.y);
        /* Pointed at, not merely standing nearby. Taking hold of whatever
           happened to be within arm's reach while the player was clearly
           looking somewhere else is the kind of control that feels haunted. */
        if (toAim > config->grabDistance * config->grabDistance) {
            continue;
        }
        if (best.generation != 0u && toAim >= bestDistance) {
            continue;
        }
        bestDistance = toAim;
        best = (TerrainBodyHandle){(uint16_t)slot, body->generation};
        if (localPoint != NULL &&
            !TerrainInteractionGrabPoint(terrain,
                                         (TerrainBodyHandle){(uint16_t)slot,
                                                             body->generation},
                                         body, aim, localPoint)) {
            /* The aim landed on a hole in the raster and there was no material
               near enough to mean it. A bounding box is not a shape: holding a
               body by a gap in it would put the spring on nothing. */
            best = TerrainBodyInvalidHandle();
            bestDistance = 0.0f;
            continue;
        }
    }
    return best;
}

static void TerrainInteractionRelease(TerrainInteractionSystem *system,
                                      DynamicTerrainSystem *terrain,
                                      Vector2 playerAt, Vector2 aim)
{
    TerrainBodyHandle handle = system->held;
    TerrainBody *body = DynamicTerrainGet(terrain, handle);

    system->holding = false;
    system->held = TerrainBodyInvalidHandle();
    if (body == NULL) {
        ++system->stats.lostHolds;
        return;
    }
    ++system->stats.releases;

    {
        float dx = aim.x - playerAt.x;
        float dy = aim.y - playerAt.y;
        float length = sqrtf(dx * dx + dy * dy);
        Vector2 grabWorld;

        if (length < 0.001f || system->config.throwImpulse <= 0.0f) {
            return;
        }
        /* Along the aim, at the point being held, so a throw off the corner of
           a slab sets it spinning the way a shove there would. The impulse is
           not scaled by mass: that is what makes a chip throwable and a
           boulder merely droppable. */
        grabWorld = TerrainBodyLocalToWorld(body, system->holdLocalPoint.x,
                                            system->holdLocalPoint.y);
        DynamicTerrainApplyImpulse(terrain, handle,
                                   (Vector2){dx / length *
                                                 system->config.throwImpulse,
                                             dy / length *
                                                 system->config.throwImpulse},
                                   grabWorld);
        ++system->stats.throws;
    }
}

static bool TerrainInteractionHoldStillHasCell(const DynamicTerrainSystem *terrain,
                                              TerrainBodyHandle handle,
                                              Vector2 localPoint)
{
    const TerrainBody *body = DynamicTerrainGetConst(terrain, handle);
    int cellX = (int)floorf(localPoint.x);
    int cellY = (int)floorf(localPoint.y);

    if (body == NULL || cellX < 0 || cellY < 0 || cellX >= body->width ||
        cellY >= body->height) {
        return false;
    }
    return DynamicTerrainCellAt(terrain, handle, cellX, cellY) != MATERIAL_EMPTY;
}

/* Follows the held cell onto whichever body now owns it.

   A fracture copies each piece into a raster of the same size at the same local
   coordinates and leaves it exactly where it already was, so the cell being
   held is still at the same local point and still at the same world point — on
   a different body. Matching on both is enough to find it, and cheap enough to
   do with a pass over the slots. No mapping is recorded anywhere: this is a
   search over 32 candidates, run once, at the moment a hold would otherwise
   have to be dropped. */
static bool TerrainInteractionTransferHold(TerrainInteractionSystem *system,
                                           DynamicTerrainSystem *terrain)
{
    int slot;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *body = &terrain->bodies[slot];
        TerrainBodyHandle handle;
        Vector2 where;

        if (!body->active) {
            continue;
        }
        handle = (TerrainBodyHandle){(uint16_t)slot, body->generation};
        if (handle.index == system->held.index &&
            handle.generation == system->held.generation) {
            continue;
        }
        if (!TerrainInteractionHoldStillHasCell(terrain, handle,
                                                system->holdLocalPoint)) {
            continue;
        }
        where = TerrainBodyLocalToWorld(body, system->holdLocalPoint.x,
                                        system->holdLocalPoint.y);
        /* Half a cell: a piece that has just broken away has not moved yet, so
           anything further off is a different rock that merely happens to have
           material at the same raster coordinate. */
        if ((where.x - system->holdWorldPoint.x) *
                    (where.x - system->holdWorldPoint.x) +
                (where.y - system->holdWorldPoint.y) *
                    (where.y - system->holdWorldPoint.y) >
            0.25f) {
            continue;
        }
        system->held = handle;
        ++system->stats.transferredHolds;
        return true;
    }
    return false;
}

static void TerrainInteractionHold(TerrainInteractionSystem *system,
                                   DynamicTerrainSystem *terrain,
                                   Vector2 playerAt, Vector2 aim,
                                   float deltaTime)
{
    TerrainBody *body = DynamicTerrainGet(terrain, system->held);
    Vector2 grabWorld;
    Vector2 target;
    Vector2 pointVelocity;
    Vector2 force;
    float dx;
    float dy;
    float length;
    float magnitude;

    if (body == NULL || !TerrainInteractionHoldStillHasCell(terrain,
                                                             system->held,
                                                             system->holdLocalPoint)) {
        /* The cell being held is gone: carved away, or carried off by a piece
           that broke away and left this handle naming the remainder. A spring
           anchored to empty raster would drag the body by nothing. */
        if (!TerrainInteractionTransferHold(system, terrain)) {
            system->holding = false;
            system->held = TerrainBodyInvalidHandle();
            ++system->stats.lostHolds;
            return;
        }
        body = DynamicTerrainGet(terrain, system->held);
    }

    grabWorld = TerrainBodyLocalToWorld(body, system->holdLocalPoint.x,
                                        system->holdLocalPoint.y);
    system->holdWorldPoint = grabWorld;

    dx = aim.x - playerAt.x;
    dy = aim.y - playerAt.y;
    length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        target = (Vector2){playerAt.x, playerAt.y - system->config.holdDistance};
    } else {
        target = (Vector2){playerAt.x + dx / length * system->config.holdDistance,
                           playerAt.y + dy / length * system->config.holdDistance};
    }

    pointVelocity = TerrainBodyPointVelocity(body, grabWorld);
    /* A spring with damping, in absolute force units. Dividing by mass is what
       makes a heavy body follow the hand sluggishly and a light one snap to it,
       and it is the whole reason the hold is not simply a position assignment:
       a body that teleports to the cursor passes through walls, and this one
       has to be pushed there against everything in the way. */
    force.x = (target.x - grabWorld.x) * system->config.pullStrength -
              pointVelocity.x * system->config.pullDamping;
    force.y = (target.y - grabWorld.y) * system->config.pullStrength -
              pointVelocity.y * system->config.pullDamping;
    magnitude = sqrtf(force.x * force.x + force.y * force.y);
    if (magnitude > system->config.maxPullForce) {
        force.x = force.x / magnitude * system->config.maxPullForce;
        force.y = force.y / magnitude * system->config.maxPullForce;
    }

    DynamicTerrainApplyImpulse(terrain, system->held,
                               (Vector2){force.x * deltaTime,
                                         force.y * deltaTime}, grabWorld);
}

void TerrainInteractionUpdate(TerrainInteractionSystem *system, Player *player,
                              DynamicTerrainSystem *terrain, Vector2 aimWorld,
                              bool grabHeld, float deltaTime)
{
    Vector2 localPoint = {0.0f, 0.0f};

    if (system == NULL || player == NULL || terrain == NULL ||
        !(deltaTime > 0.0f)) {
        return;
    }

    /* Contact first: the hold should pull against a body that is already where
       the player will find it this frame. */
    TerrainInteractionSweep(system, player, terrain);

    system->hovered = TerrainInteractionPick(terrain, &system->config,
                                             player->position, aimWorld,
                                             &localPoint);

    if (!grabHeld) {
        if (system->holding) {
            TerrainInteractionRelease(system, terrain, player->position,
                                      aimWorld);
        }
        return;
    }
    if (!system->holding) {
        if (DynamicTerrainGetConst(terrain, system->hovered) == NULL) {
            return;
        }
        system->held = system->hovered;
        system->holdLocalPoint = localPoint;
        system->holding = true;
        ++system->stats.grabs;
    }
    TerrainInteractionHold(system, terrain, player->position, aimWorld,
                           deltaTime);
}
