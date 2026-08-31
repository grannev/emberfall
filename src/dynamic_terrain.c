/* Storage and bookkeeping for detached pieces of terrain. See
 * dynamic_terrain.h for the model; this file records the two decisions that
 * shape the implementation.
 *
 * Storage is a fixed raster per body slot, out of one arena allocated at init.
 * A shared arena with variable-size allocations would pack tighter, but freeing
 * bodies in arbitrary order fragments it, and the ways out of that —
 * compaction, free lists, slabs — all cost more code and more failure modes
 * than the memory they save. Thirty-two slots of 8192 cells is 1.25 MiB, which
 * next to a 167 MiB world is not worth a single line of allocator.
 *
 * Mass is derived from material density and cell area. The units are relative,
 * not kilograms: nothing here needs an absolute scale, only for a slab of rock
 * to fall harder and turn more slowly than the same slab of ice.
 */
#include "dynamic_terrain.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "materials.h"

/* A cell is one unit square, so its area is one and its mass is its density.
   Its own moment of inertia about its centre is m * (w^2 + h^2) / 12 = m / 6.
   Including that term is what stops a one-cell body having zero inertia and
   therefore infinite angular acceleration the moment anything touches it. */
#define TERRAIN_CELL_SELF_INERTIA (1.0f / 6.0f)

/* Generation zero is reserved as "never live" so that a zero-initialised
   handle cannot name body zero by accident. */
#define TERRAIN_BODY_FIRST_GENERATION 1u

/* Starting values, not settled ones. Gravity sits between the 18-30 cells/s^2
   that particle debris uses and something that reads as a slab of rock rather
   than grit; it will want retuning once EF-DYN-005 makes bodies visible, and
   the point of gathering it here is that retuning is then one edit.

   linearSleepSpeed is deliberately under gravity * (1/60) = 2.0, so a body in
   free fall can never satisfy the sleep condition. Until collision exists there
   is nothing to hold a body still, so sleep is mostly inert; it matters from
   EF-DYN-006 onward, when a body resting on the ground has its gravity
   cancelled. */
DynamicTerrainConfig DynamicTerrainDefaultConfig(void)
{
    DynamicTerrainConfig config;

    config.gravity = 120.0f;
    config.linearDamping = 0.15f;
    config.angularDamping = 0.40f;
    /* Chosen against the collision substep budget rather than for feel: at the
       60 Hz step these keep a body of ordinary size inside what
       TERRAIN_MAX_SUBSTEPS can cover, so it cannot step over a one-cell wall.
       TerrainPhysicsConfigIsSafe states the relation and a test holds it. */
    config.maximumSpeed = 300.0f;
    config.maximumAngularSpeed = 2.2f;
    config.linearSleepSpeed = 1.5f;
    config.angularSleepSpeed = 0.05f;
    config.sleepDelay = 0.5f;
    config.restitution = 0.08f;
    config.friction = 0.55f;
    return config;
}

static bool TerrainSlotIsLive(const DynamicTerrainSystem *system,
                              TerrainBodyHandle handle)
{
    if (system == NULL || handle.index >= MAX_TERRAIN_BODIES ||
        handle.generation == 0u) {
        return false;
    }
    return system->bodies[handle.index].active &&
           system->bodies[handle.index].generation == handle.generation;
}

static size_t TerrainRasterBase(uint16_t index)
{
    return (size_t)index * (size_t)TERRAIN_BODY_RASTER_CAPACITY;
}

bool DynamicTerrainInit(DynamicTerrainSystem *system)
{
    int index;

    if (system == NULL) {
        return false;
    }
    memset(system, 0, sizeof(*system));
    system->config = DynamicTerrainDefaultConfig();
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        system->bodies[index].generation = TERRAIN_BODY_FIRST_GENERATION;
    }
    system->material = calloc((size_t)MAX_TERRAIN_RASTER_CELLS,
                              sizeof(*system->material));
    system->temperature = calloc((size_t)MAX_TERRAIN_RASTER_CELLS,
                                 sizeof(*system->temperature));
    system->surfaceX = calloc((size_t)MAX_TERRAIN_BODIES *
                                  (size_t)MAX_TERRAIN_BODY_CELLS,
                              sizeof(*system->surfaceX));
    system->surfaceY = calloc((size_t)MAX_TERRAIN_BODIES *
                                  (size_t)MAX_TERRAIN_BODY_CELLS,
                              sizeof(*system->surfaceY));
    if (system->material == NULL || system->temperature == NULL ||
        system->surfaceX == NULL || system->surfaceY == NULL) {
        DynamicTerrainUnload(system);
        return false;
    }
    return true;
}

