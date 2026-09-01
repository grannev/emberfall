/* Ability blasts acting on terrain bodies. See terrain_impulse.h for why the
 * blasts are queued rather than applied where they are described.
 *
 * Three decisions shape this file, and all three are about keeping it small:
 *
 *   which bodies    a flat pass over the live slots. The body budget is a hard
 *                   32, so a broad phase would be more code than the scan it
 *                   replaced. Each body is rejected on its world-space box
 *                   before anything expensive happens to it.
 *
 *   where it lands  the point of that box nearest the blast. It costs four
 *                   comparisons, it is on the side of the body facing the
 *                   blast, and it is off the centre of mass whenever the body
 *                   is not squarely in front — which is exactly when a real
 *                   blast would spin something. Walking the raster for a truer
 *                   contact point would cost thousands of cell reads to move
 *                   the lever arm by a cell or two.
 *
 *   how hard        linear falloff to nothing at the rim, matching what the
 *                   same two powers already do to loose cells.
 */
#include "terrain_impulse.h"

#include <math.h>
#include <stddef.h>

void TerrainImpulseInit(TerrainImpulseSystem *system)
{
    if (system == NULL) {
        return;
    }
    system->blastCount = 0;
    TerrainImpulseResetStats(system);
}

void TerrainImpulseResetStats(TerrainImpulseSystem *system)
{
    if (system == NULL) {
        return;
    }
    {
        TerrainImpulseStats empty = {0, 0, 0, 0, 0, 0, 0, 0};

        system->stats = empty;
    }
}

/* Refused at the door rather than propagated. A non-finite blast would reach
   DynamicTerrainApplyImpulse, which refuses it too, but by then the caller has
   been told the shove happened. */
static bool TerrainBlastFinite(float value)
{
    return value == value && value > -1.0e9f && value < 1.0e9f;
}

static bool TerrainBlastIsUsable(const TerrainBlast *blast)
{
    if (!TerrainFiniteSample(blast->origin) ||
        !TerrainFiniteSample(blast->direction) ||
        !TerrainBlastFinite(blast->radius) ||
        !TerrainBlastFinite(blast->momentum) ||
        !TerrainBlastFinite(blast->spreadCosine)) {
        return false;
    }
    return blast->radius > 0.0f && blast->momentum > 0.0f;
}

bool TerrainImpulseQueueBlast(TerrainImpulseSystem *system, TerrainBlast blast)
{
    if (system == NULL || !TerrainBlastIsUsable(&blast)) {
        return false;
    }
    if (system->blastCount >= MAX_TERRAIN_BLASTS) {
        ++system->stats.blastsDropped;
        return false;
    }
    system->blasts[system->blastCount++] = blast;
    ++system->stats.blastsQueued;
    return true;
}

/* Coarse line of sight, and only for the cone blast. The force blast already
   refuses to shove cells on the far side of a wall, and a body is not exempt
   from that: a blow that throws a boulder through solid rock reads as the power
   passing through the world. The march is deliberately the same half-cell step
   the cell pass uses. The body's own cells are not in the world — extraction
   took them out — so nothing here can be blocked by its target. */
static bool TerrainBlastIsBlocked(const World *world, Vector2 origin,
                                  Vector2 contact)
{
    float deltaX = contact.x - origin.x;
    float deltaY = contact.y - origin.y;
    float distance = sqrtf(deltaX * deltaX + deltaY * deltaY);
    float travelled;

    if (world == NULL || distance < 1.0f) {
        return false;
    }
    deltaX /= distance;
    deltaY /= distance;
    /* Stopped a cell short of the contact point: the terrain the body is
       resting against must not be read as the wall that shields it. */
    for (travelled = 1.0f; travelled <= distance - 1.0f; travelled += 0.5f) {
        int sampleX = (int)floorf(origin.x + deltaX * travelled);
        int sampleY = (int)floorf(origin.y + deltaY * travelled);

        if (WorldMaterialIsSolid(WorldGetCell(world, sampleX, sampleY))) {
            return true;
        }
    }
    return false;
}

