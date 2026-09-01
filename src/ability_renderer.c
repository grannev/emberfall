/* Drawing for the player's powers.
 *
 * Each ability reads its own AbilityState — where it started, where it landed,
 * whether it hit, and how much follow-through is left — and nothing here can
 * change what the simulation did. Adding a power means adding one case; the
 * geometry constants come from abilities.h so a cone is never drawn at a
 * different angle from the one that was actually applied.
 */
#include "ability_renderer.h"

#include "beam_render.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

/* The same block language the presentation effects use. A beam that ends in a
   perfect little vector circle reads as belonging to another game; what the
   reference shows at a contact point is a knot of square embers inside a large
   soft glow, and the glow is the emissive pass's job rather than this one's. */
static float AbilityFxNoise(int x, int y, int salt)
{
    unsigned int h = (unsigned int)x * 374761393u ^
                     (unsigned int)y * 668265263u ^
                     (unsigned int)salt * 2246822519u;

    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

static void AbilityBlock(float x, float y, float block, Color color)
{
    DrawRectangleV((Vector2){floorf(x / block) * block,
                             floorf(y / block) * block},
                   (Vector2){block, block}, color);
}

/* A ragged cluster of blocks: what a beam actually leaves where it lands. */
static void AbilityContactBlocks(Vector2 at, float radius, float block,
                                 Color color)
{
    float y;
    int salt = (int)(at.x * 3.0f + at.y * 5.0f);

    for (y = -radius; y <= radius; y += block) {
        float x;

        for (x = -radius; x <= radius; x += block) {
            float distance = sqrtf(x * x + y * y);
            float edge;

            if (distance > radius) {
                continue;
            }
            edge = radius > 0.0001f ? distance / radius : 0.0f;
            if (edge > 0.5f &&
                AbilityFxNoise((int)floorf((at.x + x) / block),
                               (int)floorf((at.y + y) / block), salt) <
                    (edge - 0.5f) / 0.5f) {
                continue;
            }
            AbilityBlock(at.x + x, at.y + y, block, color);
        }
    }
}

/* Blocks scattered along an arc, instead of a drawn curve. */
static void AbilityArcBlocks(Vector2 centre, float radius, float fromAngle,
                             float toAngle, float block, Color color, int salt)
{
    int count = (int)(fabsf(toAngle - fromAngle) * radius / (block * 1.4f));
    int index;

    if (radius <= 0.0f || count <= 0) {
        return;
    }
    if (count > 160) count = 160;
    for (index = 0; index <= count; ++index) {
        float amount = (float)index / (float)count;
        float angle = fromAngle + (toAngle - fromAngle) * amount;
        float wobble = 1.0f + (AbilityFxNoise(index, salt, 3) - 0.5f) * 0.14f;

        if (AbilityFxNoise(index, salt, 9) < 0.3f) {
            continue;
        }
        AbilityBlock(centre.x + cosf(angle) * radius * wobble,
                     centre.y + sinf(angle) * radius * wobble, block, color);
    }
}

/* Where a drawn beam leaves the character. The stored origin is where the
   simulation cast the ray from — a step clear of the face — and drawing from it
   left a visible gap in front of the visor. This is the visor itself, rebuilt
   from the player being drawn this frame. */
static Vector2 BeamStart(const AbilityState *state, const Player *player)
{
    if (player == NULL) {
        return state->origin;
    }
    return PlayerVisorOrigin(player, state->endpoint);
}

static void DrawForceArc(const AbilityState *state, float duration)
{
    /* An arc racing outward along the cone, so the blow reads as a single
       moment of impact travelling away from the player. */
    float progress = 1.0f - state->effectTime / duration;
    float angle = atan2f(state->direction.y, state->direction.x) * RAD2DEG;
    float halfAngle = acosf(Clamp(ABILITY_FORCE_SPREAD_COSINE, -1.0f, 1.0f)) *
                      RAD2DEG;
    int ring;

    /* Crisp cone edges make the pressure direction legible even when dust and
       bloom overlap the centre of the blow. */
    {
        float left = (angle - halfAngle) * DEG2RAD;
        float right = (angle + halfAngle) * DEG2RAD;
        float edgeLength = ABILITY_FORCE_LENGTH * (0.72f + progress * 0.28f);
        Color edge = Fade((Color){187, 224, 245, 255},
                          (1.0f - progress) * 0.42f);

        /* In blocks like every other beam. These two were the last smooth
           lines left in the abilities, and a hairline vector edge beside a
           blocky cone reads as two effects from two different games. */
        BeamStrand(state->origin,
                   Vector2Add(state->origin,
                              (Vector2){cosf(left) * edgeLength,
                                        sinf(left) * edgeLength}),
                   0.6f, 0.6f, (int)(progress * 32.0f), edge, edge, 1.0f, 41);
        BeamStrand(state->origin,
                   Vector2Add(state->origin,
                              (Vector2){cosf(right) * edgeLength,
                                        sinf(right) * edgeLength}),
                   0.6f, 0.6f, (int)(progress * 32.0f), edge, edge, 1.0f, 43);
    }

    for (ring = 0; ring < 4; ++ring) {
        float radius = 10.0f + (ABILITY_FORCE_LENGTH - 10.0f) * progress -
                       (float)ring * 6.0f;
        unsigned char alpha;

        if (radius <= 0.0f) {
            continue;
        }
        alpha = (unsigned char)Clamp((1.0f - progress) * 210.0f -
                                         (float)ring * 40.0f,
                                     0.0f, 255.0f);
        AbilityArcBlocks(state->origin, radius, (angle - halfAngle) * DEG2RAD,
                         (angle + halfAngle) * DEG2RAD, 1.6f,
                         (Color){182, 216, 255, alpha}, ring);
    }
}

static void DrawCryoEdge(const AbilityState *state, Vector2 start)
{
    Vector2 delta = Vector2Subtract(state->endpoint, start);
    float length = Vector2Length(delta);
    Vector2 direction;
    Vector2 normal;
    int shard;

    if (length <= 2.0f) {
        return;
    }
    direction = Vector2Scale(delta, 1.0f / length);
    normal = (Vector2){-direction.y, direction.x};
    for (shard = 1; shard <= 7; ++shard) {
        float amount = (float)shard / 8.0f;
        float side = (shard & 1) == 0 ? 1.0f : -1.0f;
        Vector2 point = Vector2Add(start, Vector2Scale(delta, amount));
        Vector2 tip = Vector2Add(
            Vector2Add(point, Vector2Scale(normal, side * 1.6f)),
            Vector2Scale(direction, -1.2f));

        /* Frost growing off the beam, in blocks like everything else: a hairline
           vector spike was the last smooth line left in the picture. */
        BeamStrand(point, tip, 0.6f, 0.6f, shard, (Color){200, 244, 255, 190},
                   (Color){236, 253, 255, 220}, 1.0f, shard * 7);
    }
}

void AbilityRendererDraw(const AbilitySystem *abilities, const Player *player,
                         float time)
{
    const AbilityState *laser;
    const AbilityState *cryo;
    const AbilityState *force;
    /* Quantised, so the flicker steps rather than slides and so the same moment
       always draws the same beam. */
    int frame = (int)(time * 18.0f);

    if (abilities == NULL) {
        return;
    }
    laser = AbilityStateAt(abilities, ABILITY_LASER);
    cryo = AbilityStateAt(abilities, ABILITY_CRYO);
    force = AbilityStateAt(abilities, ABILITY_FORCE);

    if (laser->active) {
        Vector2 start = BeamStart(laser, player);

        /* Narrow and hot, tapering to a point where it bites. The eye end is
           the wide end so the beam reads as leaving the face rather than as
           being aimed at it. */
        BeamStrand(start, laser->endpoint, 1.9f, 1.0f, frame,
                   (Color){255, 96, 34, 210}, (Color){255, 238, 186, 240},
                   1.0f, 3);
        if (laser->hit) {
            AbilityContactBlocks(laser->endpoint, 4.2f, 1.0f,
                                 (Color){255, 74, 24, 62});
            AbilityContactBlocks(laser->endpoint, 2.5f, 1.0f,
                                 (Color){255, 161, 43, 205});
            AbilityContactBlocks(laser->endpoint, 1.1f, 1.0f,
                                 (Color){255, 248, 203, 255});
            BeamRings(laser->endpoint, time, 2, 3.4f, 2.6f,
                      (Color){255, 156, 62, 200}, 1.0f);
            /* Thrown back out of the cut, which is the direction sparks
               actually leave a hole being burned. */
            BeamSparks(laser->endpoint, start, time, 8, 4.0f, true,
                       (Color){255, 214, 130, 225}, 1.0f);
        } else {
            AbilityContactBlocks(laser->endpoint, 0.9f, 1.0f,
                                 (Color){255, 198, 88, 180});
        }
    }

    if (cryo->active) {
        Vector2 start = BeamStart(cryo, player);

        /* Wider and softer than the laser, and it does not taper: a freezing
           cone spreads where a cutting beam narrows. */
        BeamStrand(start, cryo->endpoint, 1.5f, 2.1f, frame,
                   (Color){104, 194, 240, 190}, (Color){228, 249, 255, 235},
                   1.0f, 11);
        DrawCryoEdge(cryo, start);
        if (cryo->hit) {
            AbilityContactBlocks(cryo->endpoint, 3.8f, 1.0f,
                                 (Color){88, 196, 244, 120});
            AbilityContactBlocks(cryo->endpoint, 1.8f, 1.0f,
                                 (Color){236, 253, 255, 235});
            BeamRings(cryo->endpoint, time, 2, 4.0f, 3.0f,
                      (Color){170, 232, 255, 190}, 1.0f);
        }
    }

    if (force->effectTime > 0.0f) {
        DrawForceArc(force, AbilityDefinitionAt(ABILITY_FORCE)->effectTime);
    }
}

void AbilityRendererDrawReticle(const AbilitySystem *abilities,
                                Vector2 aimPosition)
{
    const AbilityState *explosion;
    Color crosshair;

    if (abilities == NULL) {
        return;
    }
    explosion = AbilityStateAt(abilities, ABILITY_EXPLOSION);
    crosshair = explosion->cooldown <= 0.0f ? (Color){255, 232, 118, 230}
                                            : (Color){180, 188, 199, 190};

    /* Four corner ticks rather than a drawn circle: the crosshair is the one
       piece of interface sitting in the world, and a smooth ring is the most
       obvious thing in the frame that is not made of cells. */
    {
        float arm = 2.0f;
        float gap = 3.0f;
        int corner;

        for (corner = 0; corner < 4; ++corner) {
            float signX = (corner & 1) ? 1.0f : -1.0f;
            float signY = (corner & 2) ? 1.0f : -1.0f;

            DrawLineV((Vector2){aimPosition.x + signX * gap,
                                aimPosition.y + signY * gap},
                      (Vector2){aimPosition.x + signX * (gap + arm),
                                aimPosition.y + signY * gap}, crosshair);
            DrawLineV((Vector2){aimPosition.x + signX * gap,
                                aimPosition.y + signY * gap},
                      (Vector2){aimPosition.x + signX * gap,
                                aimPosition.y + signY * (gap + arm)}, crosshair);
        }
    }
}

void AbilityRendererDrawEmissive(const AbilitySystem *abilities,
                                 const Player *player, float time)
{
    const AbilityState *laser;
    const AbilityState *cryo;
    int frame = (int)(time * 18.0f);

    if (abilities == NULL) {
        return;
    }
    laser = AbilityStateAt(abilities, ABILITY_LASER);
    cryo = AbilityStateAt(abilities, ABILITY_CRYO);

    /* Dimmer than the scene pass and drawn with coarser blocks: this is what
       the bloom spreads into the glow around a beam, and a fine one costs many
       times the fill for a result the blur erases anyway. */
    if (laser->active) {
        Vector2 start = BeamStart(laser, player);

        BeamStrand(start, laser->endpoint, 2.0f, 1.1f, frame,
                   (Color){168, 54, 18, 255}, (Color){255, 214, 150, 255},
                   2.0f, 3);
        AbilityContactBlocks(laser->endpoint, laser->hit ? 3.0f : 1.0f, 1.0f,
                             (Color){255, 126, 34, laser->hit ? 230 : 150});
    }
    if (cryo->active) {
        Vector2 start = BeamStart(cryo, player);

        BeamStrand(start, cryo->endpoint, 1.6f, 2.2f, frame,
                   (Color){48, 118, 168, 255}, (Color){160, 216, 246, 255},
                   2.0f, 11);
        AbilityContactBlocks(cryo->endpoint, cryo->hit ? 2.4f : 0.8f, 1.0f,
                             (Color){167, 232, 255, 130});
    }
}
