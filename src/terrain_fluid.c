/* Terrain bodies and a liquid. See terrain_fluid.h for the shape of it; this
 * file records the decisions inside.
 */
#include "terrain_fluid.h"

#include <math.h>
#include <stddef.h>

#include "materials.h"

/* Hysteresis on "in the liquid". */
#define TERRAIN_FLUID_ENTER_FRACTION 0.25f
#define TERRAIN_FLUID_LEAVE_FRACTION 0.05f
/* Strongest push a body's splash can give, in steps, and the widest, in
   cells of radius: a slab the size of a house dropped into a lake from a
   height throws water thirty cells up and the ring reaches the shore. */
#define TERRAIN_FLUID_SPLASH_MAX_STRENGTH 30
#define TERRAIN_FLUID_SPLASH_MAX_RADIUS 64.0f
/* Per second, on vertical and angular motion of a body riding the surface. */
#define TERRAIN_FLUID_HEAVE_DAMPING 4.0f

TerrainFluidConfig TerrainFluidDefaultConfig(void)
{
    TerrainFluidConfig config;

    config.linearDrag = 2.5f;
    config.angularDrag = 3.0f;
    config.lavaDragScale = 2.5f;
    config.splashSpeed = 30.0f;
    return config;
}

void TerrainFluidInit(TerrainFluidSystem *system)
{
    TerrainFluidStats empty = {0, 0, 0, 0};

    if (system == NULL) {
        return;
    }
    system->config = TerrainFluidDefaultConfig();
    system->stats = empty;
}

/* Fraction of the sampled surface cells that stand in liquid, and which
   liquid most of them stand in. */
static float TerrainFluidSubmerged(const DynamicTerrainSystem *terrain, int slot,
                                   const TerrainBody *body, const World *world,
                                   CellMaterial *liquid)
{
    size_t surfaceBase = (size_t)slot * (size_t)MAX_TERRAIN_BODY_CELLS;
    int stride = body->surfaceCount / TERRAIN_FLUID_SAMPLES + 1;
    int counts[MATERIAL_COUNT] = {0};
    int best = MATERIAL_EMPTY;
    int sampled = 0;
    float inLiquid = 0.0f;
    int index;

    for (index = 0; index < body->surfaceCount; index += stride) {
        Vector2 at = TerrainBodyLocalToWorld(
            body, (float)terrain->surfaceX[surfaceBase + (size_t)index] + 0.5f,
            (float)terrain->surfaceY[surfaceBase + (size_t)index] + 0.5f);
        int x = (int)floorf(at.x);
        int y = (int)floorf(at.y);
        CellMaterial material = WorldGetCell(world, x, y);
        CellMaterial below = WorldGetCell(world, x, y + 1);
        CellMaterial wet = MATERIAL_EMPTY;
        float waterline = 0.0f;
        float covered = 0.0f;

        ++sampled;
        /* How much of a cell-sized square round the sample is under the
           waterline, not whether its centre is: counted by centres, a small
           body's lift jumped between nothing and several times its weight
           as its bottom row crossed the surface, it had no position to float
           at, and it bobbed for ever. */
        if (MaterialIsLiquid(material)) {
            wet = material;
            waterline = MaterialIsLiquid(WorldGetCell(world, x, y - 1)) ? -1.0e9f : (float)y;
        } else if (MaterialIsLiquid(below)) {
            wet = below;
            waterline = (float)(y + 1);
        }
        if (wet != MATERIAL_EMPTY) {
            covered = at.y + 0.5f - waterline;
            covered = covered < 0.0f ? 0.0f : (covered > 1.0f ? 1.0f : covered);
        }
        if (covered > 0.0f) {
            inLiquid += covered;
            ++counts[wet];
            if (counts[wet] > counts[best]) {
                best = (int)wet;
            }
        }
    }
    *liquid = (CellMaterial)best;
    return sampled > 0 ? inLiquid / (float)sampled : 0.0f;
}

/* The splash a body makes is the water's to make: a crown thrown clear of
   the surface around it and a ring spreading out, sized by how fast it came
   and how big it is — the mass that went in is water that has to go
   somewhere, at once. No effect is drawn over it; what the player sees is
   the water. */
