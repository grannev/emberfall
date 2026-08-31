#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdbool.h>

#include <raylib.h>

#include "world.h"

#define MAX_PARTICLES 1024

/* What a particle does when it meets a solid cell. Effects used to ignore the
   world entirely and fly through rock, which read as decoration painted over
   the simulation rather than as part of it. */
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
    ParticleContact contact;
    CellMaterial settleMaterial;
    bool active;
} Particle;

typedef struct ParticleSystem {
    Particle particles[MAX_PARTICLES];
    int nextParticle;
} ParticleSystem;

void ParticlesInit(ParticleSystem *system);
void ParticlesUpdate(ParticleSystem *system, World *world, float deltaTime);
void ParticlesDraw(const ParticleSystem *system);
void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position);
void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position, Vector2 direction);
void ParticlesSpawnImpact(ParticleSystem *system, Vector2 position, Vector2 normal,
                          float strength);
void ParticlesSpawnBoostTrail(ParticleSystem *system, Vector2 position,
                              Vector2 velocity);
void ParticlesSpawnDrillDebris(ParticleSystem *system, Vector2 position,
                               Vector2 velocity, int destroyedCells);
void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position);

#endif
