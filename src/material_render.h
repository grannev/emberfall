#ifndef MATERIAL_RENDER_H
#define MATERIAL_RENDER_H

/* Shared CPU conversion from material state to presentation pixels.
 *
 * Static world pages and detached terrain bodies must use one palette path:
 * otherwise a fragment changes colour at the instant it is extracted. The
 * caller supplies its lighting multiplier because the static world has a
 * solved light field while a moving body currently uses a bounded ambient
 * approximation.
 */

#include <raylib.h>

#include "world.h"

typedef struct MaterialRenderSample {
    Color scene;
    Color emissive;
} MaterialRenderSample;

MaterialRenderSample MaterialRenderCell(CellMaterial material,
                                        float temperature,
                                        int variationX, int variationY,
                                        float red, float green, float blue);

#endif
