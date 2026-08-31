#ifndef WORLD_LIGHTING_H
#define WORLD_LIGHTING_H

/* Coarse two-channel light field.
 *
 * Light is solved on a grid WORLD_LIGHT_SCALE times coarser than the cells.
 * Two derived fields feed it: `lightEmission`, how much a block of cells gives
 * off, and `lightOpacity`, how much of it is solid. Both are refreshed only for
 * dirty chunks, because terrain only changes where the simulation is awake.
 *
 * The solve itself is a seed followed by two raster sweeps. Sky light is filled
 * per column from the top and needs no iteration, which is what keeps the open
 * surface uniformly bright no matter how tall the world is; the sweeps then
 * carry that light, plus every emitter, sideways and into overhangs, attenuated
 * by whatever it passes through. Two sweeps are not an exact flood fill around
 * a hairpin corridor, but they are stable, allocation-free, and close enough
 * that the error is invisible at four cells per sample.
 */

#include <math.h>

#include "world.h"

/* Ambient floor: even sealed rock stays legible, and a light of zero would make
   an unlit tunnel unplayable rather than atmospheric. Lighting must read as
   atmosphere, not as an unlit image; sealed ground is meant to be dim and
   obviously solid, never a black hole in the picture. */
#define WORLD_MINIMUM_LIGHT 0.40f
/* Transmission per light cell, i.e. per WORLD_LIGHT_SCALE world cells. Open air
   carries light across a large cavern; solid material swallows it over a few
   dozen cells, which is what makes a deep bore go dark while a shallow one still
   sees daylight. */
#define WORLD_LIGHT_OPEN_TRANSMISSION 0.97f
#define WORLD_LIGHT_SOLID_TRANSMISSION 0.74f
/* The solved light is quantised to this many steps. A pixel channel is one byte,
   so a finer change cannot alter the image; quantising lets the renderer compare
   light exactly instead of against a tolerance. A tolerance drifts: a sample
   that moves less than it each frame is never rebuilt, and the texture wanders
   arbitrarily far from the light it should be showing. */
#define WORLD_LIGHT_STEPS 512.0f
/* Temperature at which material starts to glow on its own, and the span over
   which that glow reaches full strength. */
#define WORLD_LIGHT_HEAT_FLOOR 180.0f
#define WORLD_LIGHT_HEAT_SPAN 520.0f

/* Light outside the visible region is still solved, but only out to this many
   light cells beyond it. Open air transmits 0.97 per light cell, so a source
   this far outside the window arrives at the visible edge at 0.97^128 = 2% of
   its strength — below what a byte-per-channel image can show and below the
   quantisation step the renderer compares against. Sky light is unaffected by
   the window at all: it is filled per column from the top, and a column is
   solved independently of its neighbours. */
#define WORLD_LIGHT_WINDOW_MARGIN 128

/* Refreshes the light inputs of every dirty chunk and re-solves the field when
   something that can change it has moved. Dirties any chunk whose light changed
   even though its cells did not, so the incremental renderer never shows a
   shaft that has been carved but not lit.

   `visible` is the region that must be correct, in cells. Solving the whole
   16384-wide field cost 10 ms every time the player's own lamp moved far enough
   to matter, which at flying speed is every single frame. */
void WorldUpdateLighting(World *world, Rectangle visible);

/* Resolves one axis of the bilinear sample: the two light rows or columns a cell
   falls between, and how far it sits between them. */
static inline void WorldLightAxis(int samples, int coordinate, int *low, int *high,
                                  float *blend)
{
    float position = ((float)coordinate + 0.5f) / (float)WORLD_LIGHT_SCALE - 0.5f;
    int floored = (int)floorf(position);

    *blend = position - (float)floored;
    *low = floored < 0 ? 0 : (floored > samples - 1 ? samples - 1 : floored);
    *high = floored + 1 < 0 ? 0
                            : (floored + 1 > samples - 1 ? samples - 1 : floored + 1);
}

/* Turns the two light channels into a multiplier per colour channel. Light that
   is mostly ember rather than sky is warmed, so a lava cavern glows orange
   instead of merely being less dark. Inline because the renderer calls it once
   per cell of every rebuilt chunk. */
static inline void WorldLightTint(float sky, float ember, float *red, float *green,
                                  float *blue)
{
    float brightest = ember > sky ? ember : sky;
    float level = WORLD_MINIMUM_LIGHT + (1.0f - WORLD_MINIMUM_LIGHT) * brightest;
    float warmth = ember > sky ? ember - sky : 0.0f;

    /* Plain comparisons rather than fmaxf: this runs for every cell of every
       dirty chunk, and a libm call per pixel is not free at that rate. */
    *red = level * (1.0f + 0.42f * warmth);
    *green = level * (1.0f - 0.06f * warmth);
    *blue = level * (1.0f - 0.44f * warmth);
}

#endif
