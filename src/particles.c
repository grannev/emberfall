#include "particles.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include <raymath.h>

static float RandomUnit(void)
{
    return (float)GetRandomValue(0, 10000) / 10000.0f;
}

static void ParticlesSpawnOne(ParticleSystem *system, Vector2 position,
                              Vector2 velocity, Color color, float life, float size)
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
    particle->active = true;
}

void ParticlesInit(ParticleSystem *system)
{
    if (system == NULL) {
        return;
    }
    memset(system, 0, sizeof(*system));
}

void ParticlesUpdate(ParticleSystem *system, float deltaTime)
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

        particle->velocity.y += 30.0f * deltaTime;
        particle->velocity.x *= 1.0f - Clamp(1.8f * deltaTime, 0.0f, 0.9f);
        particle->position.x += particle->velocity.x * deltaTime;
        particle->position.y += particle->velocity.y * deltaTime;
    }
}

void ParticlesDraw(const ParticleSystem *system)
{
    int i;

    if (system == NULL) {
        return;
    }

    for (i = 0; i < MAX_PARTICLES; ++i) {
        const Particle *particle = &system->particles[i];

        if (particle->active) {
            Color color = Fade(particle->color, particle->life / particle->maxLife);
            DrawCircleV(particle->position, particle->size, color);
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
        float angle = RandomUnit() * 2.0f * PI;
        float speed = 18.0f + RandomUnit() * 105.0f;
        float life = 0.35f + RandomUnit() * 0.65f;
        Color color;

        if (i % 4 == 0) {
            color = (Color){112, 107, 105, 255};
        } else if (i % 3 == 0) {
            color = (Color){255, 211, 72, 255};
        } else {
            color = (Color){245, 83, 30, 255};
        }
        ParticlesSpawnOne(system, position,
                          (Vector2){cosf(angle) * speed, sinf(angle) * speed},
                          color, life, 0.7f + RandomUnit() * 1.5f);
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
                      (RandomUnit() - 0.5f) * 1.6f;
        float speed = 15.0f + RandomUnit() * 35.0f;

        ParticlesSpawnOne(system, position,
                          (Vector2){cosf(angle) * speed, sinf(angle) * speed},
                          (Color){255, 225, 90, 255}, 0.12f + RandomUnit() * 0.18f,
                          0.45f + RandomUnit() * 0.65f);
    }
}