void DynamicTerrainUnload(DynamicTerrainSystem *system)
{
    if (system == NULL) {
        return;
    }
    free(system->material);
    free(system->temperature);
    free(system->surfaceX);
    free(system->surfaceY);
    memset(system, 0, sizeof(*system));
}

void DynamicTerrainReset(DynamicTerrainSystem *system)
{
    int index;

    if (system == NULL || system->material == NULL) {
        return;
    }
    /* Generations are deliberately not reset. A handle held across a world
       regeneration must not come back to life pointing at a body that happens
       to land in the same slot. */
    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        if (system->bodies[index].active) {
            DynamicTerrainFreeBody(system, (TerrainBodyHandle){
                (uint16_t)index, system->bodies[index].generation});
        }
    }
    system->stats.activeBodies = 0;
    system->stats.allocatedDynamicCells = 0;
    system->stats.awakeBodies = 0;
    system->stats.sleepingBodies = 0;
}

TerrainBodyHandle DynamicTerrainAllocBody(DynamicTerrainSystem *system,
                                          int width, int height)
{
    TerrainBodyHandle handle = TerrainBodyInvalidHandle();
    TerrainBody *body;
    int index;

    if (system == NULL || system->material == NULL) {
        return handle;
    }
    if (width <= 0 || height <= 0 || width > TERRAIN_BODY_MAX_SPAN ||
        height > TERRAIN_BODY_MAX_SPAN ||
        width * height > TERRAIN_BODY_RASTER_CAPACITY) {
        return handle;
    }

    for (index = 0; index < MAX_TERRAIN_BODIES; ++index) {
        if (!system->bodies[index].active) {
            break;
        }
    }
    if (index >= MAX_TERRAIN_BODIES) {
        /* Full. Refusing is the whole point of a hard budget: the caller
           decides what to do without a body, and nothing grows. */
        return handle;
    }

    body = &system->bodies[index];
    /* Keep the generation; everything else starts clean. */
    {
        uint16_t generation = body->generation;

        memset(body, 0, sizeof(*body));
        body->generation = generation;
    }
    body->active = true;
    body->awake = true;
    body->sleepTimer = 0.0f;
    body->width = width;
    body->height = height;
    /* An empty body has no extent. Finalize will set real bounds once cells
       arrive; until then the inverted range says "nothing here". */
    body->minimumX = 0;
    body->minimumY = 0;
    body->maximumX = -1;
    body->maximumY = -1;

    memset(system->material + TerrainRasterBase((uint16_t)index), 0,
           (size_t)(width * height) * sizeof(*system->material));
    memset(system->temperature + TerrainRasterBase((uint16_t)index), 0,
           (size_t)(width * height) * sizeof(*system->temperature));

    ++system->stats.activeBodies;
    system->stats.allocatedDynamicCells += width * height;
    if (system->stats.activeBodies > system->stats.peakBodies) {
        system->stats.peakBodies = system->stats.activeBodies;
    }
    if (system->stats.allocatedDynamicCells > system->stats.peakDynamicCells) {
        system->stats.peakDynamicCells = system->stats.allocatedDynamicCells;
    }

    handle.index = (uint16_t)index;
    handle.generation = body->generation;
    return handle;
}

void DynamicTerrainFreeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle)
{
    TerrainBody *body;

    if (!TerrainSlotIsLive(system, handle)) {
        return;
    }
    body = &system->bodies[handle.index];
    system->stats.allocatedDynamicCells -= body->width * body->height;
    --system->stats.activeBodies;
    body->active = false;
    /* Bumping on free is what makes every outstanding handle to this body stale
       from this moment, including the one that did the freeing. Zero is
       skipped: it is the value a zero-initialised handle carries. */
    ++body->generation;
    if (body->generation == 0u) {
        body->generation = TERRAIN_BODY_FIRST_GENERATION;
    }
}

TerrainBody *DynamicTerrainGet(DynamicTerrainSystem *system,
                               TerrainBodyHandle handle)
{
    if (!TerrainSlotIsLive(system, handle)) {
        return NULL;
    }
    return &system->bodies[handle.index];
}

const TerrainBody *DynamicTerrainGetConst(const DynamicTerrainSystem *system,
                                          TerrainBodyHandle handle)
{
    if (!TerrainSlotIsLive(system, handle)) {
        return NULL;
    }
    return &system->bodies[handle.index];
}

/* Resolves a local coordinate to an arena index, or returns false when the
   handle is dead or the coordinate is outside the body's raster. */
