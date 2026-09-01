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
 * Nothing is stored. A cloud is a slot index hashed into a shape, a height and
 * a drift, so the sky is as wide as the world for no memory at all and the same
 * seed always makes the same sky. `time` moves the drift; passing the same
 * value twice draws the same sky, which is what lets the smoke run photograph
 * one.
 *
 * Presentation only: it reads the world's height and the camera and writes to
 * neither.
 */

#include <stdint.h>

#include <raylib.h>

/* Cells between one cloud slot and the next. Every slot holds exactly one
   cloud, and its size and offset within the slot come from the seed, so the
   spacing is a rhythm rather than a grid. */
#define SKY_CLOUD_SPACING 150

typedef struct SkyRendererStats {
    uint16_t cloudsDrawn;
    uint16_t starsDrawn;
    /* Whether the camera is looking at the weightless band at all. */
    bool spaceVisible;
} SkyRendererStats;

typedef struct SkyRenderer {
    uint64_t seed;
    SkyRendererStats stats;
} SkyRenderer;

void SkyRendererInit(SkyRenderer *sky, uint64_t seed);
void SkyRendererSyncSeed(SkyRenderer *sky, uint64_t seed);

/* Draws the space veil, its stars and the clouds, in that order, for the region
   `visible` of a world `worldHeight` cells tall. Must be called inside the
   camera transform. `daylight` fades the clouds toward night and lets the stars
   through; `time` drifts them. */
void SkyRendererDraw(SkyRenderer *sky, Rectangle visible, int worldHeight,
                     float daylight, float time);
/* The same sky for the emissive pass: only what should glow — the stars, and
   the lit upper edge of a cloud. */
void SkyRendererDrawEmissive(SkyRenderer *sky, Rectangle visible,
                             int worldHeight, float daylight, float time);

const SkyRendererStats *SkyRendererStatistics(const SkyRenderer *sky);

#endif
