#include "particles.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <raymath.h>

static float RandomUnit(ParticleSystem *system)
{
    return RngFloat(&system->rng, 0.0f, 1.0f);
}

/* Returns the particle so callers can override its contact behaviour; passing
   every field through the argument list would make the spawn helpers unreadable
   for the two of them that actually differ. */
static Particle *ParticlesSpawnOne(ParticleSystem *system, Vector2 position,
                                   Vector2 velocity, Color color, float life,
                                   float size, float gravity)
{
    Particle *particle;

    particle = &system->particles[system->nextParticle];
    system->nextParticle = (system->nextParticle + 1) % MAX_PARTICLES;
    particle->position = position;
    particle->velocity = velocity;
    particle->color = color;
    particle->life = life;
    particle->maxLife = life;
    particle->size = size;
    particle->gravity = gravity;
    particle->restitution = 0.34f;
    particle->emission = 0.0f;
    particle->contact = PARTICLE_CONTACT_PASS;
    particle->settleMaterial = MATERIAL_EMPTY;
    particle->active = true;
    return particle;
}

static bool ParticleCellBlocks(const World *world, float x, float y)
{
    return WorldMaterialIsSolid(WorldGetCell(world, (int)floorf(x), (int)floorf(y)));
}

/* Debris that stops moving becomes grit on the tunnel floor. Only empty cells
   are written, so settling can never overwrite terrain or bury the player, and
   the drill removes far more than the surviving fraction puts back. */
static void ParticleSettle(Particle *particle, World *world)
{
    int x = (int)floorf(particle->position.x);
    int y = (int)floorf(particle->position.y);

    if (WorldGetCell(world, x, y) == MATERIAL_EMPTY) {
        WorldSetCell(world, x, y, particle->settleMaterial);
    }
    particle->active = false;
}

void ParticlesInit(ParticleSystem *system, uint64_t seed)
{
    if (system == NULL) {
        return;
    }
    memset(system, 0, sizeof(*system));
    RngSeed(&system->rng, seed);
}

/* Integrates one particle and returns where it wants to be next. Shared by
   both roles so their motion cannot drift apart. */
static void ParticleIntegrate(Particle *particle, const World *world,
                              float deltaTime, float *nextX, float *nextY)
{
    /* Debris in the weightless band drifts instead of falling, exactly as a
       slab does: the two must not disagree about where gravity stops. */
    particle->velocity.y += particle->gravity *
                            WorldGravityScaleAt(world, particle->position.y) *
                            deltaTime;
    particle->velocity.x *= 1.0f - Clamp(1.8f * deltaTime, 0.0f, 0.9f);
    *nextX = particle->position.x + particle->velocity.x * deltaTime;
    *nextY = particle->position.y + particle->velocity.y * deltaTime;
}

/* Presentation particles. The `const World *` is the point: a visual effect
   reads terrain to bounce off it and is structurally unable to change a cell,
   so decoration can never quietly become gameplay. */
static void ParticleStepVisual(Particle *particle, const World *world,
                               float deltaTime)
{
    float nextX;
    float nextY;
    bool embedded;

    ParticleIntegrate(particle, world, deltaTime, &nextX, &nextY);

    if (world == NULL || particle->contact == PARTICLE_CONTACT_PASS) {
        particle->position.x = nextX;
        particle->position.y = nextY;
        return;
    }

    /* A particle spawned inside material must be allowed to escape before
       terrain can stop it. */
    embedded = ParticleCellBlocks(world, particle->position.x,
                                  particle->position.y);

    /* Axis-separated like the player collider, so a shard that meets a wall
       keeps sliding along it instead of stopping dead. */
    if (!embedded && ParticleCellBlocks(world, nextX, particle->position.y)) {
        particle->velocity.x = -particle->velocity.x * particle->restitution;
    } else {
        particle->position.x = nextX;
    }

    if (!embedded && ParticleCellBlocks(world, particle->position.x, nextY)) {
        particle->velocity.y = -particle->velocity.y * particle->restitution;
        particle->velocity.x *= 0.72f;
    } else {
        particle->position.y = nextY;
    }
}

/* Gameplay debris: the one role allowed to write cells, and only by coming to
   rest in an empty one. */
static void ParticleStepDebris(Particle *particle, World *world, float deltaTime)
{
    float nextX;
    float nextY;
    bool embedded;

    ParticleIntegrate(particle, world, deltaTime, &nextX, &nextY);

    if (world == NULL) {
        particle->position.x = nextX;
        particle->position.y = nextY;
        return;
    }

    /* Drill debris is born at the cut face, inside material. */
    embedded = ParticleCellBlocks(world, particle->position.x,
                                  particle->position.y);

    if (!embedded && ParticleCellBlocks(world, nextX, particle->position.y)) {
        ParticleSettle(particle, world);
        return;
    }
    particle->position.x = nextX;

    if (!embedded && ParticleCellBlocks(world, particle->position.x, nextY)) {
        ParticleSettle(particle, world);
        return;
    }
    particle->position.y = nextY;
}

