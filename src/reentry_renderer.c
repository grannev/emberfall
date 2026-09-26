/* The re-entry cap. See reentry_renderer.h for what it is; this file records
 * how it is shaped.
 *
 * In the frame of the thing burning — `forward` along its velocity, `side`
 * across it — the shock is a parabola opening backward:
 *
 *     u = standoff - v * v / (2 * curvature),   |v| <= halfWidth
 *
 * so its nose stands `standoff` ahead of the centre and its arms sweep back
 * past the sides. It is drawn in layers, the innermost white-hot and the
 * outer ones orange and red and broken up by noise, and from each end of the
 * arms streamers peel away backward and waver. Everything scales with the
 * size of what wears it and with its heat: a cap on the character is a few
 * cells across, a cap on a slab from orbit is as wide as the slab.
 */
#include "reentry_renderer.h"

#include <math.h>

#include "beam_render.h"

/* Below this the cap is not drawn at all: the first whisper of heat is the
   sparks' to show. */
#define REENTRY_CAP_MINIMUM_HEAT 0.06f
/* Streamers from each arm of the cap. */
#define REENTRY_STREAMERS 3

typedef struct ReentryShape {
    Vector2 centre;
    Vector2 forward;
    Vector2 side;
    float size;
    float heat;
    float block;
    int salt;
    bool air;
    float opacity;
} ReentryShape;

static float ReentryClamp(float value, float minimum, float maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static Vector2 ReentryPoint(const ReentryShape *shape, float u, float v)
{
    return (Vector2){
        shape->centre.x + shape->forward.x * u + shape->side.x * v,
        shape->centre.y + shape->forward.y * u + shape->side.y * v,
    };
}

/* Fire from white through yellow and orange to red as `depth` goes 0..1,
   with `alpha` over all of it. The emissive plane gets it brighter. */
static Color ReentryFire(float depth, float alpha, bool emissive)
{
    float d = ReentryClamp(depth, 0.0f, 1.0f);
    /* White at the shock, then a saturated yellow-orange, then red: the
       green channel falls faster than a straight line, so the middle of the
       ramp is orange rather than a pale tan. */
    float r = 255.0f;
    float g = 245.0f - 195.0f * sqrtf(d);
    float b = 215.0f - 205.0f * sqrtf(sqrtf(d));

    if (emissive) {
        g = fminf(255.0f, g + 20.0f);
        b = fminf(255.0f, b + 10.0f);
    }
    return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b,
                   (unsigned char)(255.0f * ReentryClamp(alpha, 0.0f, 1.0f))};
}

static Color ReentryColor(const ReentryShape *shape, float depth, float alpha,
                           bool emissive)
{
    if (!shape->air) return ReentryFire(depth, alpha, emissive);
    float d = ReentryClamp(depth, 0.0f, 1.0f);
    return (Color){(unsigned char)(225.0f - d * 70.0f),
                   (unsigned char)(239.0f - d * 61.0f),
                   (unsigned char)(233.0f - d * 48.0f),
                   (unsigned char)(255.0f * ReentryClamp(alpha * shape->opacity *
                                          (emissive ? 0.24f : 0.64f), 0.0f, 1.0f))};
}

