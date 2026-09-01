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
    int steps;
    int step;
    int index;

    if (system == NULL || player == NULL ||
        !TerrainInteractionIsHolding(system, terrain)) {
        return;
    }
    body = DynamicTerrainGetConst(terrain, system->held);
    if (body == NULL) {
        return;
    }

    /* From the hand, which is the same point every beam leaves from, to the
       cell actually being held rather than to the body's centre: the grip is on
       a corner of the rock and the picture should say so. */
    from = PlayerBeamOrigin(player, system->holdWorldPoint);
    to = system->holdWorldPoint;
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

    /* The wedge: wide at the hand, narrowing to the grip. */
    steps = (int)(length / block);
    if (steps < 1) steps = 1;
    if (steps > 900) steps = 900;
    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        float half = (6.2f - 3.6f * amount) * (1.0f + 0.16f *
                     (GrabNoise(step, frame, 5) - 0.5f));
        float offset;
        Vector2 spine = {from.x + along.x * length * amount,
                         from.y + along.y * length * amount};

        for (offset = -half; offset <= half; offset += block) {
            float x = spine.x + across.x * offset;
            float y = spine.y + across.y * offset;
            bool rim = fabsf(offset) > half - block * 1.2f;

            /* A broken interior, so the beam reads as force rather than as a
               painted stripe. */
            if (!rim && GrabNoise(step, (int)(offset / block), frame) < 0.22f) {
                continue;
            }
            GrabBlock(x, y, block, rim ? edge : beam);
        }
    }

    /* The grip itself: a hard white core. */
    GrabBlock(to.x, to.y, block * 2.0f, edge);

    /* Rings turning around the grip, off centre and gappy. Two of them at
       different rates, which is what made the reference read as a vortex
       rather than as a target reticle. */
    for (index = 0; index < 2; ++index) {
        float radius = 6.5f + (float)index * 4.5f +
                       sinf(time * (2.2f + (float)index)) * 0.8f;
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
        float side = (GrabNoise(index, 1, 19) - 0.5f) * 7.0f;
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
    GrabDraw(system, terrain, player, time,
             (Color){70, 96, 168, 255}, (Color){188, 214, 255, 255},
             (Color){186, 96, 34, 255}, (Color){120, 168, 208, 255}, 2.0f);
}