void ParticlesUpdate(ParticleSystem *system, World *world, float deltaTime)
{
    int i;

    if (system == NULL) {
        return;
    }

    for (i = 0; i < MAX_PARTICLES; ++i) {
        Particle *particle = &system->particles[i];

        if (!particle->active) {
            continue;
        }

        particle->life -= deltaTime;
        if (particle->life <= 0.0f) {
            particle->active = false;
            continue;
        }

        /* One pool, two roles. Keeping them in the same fixed-capacity array
           preserves the property that matters — no allocation, ever — while
           the split below is what keeps the roles honest. */
        if (particle->contact == PARTICLE_CONTACT_SETTLE) {
            ParticleStepDebris(particle, world, deltaTime);
        } else {
            ParticleStepVisual(particle, world, deltaTime);
        }
    }
}

void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position)
{
    int i;

    if (system == NULL) {
        return;
    }

    for (i = 0; i < 120; ++i) {
        float angle = RandomUnit(system) * 2.0f * PI;
        float speed = 18.0f + RandomUnit(system) * 105.0f;
        float life = 0.35f + RandomUnit(system) * 0.65f;
        Color color;

        if (i % 4 == 0) {
            color = (Color){112, 107, 105, 255};
        } else if (i % 3 == 0) {
            color = (Color){255, 211, 72, 255};
        } else {
            color = (Color){245, 83, 30, 255};
        }
        Particle *particle =
            ParticlesSpawnOne(system, position,
                              (Vector2){cosf(angle) * speed, sinf(angle) * speed},
                              color, life, 0.7f + RandomUnit(system) * 1.5f, 30.0f);

        particle->contact = PARTICLE_CONTACT_BOUNCE;
        particle->emission = i % 4 == 0 ? 0.0f : 0.9f;
    }
}

void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position, Vector2 direction)
{
    int i;

    if (system == NULL) {
        return;
    }

    for (i = 0; i < 3; ++i) {
        float angle = atan2f(direction.y, direction.x) + PI +
                      (RandomUnit(system) - 0.5f) * 1.6f;
        float speed = 15.0f + RandomUnit(system) * 35.0f;

        /* Sparks come off the cell the laser is eating, so they ricochet
           along the face instead of sinking into it. */
        Particle *particle =
            ParticlesSpawnOne(system, position,
                              (Vector2){cosf(angle) * speed, sinf(angle) * speed},
                              (Color){255, 225, 90, 255},
                              0.12f + RandomUnit(system) * 0.18f,
                              0.45f + RandomUnit(system) * 0.65f, 18.0f);

        particle->contact = PARTICLE_CONTACT_BOUNCE;
        particle->emission = 1.0f;
    }
}

void ParticlesSpawnImpact(ParticleSystem *system, Vector2 position, Vector2 normal,
                          float strength)
{
    Vector2 tangent = {-normal.y, normal.x};
    int count;
    int i;

    if (system == NULL || strength < 14.0f) {
        return;
    }

    count = 5 + (int)Clamp(strength / 18.0f, 0.0f, 7.0f);
    for (i = 0; i < count; ++i) {
        float outward = 9.0f + RandomUnit(system) * fminf(strength * 0.42f, 42.0f);
        float sideways = (RandomUnit(system) - 0.5f) * 34.0f;
        Color color = i % 3 == 0 ? (Color){255, 190, 77, 255}
                                 : (Color){132, 126, 119, 230};

        Particle *particle =
            ParticlesSpawnOne(system, position,
                              (Vector2){normal.x * outward + tangent.x * sideways,
                                        normal.y * outward + tangent.y * sideways},
                              color, 0.18f + RandomUnit(system) * 0.25f,
                              0.45f + RandomUnit(system) * 0.7f, 22.0f);

        particle->contact = PARTICLE_CONTACT_BOUNCE;
        particle->emission = i % 3 == 0 ? 0.72f : 0.0f;
    }
}

void ParticlesSpawnBoostTrail(ParticleSystem *system, Vector2 position,
                              Vector2 velocity)
{
    float speed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
    Vector2 direction;
    Vector2 tangent;
    int i;

    if (system == NULL || speed < 1.0f) {
        return;
    }

    direction = (Vector2){velocity.x / speed, velocity.y / speed};
    tangent = (Vector2){-direction.y, direction.x};
    position.x -= direction.x * 5.0f;
    position.y -= direction.y * 5.0f;
    for (i = 0; i < 5; ++i) {
        float spread = (RandomUnit(system) - 0.5f) * 8.0f;
        float backward = 14.0f + RandomUnit(system) * 56.0f;
        Color color = i % 2 == 0 ? (Color){170, 226, 255, 225}
                                 : (Color){104, 238, 244, 190};

        Particle *particle =
            ParticlesSpawnOne(
                system,
                (Vector2){position.x + tangent.x * spread,
                          position.y + tangent.y * spread},
                (Vector2){-direction.x * backward + tangent.x * spread,
                          -direction.y * backward + tangent.y * spread},
                color, 0.12f + RandomUnit(system) * 0.20f,
                0.35f + RandomUnit(system) * 0.89f, 0.0f);

        particle->emission = 0.88f;
    }
}

