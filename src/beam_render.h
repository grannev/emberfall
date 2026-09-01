#ifndef BEAM_RENDER_H
#define BEAM_RENDER_H

/* The block language every beam in the game is drawn in.
 *
 * The world is squares. A smooth gradient line laid over it belongs to a
 * different game, and for a long time the laser and the cryo beam were exactly
 * that — two `DrawLineEx` calls — while the telekinetic hold was built out of
 * grid-snapped blocks with broken edges. The hold read as force; the beams read
 * as vector art someone had drawn on top of the screenshot.
 *
 * This is the hold's strand, lifted out so that every beam is the same thing
 * with different colours. It lives in its own module rather than in one of the
 * renderers because a second copy is a second chance for two beams to drift
 * apart, which is the same mistake the beam origin already made once.
 *
 * Presentation only: nothing here reads or writes simulation state, and every
 * number it draws is one a caller already decided. `frame` is a quantised time
 * so that the flicker steps rather than slides and so that the same moment
 * always draws the same beam — which is what lets the smoke run photograph one.
 */

#include <raylib.h>

/* Deterministic 0..1 hash. Exposed because callers build their own sparks and
   rings from it and must agree with the strand about what "random" means. */
float BeamNoise(int a, int b, int salt);

/* One grid-snapped square of `block` size, snapped in world space rather than
   relative to the caller, so neighbouring pieces of one effect line up. */
void BeamBlock(float x, float y, float block, Color color);

/* A tapering strand from `from` to `to`.
 *
 * `halfFrom` and `halfTo` are the half-widths at each end, in cells, so a beam
 * can widen or narrow along its length. The interior is broken by noise, the
 * two outer blocks of every rank are drawn in `edge`, and the whole thing
 * wobbles a little with `frame`. `salt` separates strands that share a frame,
 * so two beams from two hands never flicker in step. */
void BeamStrand(Vector2 from, Vector2 to, float halfFrom, float halfTo,
                int frame, Color beam, Color edge, float block, int salt);

/* Gappy counter-rotating rings around a point: what a beam does where it lands
   or where it grips. `count` is how many rings. */
void BeamRings(Vector2 centre, float time, int count, float innerRadius,
               float spacing, Color color, float block);

/* Sparks travelling along a beam. `outward` sends them from `from` toward `to`,
   which is what a beam burning into rock throws; the hold pulls them the other
   way. */
void BeamSparks(Vector2 from, Vector2 to, float time, int count, float spread,
                bool outward, Color color, float block);

#endif