static bool TerrainCellIndex(const DynamicTerrainSystem *system,
                             TerrainBodyHandle handle, int localX, int localY,
                             size_t *index)
{
    const TerrainBody *body;

    if (!TerrainSlotIsLive(system, handle)) {
        return false;
    }
    body = &system->bodies[handle.index];
    if (localX < 0 || localY < 0 || localX >= body->width ||
        localY >= body->height) {
        return false;
    }
    *index = TerrainRasterBase(handle.index) +
             (size_t)localY * (size_t)body->width + (size_t)localX;
    return true;
}

void DynamicTerrainSetCell(DynamicTerrainSystem *system, TerrainBodyHandle handle,
                           int localX, int localY, CellMaterial material,
                           float temperature)
{
    TerrainBody *body;
    size_t index;
    bool wasOccupied;
    bool isOccupied;

    if (!TerrainCellIndex(system, handle, localX, localY, &index)) {
        return;
    }
    if (material < 0 || material >= MATERIAL_COUNT) {
        return;
    }
    body = &system->bodies[handle.index];
    wasOccupied = system->material[index] != (uint8_t)MATERIAL_EMPTY;
    isOccupied = material != MATERIAL_EMPTY;

    system->material[index] = (uint8_t)material;
    system->temperature[index] = isOccupied ? temperature : 0.0f;
    if (isOccupied && !wasOccupied) {
        ++body->cellCount;
    } else if (!isOccupied && wasOccupied) {
        --body->cellCount;
    }
}

CellMaterial DynamicTerrainCellAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY)
{
    size_t index;

    if (!TerrainCellIndex(system, handle, localX, localY, &index)) {
        return MATERIAL_EMPTY;
    }
    return (CellMaterial)system->material[index];
}

float DynamicTerrainTemperatureAt(const DynamicTerrainSystem *system,
                                  TerrainBodyHandle handle, int localX, int localY)
{
    size_t index;

    if (!TerrainCellIndex(system, handle, localX, localY, &index)) {
        return 0.0f;
    }
    return system->temperature[index];
}