static void TerrainFluidSplash(TerrainFluidSystem *system, const TerrainBody *body,
                               World *world, GameEventBuffer *events,
                               CellMaterial liquid, float speed, bool entering)
{
    float size = sqrtf((float)body->cellCount);
    float radius = body->boundingRadius * 1.5f + 3.0f + speed / 40.0f;
    int strength = 3 + (int)(speed / 12.0f) + (int)(size / 3.0f);
    Vector2 direction = {0.0f, entering ? 1.0f : -1.0f};
    Vector2 at = body->position;

    if (radius > TERRAIN_FLUID_SPLASH_MAX_RADIUS) {
        radius = TERRAIN_FLUID_SPLASH_MAX_RADIUS;
    }
    if (strength > TERRAIN_FLUID_SPLASH_MAX_STRENGTH) {
        strength = TERRAIN_FLUID_SPLASH_MAX_STRENGTH;
    }
    if (speed > 0.0f) {
        direction.x = body->velocity.x / speed;
        direction.y = body->velocity.y / speed;
    }
    /* From the surface the body broke, not its centre: the crown rises from
       where the water was. */
    {
        int x = (int)floorf(at.x);
        int y = (int)floorf(at.y);
        int probe;

        for (probe = y - (int)ceilf(body->boundingRadius) - 2;
             probe <= y + (int)ceilf(body->boundingRadius) + 2; ++probe) {
            if (MaterialIsLiquid(WorldGetCell(world, x, probe))) {
                at.y = (float)probe + 0.5f;
                break;
            }
        }
    }
    system->stats.cellsPushed += WorldSplashLiquid(world, at, radius, strength);
    (void)GameEventsPush(events, (GameEvent){
        .type = GAME_EVENT_LIQUID_SPLASH,
        .position = body->position,
        .direction = direction,
        .strength = speed,
        .radius = radius,
        .material = liquid,
        .count = body->cellCount,
    });
}

/* A moving body pushes the liquid out of the cells it occupies: every surface
   cell of the body that stands in liquid shoves that liquid away from the
   body's centre, and as the body moves its surface sweeps through the water
   ahead of it. What is displaced is therefore the body's own outline, cell
   for cell — a boulder shoves a boulder's worth of water aside, a pebble a
   pebble's, and neither merely sinks through it. Bounded by the surface
   sample count per step. */
#define TERRAIN_FLUID_DISPLACE_SAMPLES 512
#define TERRAIN_FLUID_DISPLACE_SPEED 6.0f

static void TerrainFluidDisplace(TerrainFluidSystem *system,
                                 const DynamicTerrainSystem *terrain, int slot,
                                 const TerrainBody *body, World *world, float speed)
{
    size_t surfaceBase = (size_t)slot * (size_t)MAX_TERRAIN_BODY_CELLS;
    int stride = body->surfaceCount / TERRAIN_FLUID_DISPLACE_SAMPLES + 1;
    int strength = 2 + (int)(speed / 30.0f) + (int)(sqrtf((float)body->cellCount) / 4.0f);
    int index;

    if (strength > 16) {
        strength = 16;
    }
    for (index = 0; index < body->surfaceCount; index += stride) {
        Vector2 at = TerrainBodyLocalToWorld(
            body, (float)terrain->surfaceX[surfaceBase + (size_t)index] + 0.5f,
            (float)terrain->surfaceY[surfaceBase + (size_t)index] + 0.5f);
        int x = (int)floorf(at.x);
        int y = (int)floorf(at.y);
        float dx = at.x - body->position.x;
        float dy = at.y - body->position.y;
        int directionX;
        int directionY;

        if (!MaterialIsLiquid(WorldGetCell(world, x, y))) {
            continue;
        }
        /* Away from the centre, in eight directions; a cell on the centre
           line goes with the body's motion instead. */
        directionX = fabsf(dx) * 2.0f > fabsf(dy) ? (dx < 0.0f ? -1 : 1) : 0;
        directionY = fabsf(dy) * 2.0f > fabsf(dx) ? (dy < 0.0f ? -1 : 1) : 0;
        if (directionX == 0 && directionY == 0) {
            directionX = body->velocity.x < 0.0f ? -1 : (body->velocity.x > 0.0f ? 1 : 0);
            directionY = body->velocity.y < 0.0f ? -1 : 1;
        }
        if (WorldPushLiquid(world, x, y, directionX, directionY, strength)) {
            ++system->stats.cellsPushed;
        }
    }
}

