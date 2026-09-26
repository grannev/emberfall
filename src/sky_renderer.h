#ifndef SKY_RENDERER_H
#define SKY_RENDERER_H

/* Clouds and the space above them, drawn in world coordinates.
 *
 * This is deliberately not part of environment_renderer.c. That module paints a
 * backdrop in screen space with a parallax factor, which is right for a horizon
 * — the horizon is infinitely far away and the player can never get above it.
 * Clouds are the opposite: the whole point of them is that they sit at an
 * altitude the player can climb past, so they have to be at a place in the
 * world rather than at a place on the screen. Drawn inside the camera's
 * transform, between the backdrop and the terrain.
 *
 * They are still further away than the ground. Each layer scrolls at a fraction
 * of the camera's horizontal motion, so a cloud slides past more slowly than
 * the hill under it and reads as distance; its altitude is not shifted, because
 * altitude is the one thing about a cloud the gameplay agrees on.
 *
 * Nothing about a cloud's shape is stored in the simulation. A cloud is a slot
 * index hashed into a shape, a height and an offset, so the sky is as wide as
 * the world for no memory at all and the same seed always makes the same sky.
 * `time` moves the drift; passing the same value twice draws the same sky,
 * which is what lets the smoke run photograph one. Every cloud in a layer
 * drifts at the layer's one speed, which is what makes "which slots can be on
 * screen" an exact question with an exact answer: a cloud used to drift out of
 * the range of slots the cull looked at and vanish in front of the player.
 *
 * A cloud is soft and it is made of squares. Its density is a handful of
 * overlapping puffs, quantised to a few levels of translucency and dithered
 * along each level's edge, and it is drawn in blocks of SKY_CLOUD_BLOCK cells
 * — coarser than the ground, the way the backdrop's ridges are, because it is
 * further away. Each shape is rasterised once into a small texture and drawn
 * with point filtering, so a cloud costs one quad however large it is and the
 * blocks stay crisp at any zoom. The textures are the only GPU state here;
 * everything that decides where a cloud is and what it looks like is plain
 * arithmetic a headless test can run.
 *
 * Presentation only: it reads the world's height and the camera and writes to
 * neither.
 */

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

/* Two layers, far and near. The far one is smaller, fainter, higher and slower
   against the camera; the near one is what the player flies through. */
#define SKY_CLOUD_LAYERS 3
/* Overlapping puffs per cloud, so a cloud has a silhouette rather than an
   outline. */
#define SKY_CLOUD_PUFFS 7
/* World cells per block of cloud. */
#define SKY_CLOUD_BLOCK 2
/* Blocks in a cloud's texture. Every cloud of every layer fits, with its
   outermost blocks always empty so the texture's edge never shows. */
#define SKY_CLOUD_TEXTURE_WIDTH 64
#define SKY_CLOUD_TEXTURE_HEIGHT 40
/* Levels of translucency a cloud is quantised to, dithered between. */
#define SKY_CLOUD_LEVELS 4
/* Rasterised clouds kept on the GPU. More than fit on the widest view, so
   panning re-rasterises only what newly arrives. */
#define SKY_CLOUD_CACHE 40

typedef struct SkyCloudLayer {
    /* Cells between one cloud slot and the next. Every slot holds exactly one
       cloud, and its size and offset within the slot come from the seed, so the
       spacing is a rhythm rather than a grid. */
    float spacing;
    /* Cells per second the whole layer drifts. */
    float drift;
    /* How much of the camera's horizontal motion the layer follows: one is the
       ground, zero is the horizon. */
    float parallax;
    /* Radius of a cloud's body, and of the largest puff in it. */
    float radius;
    /* Where the layer sits, in units of the band between the space line and
       the cloud line, measured down from the space line. May exceed one: the
       cloud line is where weight fades, not where weather stops. */
    float bandLow;
    float bandHigh;
    /* Opacity of the densest block at noon. */
    float alpha;
} SkyCloudLayer;