static void ReentryDrawShape(const ReentryShape *shape, int frame, bool emissive)
{
    float glow = ReentryClamp((shape->heat - REENTRY_CAP_MINIMUM_HEAT) /
                                  (0.5f - REENTRY_CAP_MINIMUM_HEAT),
                              0.0f, 1.0f);
    float size = shape->size;
    float block = shape->block;
    /* Sized in cells as well as in the body's own size: on the character,
       two cells across, a cap in proportion would be a smudge on the
       sprite, and the thing being shown is the air, which does not care how
       small what is pushing it is. The nose stands off the leading face,
       further the harder it burns — the cap is pushed ahead by the air it
       is compressing — and the arms reach well past the sides. */
    float standoff = size * 1.1f + 2.5f + 1.5f * glow;
    float curvature = size * 1.0f + 3.0f;
    float halfWidth = size * 1.6f + 5.0f + 3.0f * glow;
    int layers = 3 + (int)(3.0f * glow + 0.5f);
    int layer;
    int arm;
    if (shape->air) layers = 4;

    glow = glow * glow * (3.0f - 2.0f * glow);

    /* The sheath: the air between the cap and the face, glowing through,
       thin and ragged, so the character is seen inside the fire rather than
       behind a wall of it. */
    for (layer = 1; layer <= 3; ++layer) {
        float inset = (float)layer * block * 1.2f;
        float v;

        for (v = -halfWidth * 0.7f; v <= halfWidth * 0.7f; v += block) {
            float u = standoff - inset - v * v / (2.0f * curvature);
            float noise = BeamNoise((int)floorf(v / block), frame + 41 * layer,
                                    shape->salt + 101);
            Vector2 point;

            if (u < size * 0.6f || noise < 0.35f + 0.12f * (float)layer) {
                continue;
            }
            point = ReentryPoint(shape, u, v);
            BeamBlock(point.x, point.y, block,
                      ReentryColor(shape, 0.35f + 0.15f * (float)layer,
                                  glow * (emissive ? 0.55f : 0.35f) /
                                      (float)layer,
                                  emissive));
        }
    }

    /* The cap: a solid white-hot rim at the nose, and outer layers going to
       orange and red and breaking up the further out they are. */
    for (layer = 0; layer < layers; ++layer) {
        float depth = (float)layer / (float)(layers > 1 ? layers - 1 : 1);
        float offset = (float)layer * block;
        float v;

        for (v = -halfWidth; v <= halfWidth; v += block * 0.6f) {
            float along = fabsf(v) / halfWidth;
            float u = standoff + offset - v * v / (2.0f * curvature);
            float noise = BeamNoise((int)floorf(v / block), frame + layer * 7,
                                    shape->salt + layer);
            /* Opaque enough that orange reads as orange: half-transparent
               fire over a night sky is brown. */
            float alpha = glow * (1.0f - 0.3f * along) * (1.0f - 0.3f * depth);
            Vector2 point;

            if (noise < (shape->air
                             ? 0.015f + 0.16f * depth + 0.18f * along * along
                             : 0.04f + 0.36f * depth + 0.3f * along * along)) {
                continue;
            }
            /* The flicker of the shock: each block breathes a little in and
               out along the flow. */
            u += (noise - 0.5f) * block * (0.4f + depth);
            point = ReentryPoint(shape, u, v);
            BeamBlock(point.x, point.y, block,
                      ReentryColor(shape, depth * 0.85f + along * 0.35f,
                                  emissive ? fminf(1.0f, alpha * 1.3f) : alpha,
                                  emissive));
        }
    }

    /* Streamers: the air that went round the cap, peeling off each arm and
       flowing back past the body, longer and more of them the hotter. */
    for (arm = -1; arm <= 1; arm += 2) {
        float armU = standoff - halfWidth * halfWidth / (2.0f * curvature);
        int streamer;

        for (streamer = 0; streamer < REENTRY_STREAMERS; ++streamer) {
            float spread = (float)streamer * block * 1.4f;
            float length = (size * 2.0f + 6.0f + 20.0f * glow) *
                           (0.6f + 0.5f * BeamNoise(streamer, frame / 2,
                                                    shape->salt + arm * 13));
            float t;
            /* Short ragged peel at the rim, never long straight speed lines. */
            if (shape->air) length = (3.0f + 5.0f * glow) * (1.0f - 0.2f * (float)streamer);

            for (t = 0.0f; t < length; t += block * 0.8f) {
                float fade = 1.0f - t / length;
                float waver = sinf((t / (size + 2.0f)) * 2.3f + (float)frame * 0.9f +
                                   (float)streamer * 1.7f) * block;
                float noise = BeamNoise((int)floorf(t / block), frame,
                                        shape->salt + 31 * streamer + arm * 5);
                Vector2 point;

                if (noise < 0.2f + 0.45f * (1.0f - fade)) {
                    continue;
                }
                /* Drawn in toward the wake as it goes: the flow closes behind
                   the body rather than fanning out for ever. */
                point = ReentryPoint(shape, armU - t,
                                     (float)arm * (halfWidth * (1.0f - 0.35f * (1.0f - fade)) +
                                                   spread) +
                                         waver);
                BeamBlock(point.x, point.y, block,
                          ReentryColor(shape, 0.35f + 0.6f * (1.0f - fade),
                                      glow * fade * 0.9f, emissive));
            }
        }
    }
}