void TerrainFluidUpdate(TerrainFluidSystem *system, DynamicTerrainSystem *terrain,
                        World *world, GameEventBuffer *events, float deltaTime)
{
    int slot;

    if (system == NULL || terrain == NULL || terrain->material == NULL ||
        world == NULL || world->cells == NULL || !TerrainStepIsUsable(deltaTime)) {
        return;
    }
    system->stats.bodiesInLiquid = 0;

    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        TerrainBody *body = &terrain->bodies[slot];
        CellMaterial liquid;
        float fraction;
        float speed;
        float dragScale;

        if (!body->active || !body->awake || body->cellCount <= 0 ||
            !(body->mass > 0.0f)) {
            continue;
        }
        fraction = TerrainFluidSubmerged(terrain, slot, body, world, &liquid);
        body->submerged = fraction;
        speed = sqrtf(body->velocity.x * body->velocity.x +
                      body->velocity.y * body->velocity.y);

        if (!body->inLiquid && fraction >= TERRAIN_FLUID_ENTER_FRACTION) {
            body->inLiquid = true;
            ++system->stats.entries;
            if (speed >= system->config.splashSpeed && events != NULL) {
                TerrainFluidSplash(system, body, world, events, liquid, speed, true);
            }
        } else if (body->inLiquid && fraction <= TERRAIN_FLUID_LEAVE_FRACTION) {
            body->inLiquid = false;
            ++system->stats.exits;
            if (speed >= system->config.splashSpeed && events != NULL) {
                TerrainFluidSplash(system, body, world, events, liquid, speed, false);
            }
        }
        if (fraction <= 0.0f) {
            continue;
        }
        ++system->stats.bodiesInLiquid;
        if (speed >= TERRAIN_FLUID_DISPLACE_SPEED) {
            TerrainFluidDisplace(system, terrain, slot, body, world, speed);
        }

        /* Drag first, in proportion to how much is under, then buoyancy.
           The integrator adds gravity after this and damps almost nothing,
           so the order is what makes the two weights compare fairly: drag
           applied after the buoyant push ate part of it every step and left
           an ice floe with half the lift it was owed, which was under the
           sleep threshold, and it dozed off under water. */
        dragScale = fraction * (liquid == MATERIAL_LAVA ? system->config.lavaDragScale
                                                        : 1.0f);
        {
            float linear = expf(-system->config.linearDrag * dragScale * deltaTime);
            float angular = expf(-system->config.angularDrag * dragScale * deltaTime);

            body->velocity.x *= linear;
            body->velocity.y *= linear;
            body->angularVelocity *= angular;
        }
        /* Buoyancy: the weight of the liquid the submerged part displaces,
           upward, against the body's own weight, which the integrator adds.
           Mass is density summed over cells, so the two compare as densities
           and nothing but density decides who floats. */
        {
            float displaced = fraction * (float)body->cellCount *
                              MaterialAt(liquid)->density;
            float gravity = terrain->config.gravity *
                            WorldGravityScaleAt(world, body->position.y);

            body->velocity.y -= gravity * (displaced / body->mass) * deltaTime;
        }
        /* Heave damping at the waterline. The fraction under is read from a
           handful of surface cells, so for a small body it moves in steps,
           and the lift with it: a chip rode up out of the water on one step,
           fell back through the next and bobbed for ever. A floating body
           in real water loses its bobbing to the waves it makes; this is
           that loss, and it is what lets it lie still and sleep. */
        if (fraction < 0.98f) {
            float heave = expf(-TERRAIN_FLUID_HEAVE_DAMPING * deltaTime);

            body->velocity.y *= heave;
            body->angularVelocity *= heave;
        }
    }
}
