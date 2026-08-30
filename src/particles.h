#ifndef PARTICLES_H
#define PARTICLES_H

#include <stdbool.h>

#include <raylib.h>

#define MAX_PARTICLES 1024

typedef struct Particle {
    Vector2 position;
    Vector2 velocity;
    Color color;
    float life;
    float maxLife;
    float size;
    float gravity;
    bool active;
} Particle;

typedef struct ParticleSystem {
    Particle particles[MAX_PARTICLES];
    int nextParticle;
} ParticleSystem;

void ParticlesInit(ParticleSystem *system);
void ParticlesUpdate(ParticleSystem *system, float deltaTime);
void ParticlesDraw(const ParticleSystem *system);
void ParticlesSpawnExplosion(ParticleSystem *system, Vector2 position);
void ParticlesSpawnLaserSparks(ParticleSystem *system, Vector2 position, Vector2 direction);
void ParticlesSpawnImpact(ParticleSystem *system, Vector2 position, Vector2 normal,
                          float strength);
void ParticlesSpawnSteam(ParticleSystem *system, Vector2 position);

#endif
