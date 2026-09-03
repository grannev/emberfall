#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdbool.h>

#include <raylib.h>

#include "rng.h"
#include "world.h"

#define MAX_PARTICLES 1024

/* What a particle does when it meets a solid cell — and, with it, whether the
   particle is presentation or gameplay. PASS and BOUNCE are visual: they read
   terrain through a `const World *` and cannot change it. SETTLE is debris: it
   comes to rest as a real cell, which makes it part of the simulation, so its
   randomness is seeded and its behaviour is covered by headless tests.

   Effects used to ignore the world entirely and fly through rock, which read
   as decoration painted over the simulation rather than as part of it. */
typedef enum ParticleContact {
    PARTICLE_CONTACT_PASS = 0, /* glow and gases: terrain does not stop them */
    PARTICLE_CONTACT_BOUNCE,   /* sparks and shards ricochet and lose energy */
    PARTICLE_CONTACT_SETTLE    /* debris comes to rest as a real world cell */
} ParticleContact;

typedef struct Particle {
    Vector2 position;
    Vector2 velocity;
    Color color;
    float life;
    float maxLife;
    float size;
    float gravity;
    float restitution;
    /* Presentation-only bloom contribution, 0..1. Simulation never branches
       on it; explicit metadata keeps pale dust and steam out of emissive. */
    float emission;
    ParticleContact contact;
    CellMaterial settleMaterial;
    bool active;
} Particle;

typedef struct ParticleSystem {
    Particle particles[MAX_PARTICLES];
    int nextParticle;
    Rng rng;
} ParticleSystem;

/* `seed` makes debris reproducible: settling particles write real cells, so
   their randomness is gameplay state, not decoration. */
void ParticlesInit(ParticleSystem *system, uint64_t seed);
void ParticlesUpdate(ParticleSystem *system, World *world, float deltaTime);
void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position);
void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position, Vector2 direction);
void ParticlesSpawnImpact(ParticleSystem *system, Vector2 position, Vector2 normal,
                          float strength);
void ParticlesSpawnBoostTrail(ParticleSystem *system, Vector2 position,
                              Vector2 velocity);
void ParticlesSpawnBoostBurst(ParticleSystem *system, Vector2 position,
                              Vector2 velocity);
void ParticlesSpawnDrillDebris(ParticleSystem *system, Vector2 position,
                               Vector2 velocity, int destroyedCells);
void ParticlesSpawnForceBlast(ParticleSystem *system, Vector2 origin,
                              Vector2 direction);
void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position);

#endif