static bool ReentryShapeFor(ReentryShape *shape, Vector2 centre, Vector2 velocity,
                            float size, float heat, int salt)
{
    float speed = sqrtf(velocity.x * velocity.x + velocity.y * velocity.y);

    if (heat < REENTRY_CAP_MINIMUM_HEAT || speed < 1.0f || size <= 0.0f) {
        return false;
    }
    shape->centre = centre;
    shape->forward = (Vector2){velocity.x / speed, velocity.y / speed};
    shape->side = (Vector2){-shape->forward.y, shape->forward.x};
    shape->size = size;
    shape->heat = heat;
    /* One cell, the size of a pixel of the world, on the character; a
       slab's cap is drawn in coarser blocks so its cost follows how many
       blocks it is, not how big it is. */
    shape->block = size / 8.0f > 1.0f ? size / 8.0f : 1.0f;
    shape->salt = salt;
    shape->air = false;
    shape->opacity = 1.0f;
    return true;
}

void ReentryRendererDrawAir(const Player *player, float density, float heat,
                            float time, bool emissive)
{
    ReentryShape shape;
    float speed;
    float power;
    if (player == NULL || density <= 0.0f) return;
    speed = sqrtf(player->velocity.x * player->velocity.x + player->velocity.y * player->velocity.y);
    power = ReentryClamp((speed - player->maxSpeed) /
                         fmaxf(1.0f, player->boostSpeed - player->maxSpeed), 0.0f, 1.0f);
    if (!ReentryShapeFor(&shape, player->position, player->velocity,
                         PlayerExtent(player) * 0.7f, power, 0x19a3)) return;
    shape.air = true;
    shape.opacity = ReentryClamp(density, 0.0f, 1.0f) *
                    (1.0f - ReentryClamp(heat * 2.0f, 0.0f, 1.0f));
    if (shape.opacity <= 0.0f) return;
    ReentryDrawShape(&shape, (int)floorf(time * 24.0f), emissive);
}

static bool ReentryVisible(Rectangle visible, Vector2 centre, float reach)
{
    return centre.x + reach >= visible.x &&
           centre.x - reach <= visible.x + visible.width &&
           centre.y + reach >= visible.y &&
           centre.y - reach <= visible.y + visible.height;
}

static void ReentryDrawAll(const AtmosphereSystem *atmosphere, const Player *player,
                           const DynamicTerrainSystem *terrain, Rectangle visible,
                           float time, bool emissive)
{
    ReentryShape shape;
    int frame = (int)floorf(time * 30.0f);
    int slot;

    if (atmosphere == NULL) {
        return;
    }
    if (player != NULL &&
        ReentryShapeFor(&shape, player->position, player->velocity,
                        PlayerExtent(player) * 0.7f, atmosphere->playerHeat, 0x5a11) &&
        ReentryVisible(visible, shape.centre, shape.size * 4.0f + 40.0f)) {
        ReentryDrawShape(&shape, frame, emissive);
    }
    if (terrain == NULL) {
        return;
    }
    for (slot = 0; slot < MAX_TERRAIN_BODIES; ++slot) {
        const TerrainBody *body = &terrain->bodies[slot];

        if (!body->active || atmosphere->bodyGeneration[slot] != body->generation) {
            continue;
        }
        if (!ReentryShapeFor(&shape, body->position, body->velocity,
                             body->boundingRadius, atmosphere->bodyHeat[slot],
                             slot * 97 + 3) ||
            !ReentryVisible(visible, shape.centre, shape.size * 4.0f + 40.0f)) {
            continue;
        }
        ReentryDrawShape(&shape, frame, emissive);
    }
}

void ReentryRendererDraw(const AtmosphereSystem *atmosphere, const Player *player,
                         const DynamicTerrainSystem *terrain, Rectangle visible,
                         float time)
{
    ReentryDrawAll(atmosphere, player, terrain, visible, time, false);
}

void ReentryRendererDrawEmissive(const AtmosphereSystem *atmosphere,
                                 const Player *player,
                                 const DynamicTerrainSystem *terrain,
                                 Rectangle visible, float time)
{
    ReentryDrawAll(atmosphere, player, terrain, visible, time, true);
}
