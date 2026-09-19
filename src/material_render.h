#ifndef MATERIAL_RENDER_H
#define MATERIAL_RENDER_H

/* Shared CPU conversion from material state to presentation pixels.
 *
 * Static world pages and detached terrain bodies must use one palette path:
 * otherwise a fragment changes colour at the instant it is extracted.
 *
 * The pixels are unlit. Light is applied on the GPU by the world light shader
 * (see light_renderer.h) from the coarse light field, which is what lets the
 * player's lamp move and the day turn without a single chunk being rebuilt:
 * a page only changes when a cell in it does. Baking the light into the
 * pixels was the single largest cost of flying — every frame the lamp moved,
 * every chunk it reached was rebuilt from scratch, a thousand cells at a time.
 *
 * The two planes carry their occlusion with them. In the scene plane a
 * material is opaque or translucent as its table colour says; in the emissive
 * plane a material that does not glow is opaque black, so that a star, a
 * distant tower light or a lava pool drawn behind it cannot bloom through
 * it. Air is marked rather than coloured: MATERIAL_RENDER_AIR_ALPHA in both
 * planes, and the shader gives it the alpha the sky light says it should have.
 * Drawn without the shader, air is all but transparent.
 */

#include <raylib.h>

#include "world.h"

/* Alpha of an air texel in the scene plane. Not zero: a terrain body's raster
   has empty cells too, and those must stay transparent under the same shader,
   so air is told apart from "nothing here" by being almost — not quite —
   invisible. Every material's own alpha is far above it. */
#define MATERIAL_RENDER_AIR_ALPHA 1u

typedef struct MaterialRenderSample {
    Color scene;
    Color emissive;
} MaterialRenderSample;

/* The unlit pixel for one cell. `variationX`/`variationY` seed the dither, so
   a cell keeps its grain whether it is drawn in the world or in a body. */
MaterialRenderSample MaterialRenderCell(CellMaterial material,
                                        float temperature,
                                        int variationX, int variationY);

/* The unlit pixel for air at world row `y` of a world `height` tall: a depth
   gradient the shader tints and veils. */
MaterialRenderSample MaterialRenderAir(int y, int height);

#endif
