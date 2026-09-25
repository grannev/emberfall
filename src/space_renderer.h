#ifndef SPACE_RENDERER_H
#define SPACE_RENDERER_H

/* What lies beyond the sky: the backdrop of open space.
 *
 * Stars used to be drawn in the world's own coordinates, scrolling with the
 * ground one for one, and so they read as specks in front of the player
 * rather than as the depth of space. This is the other way round: a picture
 * as far away as a picture can be, drawn in screen space behind everything,
 * moving against the camera by a few thousandths — a nebula, two layers of
 * stars of many colours, a ringed giant hanging low, asteroids drifting
 * across. Its textures are made once from the world's seed and kept; a frame
 * costs a handful of textured quads and a few dozen blocks.
 *
 * It is drawn twice over a frame: faintly behind the landscape at night, so
 * the night sky has stars in it as far away as they belong, and fully over
 * the landscape as the camera climbs out of the air, where it is the only
 * backdrop there is. Presentation only; it never sees GameState or World.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#define SPACE_ASTEROID_COUNT 16

typedef struct SpaceAsteroid {
    /* Where it sits in its layer, 0..1 across the wrap, 0..1 down the view. */
    float x;
    float y;
    /* Radius in blocks, and how fast its layer moves against the camera. */
    float radius;
    float parallax;
    /* Its own slow drift across, in blocks per second. */
    float drift;
    int salt;
} SpaceAsteroid;

typedef struct SpaceRenderer {
    uint64_t seed;
    Texture2D nebula;
    Texture2D starsFar;
    Texture2D starsNear;
    Texture2D planet;
    SpaceAsteroid asteroids[SPACE_ASTEROID_COUNT];
    bool ready;
} SpaceRenderer;

/* Builds the textures for `seed`. Needs a GL context; false when a texture
   could not be made, in which case drawing does nothing. */
bool SpaceRendererInit(SpaceRenderer *space, uint64_t seed);
/* Rebuilds for a new world's seed; nothing when it is the same seed. */
void SpaceRendererSyncSeed(SpaceRenderer *space, uint64_t seed);
/* Draws the backdrop at `amount` (0..1) over the whole target, screen
   space. `travel` is the camera's travel round the planet, so the parallax
   does not jump at the seam. `full` adds the planet and the asteroids, which
   belong to open space and not to a night sky seen from the ground. The
   emissive variant draws only what glows: the brighter stars. */
void SpaceRendererDraw(const SpaceRenderer *space, Camera2D camera, float travel,
                       int width, int height, float amount, float time, bool full);
void SpaceRendererDrawEmissive(const SpaceRenderer *space, Camera2D camera,
                               float travel, int width, int height, float amount,
                               float time);
void SpaceRendererUnload(SpaceRenderer *space);

#endif
