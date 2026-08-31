#include "particle_renderer.h"

#include <math.h>
#include <stddef.h>

static void ParticleBounds(const Particle *particle, int *x, int *y, int *extent)
{
    *extent = (int)(particle->size + 0.5f);
    if (*extent < 1) {
        *extent = 1;
    }
    *x = (int)floorf(particle->position.x) - (*extent - 1) / 2;
    *y = (int)floorf(particle->position.y) - (*extent - 1) / 2;
}

void ParticleRendererDraw(const ParticleSystem *system)
{
    int i;

    if (system == NULL) {
        return;
    }

    for (i = 0; i < MAX_PARTICLES; ++i) {
        const Particle *particle = &system->particles[i];

        if (particle->active) {
            /* Whole cells keep particles on the same pixel grid as the world
               texture and the player model; circles would blur across it. */
            Color color = Fade(particle->color, particle->life / particle->maxLife);
            int extent;
            int x;
            int y;

            ParticleBounds(particle, &x, &y, &extent);
            DrawRectangle(x, y, extent, extent, color);
        }
    }
}

void ParticleRendererDrawEmissive(const ParticleSystem *system)
{
    int i;

    if (system == NULL) {
        return;
    }
    for (i = 0; i < MAX_PARTICLES; ++i) {
        const Particle *particle = &system->particles[i];

        if (particle->active && particle->emission > 0.0f) {
            float strength = particle->emission * particle->life /
                             particle->maxLife;
            Color color = {
                (unsigned char)((float)particle->color.r * strength),
                (unsigned char)((float)particle->color.g * strength),
                (unsigned char)((float)particle->color.b * strength),
                255u
            };
            int extent;
            int x;
            int y;

            ParticleBounds(particle, &x, &y, &extent);
            DrawRectangle(x, y, extent, extent, color);
        }
    }
}