void DynamicTerrainFinalizeBody(DynamicTerrainSystem *system,
                                TerrainBodyHandle handle)
{
    TerrainBody *body;
    size_t base;
    float mass = 0.0f;
    float momentX = 0.0f;
    float momentY = 0.0f;
    float inertia = 0.0f;
    int occupied = 0;
    int localY;

    if (!TerrainSlotIsLive(system, handle)) {
        return;
    }
    body = &system->bodies[handle.index];
    base = TerrainRasterBase(handle.index);
    body->minimumX = body->width;
    body->minimumY = body->height;
    body->maximumX = -1;
    body->maximumY = -1;

    /* First pass: extent, total mass and first moments. */
    for (localY = 0; localY < body->height; ++localY) {
        int localX;

        for (localX = 0; localX < body->width; ++localX) {
            size_t index = base + (size_t)localY * (size_t)body->width +
                           (size_t)localX;
            CellMaterial material = (CellMaterial)system->material[index];
            float cellMass;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            ++occupied;
            if (localX < body->minimumX) body->minimumX = localX;
            if (localX > body->maximumX) body->maximumX = localX;
            if (localY < body->minimumY) body->minimumY = localY;
            if (localY > body->maximumY) body->maximumY = localY;

            cellMass = MaterialAt(material)->density;
            mass += cellMass;
            /* Cell centres, so a single cell's centre of mass is its middle
               rather than its corner. */
            momentX += cellMass * ((float)localX + 0.5f);
            momentY += cellMass * ((float)localY + 0.5f);
        }
    }

    body->cellCount = occupied;
    body->mass = mass;
    if (occupied == 0 || mass <= 0.0f) {
        body->minimumX = 0;
        body->minimumY = 0;
        body->maximumX = -1;
        body->maximumY = -1;
        body->centerOfMass = (Vector2){0.0f, 0.0f};
        body->inertia = 0.0f;
        body->boundingRadius = 0.0f;
        body->surfaceCount = 0;
        return;
    }
    body->centerOfMass = (Vector2){momentX / mass, momentY / mass};

    /* Second pass: inertia needs the centre of mass, so it cannot share the
       first. Parallel axis theorem per cell, including each cell's own moment
       about its centre. */
    for (localY = 0; localY < body->height; ++localY) {
        int localX;

        for (localX = 0; localX < body->width; ++localX) {
            size_t index = base + (size_t)localY * (size_t)body->width +
                           (size_t)localX;
            CellMaterial material = (CellMaterial)system->material[index];
            float cellMass;
            float dx;
            float dy;

            if (material == MATERIAL_EMPTY) {
                continue;
            }
            cellMass = MaterialAt(material)->density;
            dx = (float)localX + 0.5f - body->centerOfMass.x;
            dy = (float)localY + 0.5f - body->centerOfMass.y;
            inertia += cellMass * (dx * dx + dy * dy + TERRAIN_CELL_SELF_INERTIA);
        }
    }
    body->inertia = inertia;

    /* Third pass: the surface, and how far it reaches. Both are pure functions
       of the raster, so computing them once here is what keeps collision from
       rediscovering them every substep. */
    {
        size_t surfaceBase = (size_t)handle.index * (size_t)MAX_TERRAIN_BODY_CELLS;
        float farthest = 0.0f;

        body->surfaceCount = 0;
        for (localY = 0; localY < body->height; ++localY) {
            int localX;

            for (localX = 0; localX < body->width; ++localX) {
                static const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
                size_t index = base + (size_t)localY * (size_t)body->width +
                               (size_t)localX;
                bool exposed = false;
                float dx;
                float dy;
                int corner;
                int i;

                if (system->material[index] == (uint8_t)MATERIAL_EMPTY) {
                    continue;
                }
                for (i = 0; i < 4 && !exposed; ++i) {
                    int neighbourX = localX + offsets[i][0];
                    int neighbourY = localY + offsets[i][1];

                    /* A cell against the edge of the raster is exposed: there is
                       no body beyond it to shield it. */
                    if (neighbourX < 0 || neighbourY < 0 ||
                        neighbourX >= body->width || neighbourY >= body->height) {
                        exposed = true;
                        break;
                    }
                    if (system->material[base +
                                         (size_t)neighbourY * (size_t)body->width +
                                         (size_t)neighbourX] ==
                        (uint8_t)MATERIAL_EMPTY) {
                        exposed = true;
                    }
                }
                if (!exposed) {
                    continue;
                }
                system->surfaceX[surfaceBase + (size_t)body->surfaceCount] =
                    (int16_t)localX;
                system->surfaceY[surfaceBase + (size_t)body->surfaceCount] =
                    (int16_t)localY;
                ++body->surfaceCount;

                /* The farthest corner of this cell, not its centre: rotation
                   carries corners, and an underestimate here would let a
                   spinning body's edge outrun its substep budget. */
                for (corner = 0; corner < 4; ++corner) {
                    float cornerX = (float)localX + (float)(corner & 1);
                    float cornerY = (float)localY + (float)((corner >> 1) & 1);
                    float distance;

                    dx = cornerX - body->centerOfMass.x;
                    dy = cornerY - body->centerOfMass.y;
                    distance = sqrtf(dx * dx + dy * dy);
                    if (distance > farthest) {
                        farthest = distance;
                    }
                }
            }
        }
        body->boundingRadius = farthest;
    }
}


/* ---- kinematics ---------------------------------------------------------
 *
 * Semi-implicit Euler: velocity is advanced first and then used to advance
 * position. It is the standard choice for this and it is stable where explicit
 * Euler is not, at no extra cost.
 *
 * A whole body moves as one transform. Nothing here walks a body's raster —
 * that is the property that keeps a scene of bodies costing what the bodies
 * cost rather than what their cells cost, and it must survive every later
 * change to this file.
 */

static bool TerrainFinite(float value)
{
    return value == value && value > -HUGE_VALF && value < HUGE_VALF;
}

static bool TerrainFiniteVector(Vector2 value)
{
    return TerrainFinite(value.x) && TerrainFinite(value.y);
}

/* Keeps the angle in (-PI, PI]. Left to grow, a body spinning for a few minutes
   loses the precision that makes its rotation smooth. */
static float TerrainNormaliseAngle(float angle)
{
    while (angle > PI) {
        angle -= 2.0f * PI;
    }
    while (angle <= -PI) {
        angle += 2.0f * PI;
    }
    return angle;
}

static void TerrainClampSpeeds(TerrainBody *body,
                               const DynamicTerrainConfig *config)
{
    float speed = sqrtf(body->velocity.x * body->velocity.x +
                        body->velocity.y * body->velocity.y);

    if (config->maximumSpeed > 0.0f && speed > config->maximumSpeed) {
        float scale = config->maximumSpeed / speed;

        body->velocity.x *= scale;
        body->velocity.y *= scale;
    }
    if (config->maximumAngularSpeed > 0.0f) {
        if (body->angularVelocity > config->maximumAngularSpeed) {
            body->angularVelocity = config->maximumAngularSpeed;
        } else if (body->angularVelocity < -config->maximumAngularSpeed) {
            body->angularVelocity = -config->maximumAngularSpeed;
        }
    }
}

