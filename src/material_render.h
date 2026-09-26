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

#include <stdbool.h>

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

/* What a pixel needs to know about the cell beyond its material: its own
   tone, and what is around it. Filled by whoever is walking the cells — the
   world's page builder or a body's raster — from the neighbours it already
   has in hand. */
typedef struct MaterialRenderContext {
    /* Cell.shade, 0..63: the cell's own tone, which moves with it. */
    unsigned char shade;
    /* Nothing of the same kind of stuff above or below: a top face catches
       the light, an underside falls into shadow. A liquid's surface is its
       top face. */
    bool openAbove;
    bool openBelow;
    /* Cells of the same liquid over this one, 0 at the surface, capped by
       the caller: how far down the water has darkened. */
    int liquidDepth;
} MaterialRenderContext;

/* Whether a cell of `material` with `neighbour` beside it has an open face
   there: a solid against anything not solid, a liquid against anything not
   liquid, anything else against anything other than itself. */
bool MaterialRenderOpenFace(CellMaterial material, CellMaterial neighbour);

/* How deep a liquid's darkening goes, in cells of the same liquid over it. */
#define MATERIAL_RENDER_DEPTH_CAP 10

/* The unlit pixel for one cell. `patternX`/`patternY` place the material's
   pattern — the strata of rock, the grain of wood — and are the cell's
   original world coordinates, so a slab carried away keeps its bands. */
MaterialRenderSample MaterialRenderCell(CellMaterial material,
                                        float temperature,
                                        int patternX, int patternY,
                                        MaterialRenderContext context);

/* The unlit pixel of the back layer: `wall`'s own pattern at (x, y), set
   back — darker and greyer than any cell in front of it, and opaque, so the
   backdrop never shows through the ground. Whatever is empty in front of a
   wall shows this instead of air. */
MaterialRenderSample MaterialRenderBackWall(CellMaterial wall, int x, int y);
/* The plants' layer keeps its alpha for how freely a pixel sways: from this
   value (a pixel that stands still) to 255 (one that moves the most). Below
   it there is no plant. */
#define MATERIAL_RENDER_FLORA_ALPHA 128u
/* How freely a plant's pixel sways in the wind and under a touch, 0..1: a
   blade's tip more than its foot, leaves more than wood, a cactus not at
   all. */
float MaterialRenderSway(CellMaterial material, unsigned char shade);
/* A cell that lets what is behind it through — a liquid, a gas — laid over
   the back wall behind it, opaque. */
MaterialRenderSample MaterialRenderOverWall(MaterialRenderSample front,
                                            MaterialRenderSample wall);

/* The unlit pixel for air at world row `y` of a world `height` tall: a depth
   gradient the shader tints and veils. */
MaterialRenderSample MaterialRenderAir(int y, int height);

#endif
