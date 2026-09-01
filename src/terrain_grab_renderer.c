/* The telekinetic hold. See terrain_grab_renderer.h for what it is for.
 *
 * Built from the same blocks as every other effect: the world is squares, and a
 * smooth gradient beam laid over it belongs to a different game. The shape is
 *
 *     a wide pale wedge from the hand, narrowing to the grip
 *     its edges outlined in white and broken up
 *     a white core block where the grip is
 *     orange rings turning around the grip, off centre and ragged
 *     sparks drifting in toward it
 *
 * and the soft light around all of it comes from the emissive pass and the
 * bloom behind it rather than from anything drawn here.
 */
#include "terrain_grab_renderer.h"

#include "beam_render.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

/* Everything the hold draws, in one place so the scene and emissive passes
   cannot drift apart: they differ only in the colours they are handed. */
static void GrabDraw(const TerrainInteractionSystem *system,
                     const DynamicTerrainSystem *terrain, const Player *player,
                     float time, Color beam, Color edge, Color ring,
                     Color spark, float block)
{
    const TerrainBody *body;
    Vector2 from;
    Vector2 to;
    float dx;
    float dy;
    float length;
    int frame;

    if (system == NULL || player == NULL ||
        !TerrainInteractionIsHolding(system, terrain)) {
        return;
    }
    body = DynamicTerrainGetConst(terrain, system->held);
    if (body == NULL) {
        return;
    }

    /* Both hands, to the cell actually being held rather than to the body's
       centre: the grip is on a corner of the rock and the picture should say
       so. Two strands from two hands is what makes the hold read as a grip
       rather than as a searchlight, and PlayerHandOrigin puts them exactly
       where the arms are drawn. */
    to = system->holdWorldPoint;
    from = PlayerHandOrigin(player, to, false);
    dx = to.x - from.x;
    dy = to.y - from.y;
    length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) {
        return;
    }
    /* Quantised, so the flicker steps rather than slides — and so the same
       moment always draws the same beam. */
    frame = (int)(time * 18.0f);

    BeamStrand(from, to, 2.4f, 0.9f, frame, beam, edge, block, 5);
    BeamStrand(PlayerHandOrigin(player, to, true), to, 2.4f, 0.9f, frame, beam,
               edge, block, 23);

    /* The grip itself: a hard white core. */
    BeamBlock(to.x, to.y, block * 1.5f, edge);

    /* Rings turning around the grip, off centre and gappy. */
    BeamRings(to, time, 2, 4.5f, 3.0f, ring, block);

    /* Sparks drifting in toward the grip, so the hold looks like it is pulling
       rather than merely touching. */
    BeamSparks(from, to, time, 10, 4.5f, false, spark, block);
}

void TerrainGrabRendererDrawScene(const TerrainInteractionSystem *system,
                                  const DynamicTerrainSystem *terrain,
                                  const Player *player, float time)
{
    GrabDraw(system, terrain, player, time,
             (Color){126, 158, 240, 150},   /* the body of the beam */
             (Color){226, 240, 255, 232},   /* its outline and the grip */
             (Color){255, 156, 74, 210},    /* the rings */
             (Color){178, 226, 255, 225},   /* sparks */
             1.0f);
}

void TerrainGrabRendererDrawEmissive(const TerrainInteractionSystem *system,
                                     const DynamicTerrainSystem *terrain,
                                     const Player *player, float time)
{
    /* Dimmer than the scene pass and drawn with coarser blocks: this is what
       the bloom spreads into the glow around the beam, and a fine one costs
       many times the fill for a result the blur erases anyway. */
    /* Dim, because two thin strands a hand's width apart are what the hold is
       supposed to read as: at the old brightness the bloom around them met in
       the middle and drew one bar of light, which is the thing this stopped
       being. */
    GrabDraw(system, terrain, player, time,
             (Color){44, 62, 116, 255}, (Color){118, 148, 200, 255},
             (Color){150, 74, 26, 255}, (Color){84, 122, 156, 255}, 2.0f);
}