void ParticlesSpawnBoostBurst(ParticleSystem *system, Vector2 position,
                              Vector2 velocity)
{
    float speed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
    Vector2 direction;
    const int count = 38;
    int i;

    if (system == NULL || speed < 1.0f) {
        return;
    }

    direction = (Vector2){velocity.x / speed, velocity.y / speed};
    for (i = 0; i < count; ++i) {
        float angle = (float)i / (float)count * 2.0f * PI;
        Vector2 radial = {cosf(angle), sinf(angle)};
        float outward = 72.0f + RandomUnit(system) * 45.0f;
        float wake = 74.0f;
        Color color = i % 2 == 0 ? (Color){134, 221, 255, 225}
                                 : (Color){111, 239, 235, 210};

        Particle *particle =
            ParticlesSpawnOne(
                system, position,
                (Vector2){radial.x * outward - direction.x * wake,
                          radial.y * outward - direction.y * wake},
                color, 0.36f + RandomUnit(system) * 0.18f,
                0.7f + RandomUnit(system) * 1.4f, 0.0f);

        particle->emission = 0.94f;
    }
}

void ParticlesSpawnDrillDebris(ParticleSystem *system, Vector2 position,
                               Vector2 velocity, int destroyedCells)
{
    float speed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);
    Vector2 direction;
    Vector2 tangent;
    int count;
    int i;

    if (system == NULL || destroyedCells <= 0 || speed < 1.0f) {
        return;
    }

    direction = (Vector2){velocity.x / speed, velocity.y / speed};
    tangent = (Vector2){-direction.y, direction.x};
    count = 5 + destroyedCells / 3;
    if (count > 18) count = 18;
    for (i = 0; i < count; ++i) {
        float backward = 18.0f + RandomUnit(system) * 58.0f;
        float sideways = (RandomUnit(system) - 0.5f) * 72.0f;
        Color color;

        if (i % 4 == 0) {
            color = (Color){239, 111, 39, 245};
        } else if (i % 3 == 0) {
            color = (Color){116, 92, 67, 235};
        } else {
            color = (Color){111, 116, 124, 235};
        }
        Particle *particle =
            ParticlesSpawnOne(system, position,
                              (Vector2){-direction.x * backward + tangent.x * sideways,
                                        -direction.y * backward + tangent.y * sideways},
                              color, 0.2f + RandomUnit(system) * 0.34f,
                              0.45f + RandomUnit(system) * 0.9f, 24.0f);

        particle->emission = i % 4 == 0 ? 0.65f : 0.0f;

        /* Roughly a third of the spoil comes to rest as real ash, so a bored
           tunnel collects grit on its floor instead of staying surgically
           clean. The rest ricochets and expires: settling all of it would refill
           the tunnel faster than it reads as debris. */
        if (i % 3 == 0) {
            particle->contact = PARTICLE_CONTACT_SETTLE;
            particle->settleMaterial = MATERIAL_ASH;
        } else {
            particle->contact = PARTICLE_CONTACT_BOUNCE;
        }
    }
}

void ParticlesSpawnForceBlast(ParticleSystem *system, Vector2 origin,
                              Vector2 direction)
{
    Vector2 tangent = {-direction.y, direction.x};
    int i;

    if (system == NULL) {
        return;
    }

    /* A pale wedge of air thrown down the cone. It passes through terrain: this
       is the shape of the blow, not debris, and stopping it at the first wall
       would hide the very impact it is announcing. */
    for (i = 0; i < 52; ++i) {
        float forward = 62.0f + RandomUnit(system) * 250.0f;
        float sideways = (RandomUnit(system) - 0.5f) * 132.0f;
        float spawn = RandomUnit(system) * 8.0f;
        Color color = i % 3 == 0 ? (Color){226, 240, 255, 220}
                                 : (Color){158, 196, 236, 190};

        ParticlesSpawnOne(system,
                          (Vector2){origin.x + direction.x * spawn,
                                    origin.y + direction.y * spawn},
                          (Vector2){direction.x * forward + tangent.x * sideways,
                                    direction.y * forward + tangent.y * sideways},
                          color, 0.18f + RandomUnit(system) * 0.3f,
                          0.7f + RandomUnit(system) * 1.8f, 6.0f);
    }
}

void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position)
{
    int i;

    if (system == NULL) {
        return;
    }

    for (i = 0; i < 7; ++i) {
        float horizontal = (RandomUnit(system) - 0.5f) * 18.0f;
        float upward = -10.0f - RandomUnit(system) * 24.0f;
        Color color = i % 3 == 0 ? (Color){188, 218, 228, 215}
                                 : (Color){224, 232, 226, 190};

        ParticlesSpawnOne(system, position,
                          (Vector2){horizontal, upward}, color,
                          0.45f + RandomUnit(system) * 0.65f,
                          0.65f + RandomUnit(system) * 1.25f, -10.0f);
    }
}