static float TerrainClampToRange(float value, float low, float high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

/* One blast against one body. Returns true when an impulse was delivered. */
static bool TerrainBlastHitsBody(TerrainImpulseSystem *system,
                                 DynamicTerrainSystem *terrain,
                                 const World *world, const TerrainBlast *blast,
                                 int slot)
{
    TerrainBody *body = &terrain->bodies[slot];
    Vector2 minimum;
    Vector2 maximum;
    Vector2 contact;
    Vector2 push;
    Vector2 impulse;
    float offsetX;
    float offsetY;
    float distance;
    float falloff;
    float magnitude;

    if (!TerrainBodyWorldBounds(body, &minimum, &maximum)) {
        return false;
    }
    contact.x = TerrainClampToRange(blast->origin.x, minimum.x, maximum.x);
    contact.y = TerrainClampToRange(blast->origin.y, minimum.y, maximum.y);
    offsetX = contact.x - blast->origin.x;
    offsetY = contact.y - blast->origin.y;
    distance = sqrtf(offsetX * offsetX + offsetY * offsetY);
    if (distance > blast->radius) {
        return false;
    }

    if (blast->shape == TERRAIN_BLAST_CONE) {
        /* A blast standing inside the body's own box has no meaningful angle to
           it, and refusing that case would make a point-blank blow the one that
           does nothing. */
        if (distance > 0.001f &&
            (offsetX * blast->direction.x + offsetY * blast->direction.y) <
                distance * blast->spreadCosine) {
            return false;
        }
        if (TerrainBlastIsBlocked(world, blast->origin, contact)) {
            ++system->stats.bodiesOccluded;
            return false;
        }
    }

    /* Outward from the blast, measured to the centre of mass rather than to the
       contact point: it is the body as a whole that is thrown, while the
       contact point decides how much of that throw becomes spin. */
    push.x = body->position.x - blast->origin.x;
    push.y = body->position.y - blast->origin.y;
    magnitude = sqrtf(push.x * push.x + push.y * push.y);
    if (magnitude < 0.001f) {
        /* Dead centre. Any direction is as defensible as any other, so the one
           chosen is fixed rather than drawn: a blast must not become a source of
           randomness in a simulation that replays. Up is the choice a player
           reads as an explosion lifting what sat on it. */
        push = (Vector2){0.0f, -1.0f};
    } else {
        push.x /= magnitude;
        push.y /= magnitude;
    }

    falloff = 1.0f - distance / blast->radius;
    magnitude = blast->momentum * falloff;
    impulse.x = push.x * magnitude;
    impulse.y = push.y * magnitude;

    /* Waking a body costs an awake slot and restarts its quiet spell, so it
       needs a reason. The threshold is not a tuned number: it is the speed the
       sleep rule already treats as standing still, and an impulse too weak to
       push the body past it has, by the simulation's own definition, not moved
       it. A body that is already awake is not tested — it is moving anyway, and
       refusing to add to that would be arbitrary. */
    if (!body->awake &&
        magnitude / body->mass < terrain->config.linearSleepSpeed) {
        ++system->stats.bodiesLeftSleeping;
        return false;
    }

    DynamicTerrainApplyImpulse(terrain, (TerrainBodyHandle){(uint16_t)slot,
                                                           body->generation},
                               impulse, contact);
    return true;
}

int TerrainImpulseApply(TerrainImpulseSystem *system,
                        DynamicTerrainSystem *terrain, const World *world)
{
    int applied = 0;
    int index;

    if (system == NULL || terrain == NULL) {
        return 0;
    }
    /* Slot order, every time. Two runs of the same scenario must reach the same
       velocities, and an iteration order that depended on anything else is the
       cheapest way to lose that. */
    for (index = 0; index < system->blastCount; ++index) {
        const TerrainBlast *blast = &system->blasts[index];
        int slot;

        ++system->stats.blastsApplied;
        for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
            if (!terrain->bodies[slot].active) {
                continue;
            }
            if (!TerrainBlastHitsBody(system, terrain, world, blast, slot)) {
                continue;
            }
            ++applied;
            ++system->stats.bodyImpulseApplications;
            if (blast->shape == TERRAIN_BLAST_CONE) {
                ++system->stats.bodiesAffectedByForce;
            } else {
                ++system->stats.bodiesAffectedByExplosion;
            }
        }
    }
    system->blastCount = 0;
    return applied;
}