typedef struct SkyRendererStats {
    uint16_t cloudsDrawn;
    /* Clouds rasterised and uploaded this frame. */
    uint16_t cloudsBuilt;
} SkyRendererStats;

typedef struct SkyCloudTexture {
    Texture2D texture;
    int layer;
    int slot;
    uint32_t lastUsedFrame;
    bool bound;
} SkyCloudTexture;

typedef struct SkyRenderer {
    uint64_t seed;
    SkyRendererStats stats;
    /* GPU state, present only after SkyRendererLoad. Without it the sky still
       has its space and its stars; it simply has no clouds. */
    SkyCloudTexture clouds[SKY_CLOUD_CACHE];
    int cloudCapacity;
    uint32_t frame;
    /* The weather over the view: how much of the sky is cloud (0..1) and how
       dark and heavy it is (0..1, a storm). */
    float cover;
    float storm;
} SkyRenderer;

void SkyRendererInit(SkyRenderer *sky, uint64_t seed);
void SkyRendererSyncSeed(SkyRenderer *sky, uint64_t seed);
/* Creates the cloud texture cache. Needs a GL context; a sky that is never
   loaded draws no clouds. */
bool SkyRendererLoad(SkyRenderer *sky);
void SkyRendererUnload(SkyRenderer *sky);

/* Draws the space veil, its stars and the clouds, in that order, for the region
   `visible` of a world `worldHeight` cells tall. Must be called inside the
   camera transform. `daylight` fades the clouds toward night and lets the stars
   through; `time` drifts them. */
void SkyRendererDraw(SkyRenderer *sky, Rectangle visible, int worldHeight,
                     float daylight, float time);
/* The same sky for the emissive pass: the stars glow, and the clouds are drawn
   over them in black at their own opacity, so a star behind a cloud blooms
   through it only as much as the cloud lets through. */
void SkyRendererDrawEmissive(SkyRenderer *sky, Rectangle visible,
                             int worldHeight, float daylight, float time);

const SkyCloudLayer *SkyRendererLayer(int layer);
/* The weather the clouds show: `cover` of the sky under cloud, `storm` how
   dark it is. The clouds' drift is the `time` the draw calls are given:
   the renderer passes the distance the wind has carried them. */
void SkyRendererSetWeather(SkyRenderer *sky, float cover, float storm);

/* Where a cloud's texture is drawn, in world cells, for the view `visible`:
   the block of SKY_CLOUD_TEXTURE_WIDTH by SKY_CLOUD_TEXTURE_HEIGHT blocks
   centred on the cloud, after the layer's parallax. Everything the cloud
   covers lies inside it. */
Rectangle SkyRendererCloudBounds(const SkyRenderer *sky, int layer, int slot,
                                 int worldHeight, float time, Rectangle visible);
/* One puff of one cloud, as centre and radius, in world cells for the view
   `visible`. Exposed alongside the bounds so a test can check the two against
   each other rather than against a constant copied out of the drawing code. */
void SkyRendererCloudPuff(const SkyRenderer *sky, int layer, int slot,
                          int worldHeight, float time, Rectangle visible,
                          int puff, Vector2 *centre, float *radius);
/* The inclusive range of slots of `layer` whose clouds can reach `visible`.
   Exact rather than generous: every cloud whose bounds meet the view is in it,
   and that is what the cull relies on. */
void SkyRendererVisibleSlots(const SkyRenderer *sky, int layer, Rectangle visible,
                             float time, int *firstSlot, int *lastSlot);
/* Rasterises one cloud into `texels`, SKY_CLOUD_TEXTURE_WIDTH by
   SKY_CLOUD_TEXTURE_HEIGHT of them, row-major. Colour is the cloud at noon;
   alpha is the quantised, dithered density. Pure arithmetic, so a test can
   check the shape without a GPU. */
void SkyRendererBuildCloud(const SkyRenderer *sky, int layer, int slot,
                           Color *texels);

const SkyRendererStats *SkyRendererStatistics(const SkyRenderer *sky);

#endif
