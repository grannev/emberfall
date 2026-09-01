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
    system->holding = false;
    TerrainInteractionResetStats(system);
}

void TerrainInteractionResetStats(TerrainInteractionSystem *system)
{
    if (system == NULL) {
        return;
    }
    {
        TerrainInteractionStats empty = {0, 0, 0, 0, 0, 0};

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
        if (localPoint != NULL) {
            /* Where the aim lands on the body, clamped into the cells it
               actually occupies so the hold is never on empty raster. */
            Vector2 local = TerrainBodyWorldToLocal(body, aim.x, aim.y);

            localPoint->x = TerrainInteractionClamp(local.x,
                                                    (float)body->minimumX,
                                                    (float)body->maximumX + 1.0f);
            localPoint->y = TerrainInteractionClamp(local.y,
                                                    (float)body->minimumY,
                                                    (float)body->maximumY + 1.0f);
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

    if (body == NULL) {
        /* Carved away, or fractured into something this handle no longer
           names. Generation handles make that a clean answer rather than a
           dangling pointer. */
        system->holding = false;
        system->held = TerrainBodyInvalidHandle();
        ++system->stats.lostHolds;
        return;
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
    int slot;

    if (system == NULL || player == NULL || terrain == NULL ||
        !(deltaTime > 0.0f)) {
        return;
    }

    /* Contact first: the hold should pull against a body that is already where
       the player will find it this frame. */
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        if (!terrain->bodies[slot].active) {
            continue;
        }
        (void)TerrainInteractionResolve(system, player, terrain, slot);
    }

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
