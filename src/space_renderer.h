#ifndef SPACE_RENDERER_H
#define SPACE_RENDERER_H

/* What lies beyond the sky: the backdrop of open space.
 *
 * A picture as far away as a picture can be, drawn in screen space behind
 * everything and moving against the camera by a few thousandths: a muted
 * nebula in stepped washes and two layers of stars of many colours. Its
 * textures are made once from the world's seed and kept; a frame costs a
 * handful of textured quads.
 *
 * It is not faded in over the landscape. The environment darkens its sky
 * from the top down as the camera climbs, and hands this a mask — full above
 * one row of the screen, nothing below another — so the stars come out
 * where the sky has gone dark and nowhere else: first overhead, then lower,
 * until only the glow over the curve of the planet is left. At night the
 * same stars stand in the whole sky, faintly. Presentation only; it never
 * sees GameState or World.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

typedef struct SpaceRenderer {
    uint64_t seed;
    Texture2D nebula;
    Texture2D starsFar;
    Texture2D starsNear;
    bool ready;
} SpaceRenderer;

/* Builds the textures for `seed`. Needs a GL context; false when a texture
   could not be made, in which case drawing does nothing. */
bool SpaceRendererInit(SpaceRenderer *space, uint64_t seed);
/* Rebuilds for a new world's seed; nothing when it is the same seed. */
void SpaceRendererSyncSeed(SpaceRenderer *space, uint64_t seed);
/* Draws the backdrop at `amount` (0..1) over the whole target, screen
   space, masked to full above screen row `fullY` and to nothing below
   `clearY`. `travel` is the camera's travel round the planet, so the
   parallax does not jump at the seam. The emissive variant draws only what
   glows: the brighter stars. */
void SpaceRendererDraw(const SpaceRenderer *space, Camera2D camera, float travel,
                       int width, int height, float amount, float fullY, float clearY);
void SpaceRendererDrawEmissive(const SpaceRenderer *space, Camera2D camera,
                               float travel, int width, int height, float amount,
                               float fullY, float clearY);
void SpaceRendererUnload(SpaceRenderer *space);

#endif
