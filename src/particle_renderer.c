#include "particle_renderer.h"

#include <math.h>
#include <stddef.h>

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
            int extent = (int)(particle->size + 0.5f);
            int x;
            int y;

            if (extent < 1) {
                extent = 1;
            }
            x = (int)floorf(particle->position.x) - (extent - 1) / 2;
            y = (int)floorf(particle->position.y) - (extent - 1) / 2;
            DrawRectangle(x, y, extent, extent, color);
        }
    }
}
