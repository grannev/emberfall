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

#include <math.h>
#include <stddef.h>

#include <raymath.h>

static float GrabNoise(int a, int b, int salt)
{
    unsigned int h = (unsigned int)a * 374761393u ^ (unsigned int)b * 668265263u ^
                     (unsigned int)salt * 2246822519u;

    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

static void GrabBlock(float x, float y, float block, Color color)
{
    DrawRectangleV((Vector2){floorf(x / block) * block,
                             floorf(y / block) * block},
                   (Vector2){block, block}, color);
}

/* Everything the hold draws, in one place so the scene and emissive passes
   cannot drift apart: they differ only in the colours they are handed. */
/* One of the two strands, from a hand to the grip. Both are drawn by the same
   code so they cannot drift apart; only where they start differs. */
static void GrabStrand(Vector2 from, Vector2 to, int frame, Color beam,
                       Color edge, float block, int salt)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float length = sqrtf(dx * dx + dy * dy);
    Vector2 along;
    Vector2 across;
    int steps;
    int step;

    if (length < 0.001f) {
        return;
    }
    along = (Vector2){dx / length, dy / length};
    across = (Vector2){-along.y, along.x};
    steps = (int)(length / block);
    if (steps < 1) steps = 1;
    if (steps > 900) steps = 900;

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        /* Narrow, and narrowing further toward the grip: two thin strands read
           as a grip, where one wide wedge read as a floodlight. */
        float half = (2.4f - 1.5f * amount) *
                     (1.0f + 0.18f * (GrabNoise(step, frame, salt) - 0.5f));
        float offset;
        Vector2 spine = {from.x + along.x * length * amount,
                         from.y + along.y * length * amount};

        for (offset = -half; offset <= half; offset += block) {
            float x = spine.x + across.x * offset;
            float y = spine.y + across.y * offset;
            bool rim = fabsf(offset) > half - block;

            /* A broken interior, so the beam reads as force rather than as a
               painted stripe. */
            if (!rim &&
                GrabNoise(step, (int)(offset / block), frame + salt) < 0.22f) {
                continue;
            }
            GrabBlock(x, y, block, rim ? edge : beam);
        }
    }
}

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
    Vector2 along;
    Vector2 across;
    int frame;
    int index;

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
    along = (Vector2){dx / length, dy / length};
    across = (Vector2){-along.y, along.x};
    /* Quantised, so the flicker steps rather than slides — and so the same
       moment always draws the same beam. */
    frame = (int)(time * 18.0f);

    GrabStrand(from, to, frame, beam, edge, block, 5);
    GrabStrand(PlayerHandOrigin(player, to, true), to, frame, beam, edge, block,
               23);

    /* The grip itself: a hard white core. */
    GrabBlock(to.x, to.y, block * 1.5f, edge);

    /* Rings turning around the grip, off centre and gappy. Two of them at
       different rates, which is what made the reference read as a vortex
       rather than as a target reticle. */
    for (index = 0; index < 2; ++index) {
        float radius = 4.5f + (float)index * 3.0f +
                       sinf(time * (2.2f + (float)index)) * 0.6f;
        float spin = time * (index == 0 ? 2.4f : -1.7f);
        int count = 14 + index * 6;
        int dot;

        for (dot = 0; dot < count; ++dot) {
            float angle = spin + (float)dot / (float)count * 6.283185f;
            float wobble = 1.0f + (GrabNoise(dot, index, 9) - 0.5f) * 0.30f;

            if (GrabNoise(dot, index + frame / 4, 13) < 0.30f) {
                continue;
            }
            GrabBlock(to.x + cosf(angle) * radius * wobble,
                      to.y + sinf(angle) * radius * wobble, block, ring);
        }
    }

    /* Sparks drifting in toward the grip along the beam, so the hold looks like
       it is pulling rather than merely touching. */
    for (index = 0; index < 10; ++index) {
        float phase = GrabNoise(index, 0, 17);
        float travel = 1.0f - fmodf(phase + time * 0.9f, 1.0f);
        float side = (GrabNoise(index, 1, 19) - 0.5f) * 4.5f;
        Vector2 at = {from.x + along.x * length * travel + across.x * side,
                      from.y + along.y * length * travel + across.y * side};

        GrabBlock(at.x, at.y, block, spark);
    }
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
