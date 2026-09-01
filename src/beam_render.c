/* The shared block language for beams. See beam_render.h. */
#include "beam_render.h"

#include <math.h>
#include <stdbool.h>

float BeamNoise(int a, int b, int salt)
{
    unsigned int h = (unsigned int)a * 374761393u ^ (unsigned int)b * 668265263u ^
                     (unsigned int)salt * 2246822519u;

    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

void BeamBlock(float x, float y, float block, Color color)
{
    if (block <= 0.0f) {
        return;
    }
    DrawRectangleV((Vector2){floorf(x / block) * block,
                             floorf(y / block) * block},
                   (Vector2){block, block}, color);
}

void BeamStrand(Vector2 from, Vector2 to, float halfFrom, float halfTo,
                int frame, Color beam, Color edge, float block, int salt)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float length = sqrtf(dx * dx + dy * dy);
    Vector2 along;
    Vector2 across;
    int steps;
    int step;

    if (length < 0.001f || block <= 0.0f) {
        return;
    }
    along = (Vector2){dx / length, dy / length};
    across = (Vector2){-along.y, along.x};
    /* Sampled at half a block along its length and across it. A whole block is
       the obvious stride and it leaves a dotted line: consecutive ranks of a
       diagonal beam snap onto the same cell, so every other cell along the
       diagonal is missed. The duplicates a half stride produces cost an
       overdraw of the same square and nothing else. */
    steps = (int)(length / (block * 0.5f));
    if (steps < 1) steps = 1;
    /* A beam that reaches the far edge of a large world must not turn into an
       unbounded loop; past this the strand is drawn coarser rather than longer,
       and nothing that far away is legible anyway. */
    if (steps > 1800) steps = 1800;

    for (step = 0; step <= steps; ++step) {
        float amount = (float)step / (float)steps;
        float half = (halfFrom + (halfTo - halfFrom) * amount) *
                     (1.0f + 0.18f * (BeamNoise(step, frame, salt) - 0.5f));
        float offset;
        Vector2 spine = {from.x + along.x * length * amount,
                         from.y + along.y * length * amount};

        if (half < block * 0.5f) half = block * 0.5f;
        for (offset = -half; offset <= half; offset += block * 0.5f) {
            float x = spine.x + across.x * offset;
            float y = spine.y + across.y * offset;
            bool rim = fabsf(offset) > half - block * 0.75f;

            /* A broken interior, so the beam reads as energy rather than as a
               painted stripe. The rim is never dropped: an outline with holes
               in it stops reading as an outline. */
            if (!rim &&
                BeamNoise(step, (int)(offset / block), frame + salt) < 0.22f) {
                continue;
            }
            BeamBlock(x, y, block, rim ? edge : beam);
        }
    }
}

void BeamRings(Vector2 centre, float time, int count, float innerRadius,
               float spacing, Color color, float block)
{
    int frame = (int)(time * 18.0f);
    int index;

    for (index = 0; index < count; ++index) {
        float radius = innerRadius + (float)index * spacing +
                       sinf(time * (2.2f + (float)index)) * 0.6f;
        /* Counter-rotating, which is what made the reference read as a vortex
           rather than as a target reticle. */
        float spin = time * (index % 2 == 0 ? 2.4f : -1.7f);
        int dots = 14 + index * 6;
        int dot;

        for (dot = 0; dot < dots; ++dot) {
            float angle = spin + (float)dot / (float)dots * 6.283185f;
            float wobble = 1.0f + (BeamNoise(dot, index, 9) - 0.5f) * 0.30f;

            if (BeamNoise(dot, index + frame / 4, 13) < 0.30f) {
                continue;
            }
            BeamBlock(centre.x + cosf(angle) * radius * wobble,
                      centre.y + sinf(angle) * radius * wobble, block, color);
        }
    }
}

void BeamSparks(Vector2 from, Vector2 to, float time, int count, float spread,
                bool outward, Color color, float block)
{
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float length = sqrtf(dx * dx + dy * dy);
    Vector2 along;
    Vector2 across;
    int index;

    if (length < 0.001f) {
        return;
    }
    along = (Vector2){dx / length, dy / length};
    across = (Vector2){-along.y, along.x};

    for (index = 0; index < count; ++index) {
        float phase = BeamNoise(index, 0, 17);
        float travel = fmodf(phase + time * 0.9f, 1.0f);
        float side = (BeamNoise(index, 1, 19) - 0.5f) * spread;
        Vector2 at;

        if (!outward) {
            travel = 1.0f - travel;
        }
        at = (Vector2){from.x + along.x * length * travel + across.x * side,
                       from.y + along.y * length * travel + across.y * side};
        BeamBlock(at.x, at.y, block, color);
    }
}