void DynamicTerrainIntegrateBody(DynamicTerrainSystem *system, TerrainBody *body,
                                 float deltaTime)
{
    const DynamicTerrainConfig *config;
    float linearRetained;
    float angularRetained;

    if (system == NULL || body == NULL || !TerrainStepIsUsable(deltaTime)) {
        return;
    }
    config = &system->config;
    /* exp(-k * dt) rather than a per-call multiplier: the fraction shed depends
       on elapsed time, so halving the step and doubling the count gives the
       same answer. */
    linearRetained = expf(-config->linearDamping * deltaTime);
    angularRetained = expf(-config->angularDamping * deltaTime);

    body->velocity.y += config->gravity * deltaTime;
    body->velocity.x *= linearRetained;
    body->velocity.y *= linearRetained;
    body->angularVelocity *= angularRetained;
    TerrainClampSpeeds(body, config);

    body->position.x += body->velocity.x * deltaTime;
    body->position.y += body->velocity.y * deltaTime;
    body->angle = TerrainNormaliseAngle(body->angle +
                                        body->angularVelocity * deltaTime);
}

void DynamicTerrainSettleBody(DynamicTerrainSystem *system, TerrainBody *body,
                              float deltaTime)
{
    const DynamicTerrainConfig *config;
    float speed;

    if (system == NULL || body == NULL || !TerrainStepIsUsable(deltaTime)) {
        return;
    }
    config = &system->config;
    speed = sqrtf(body->velocity.x * body->velocity.x +
                  body->velocity.y * body->velocity.y);

    if (speed >= config->linearSleepSpeed ||
        fabsf(body->angularVelocity) >= config->angularSleepSpeed) {
        body->sleepTimer = 0.0f;
        return;
    }
    body->sleepTimer += deltaTime;
    if (body->sleepTimer >= config->sleepDelay) {
        /* Zeroing on the way down leaves no residual drift for a body that is
           no longer being integrated, so a sleeping transform is exactly the
           one a reader sees. */
        body->awake = false;
        body->sleepTimer = 0.0f;
        body->velocity = (Vector2){0.0f, 0.0f};
        body->angularVelocity = 0.0f;
    }
}

void DynamicTerrainWakeBody(DynamicTerrainSystem *system, TerrainBodyHandle handle)
{
    TerrainBody *body = DynamicTerrainGet(system, handle);

    if (body == NULL) {
        return;
    }
    body->awake = true;
    body->sleepTimer = 0.0f;
}

void DynamicTerrainSetVelocity(DynamicTerrainSystem *system,
                               TerrainBodyHandle handle, Vector2 velocity,
                               float angularVelocity)
{
    TerrainBody *body = DynamicTerrainGet(system, handle);

    if (body == NULL || !TerrainFiniteVector(velocity) ||
        !TerrainFinite(angularVelocity)) {
        return;
    }
    body->velocity = velocity;
    body->angularVelocity = angularVelocity;
    TerrainClampSpeeds(body, &system->config);
    body->awake = true;
    body->sleepTimer = 0.0f;
}

void DynamicTerrainApplyImpulse(DynamicTerrainSystem *system,
                                TerrainBodyHandle handle, Vector2 impulse,
                                Vector2 worldPoint)
{
    TerrainBody *body = DynamicTerrainGet(system, handle);
    float leverX;
    float leverY;

    if (body == NULL || !TerrainFiniteVector(impulse) ||
        !TerrainFiniteVector(worldPoint)) {
        return;
    }
    /* A body with no mass cannot be pushed and a body with no inertia cannot be
       turned. Neither can arise from an extraction — every solid material has a
       density — but dividing by them would poison the transform with NaN, and a
       NaN body never recovers. */
    if (!(body->mass > 0.0f) || !(body->inertia > 0.0f)) {
        return;
    }

    body->velocity.x += impulse.x / body->mass;
    body->velocity.y += impulse.y / body->mass;

    /* The 2D cross product of the lever arm with the impulse. Applying an
       impulse through the centre of mass gives a zero lever and therefore no
       spin, which is the correct special case rather than one worth writing. */
    leverX = worldPoint.x - body->position.x;
    leverY = worldPoint.y - body->position.y;
    body->angularVelocity += (leverX * impulse.y - leverY * impulse.x) /
                             body->inertia;

    TerrainClampSpeeds(body, &system->config);
    body->awake = true;
    body->sleepTimer = 0.0f;
}

const DynamicTerrainStats *DynamicTerrainStatistics(const DynamicTerrainSystem *system)
{
    static const DynamicTerrainStats empty = {0};

    return system != NULL ? &system->stats : &empty;
}
