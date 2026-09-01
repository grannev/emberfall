/* Drawing for the player's powers.
 *
 * Each ability reads its own AbilityState — where it started, where it landed,
 * whether it hit, and how much follow-through is left — and nothing here can
 * change what the simulation did. Adding a power means adding one case; the
 * geometry constants come from abilities.h so a cone is never drawn at a
 * different angle from the one that was actually applied.
 */
#include "ability_renderer.h"

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

static void DrawBeam(const AbilityState *state, Color core, Color glow,
                     float glowWidth, float coreWidth, float hitRadius)
{
    DrawLineEx(state->origin, state->endpoint, glowWidth, glow);
    DrawLineEx(state->origin, state->endpoint, coreWidth, core);
    AbilityContactBlocks(state->endpoint,
                         state->hit ? hitRadius : hitRadius * 0.3f, 1.0f, glow);
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

        DrawLineEx(state->origin,
                   Vector2Add(state->origin,
                              (Vector2){cosf(left) * edgeLength,
                                        sinf(left) * edgeLength}),
                   0.55f, edge);
        DrawLineEx(state->origin,
                   Vector2Add(state->origin,
                              (Vector2){cosf(right) * edgeLength,
                                        sinf(right) * edgeLength}),
                   0.55f, edge);
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

static void DrawCryoEdge(const AbilityState *state)
{
    Vector2 delta = Vector2Subtract(state->endpoint, state->origin);
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
        Vector2 point = Vector2Add(state->origin,
                                   Vector2Scale(delta, amount));
        Vector2 tip = Vector2Add(
            Vector2Add(point, Vector2Scale(normal, side * 1.6f)),
            Vector2Scale(direction, -1.2f));

        DrawLineEx(point, tip, 0.45f, (Color){200, 244, 255, 190});
    }
}

void AbilityRendererDraw(const AbilitySystem *abilities)
{
    const AbilityState *laser;
    const AbilityState *cryo;
    const AbilityState *force;

    if (abilities == NULL) {
        return;
    }
    laser = AbilityStateAt(abilities, ABILITY_LASER);
    cryo = AbilityStateAt(abilities, ABILITY_CRYO);
    force = AbilityStateAt(abilities, ABILITY_FORCE);

    if (laser->active) {
        DrawLineEx(laser->origin, laser->endpoint, 1.7f,
                   (Color){255, 74, 31, 225});
        DrawLineEx(laser->origin, laser->endpoint, 0.62f,
                   (Color){255, 244, 188, 255});
        if (laser->hit) {
            Vector2 normal = {-laser->direction.y, laser->direction.x};

            AbilityContactBlocks(laser->endpoint, 4.2f, 1.0f,
                                 (Color){255, 74, 24, 62});
            AbilityContactBlocks(laser->endpoint, 2.5f, 1.0f,
                                 (Color){255, 161, 43, 205});
            AbilityContactBlocks(laser->endpoint, 1.1f, 1.0f,
                                 (Color){255, 248, 203, 255});
            DrawLineEx(Vector2Add(laser->endpoint, Vector2Scale(normal, -3.6f)),
                       Vector2Add(laser->endpoint, Vector2Scale(normal, 3.6f)),
                       0.55f, (Color){255, 203, 106, 210});
        } else {
            AbilityContactBlocks(laser->endpoint, 0.9f, 1.0f,
                                 (Color){255, 198, 88, 180});
        }
    }

    if (cryo->active) {
        DrawBeam(cryo, (Color){239, 253, 255, 245},
                 (Color){88, 196, 244, 175}, 1.65f, 0.56f, 3.8f);
        DrawCryoEdge(cryo);
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

void AbilityRendererDrawEmissive(const AbilitySystem *abilities)
{
    const AbilityState *laser;
    const AbilityState *cryo;

    if (abilities == NULL) {
        return;
    }
    laser = AbilityStateAt(abilities, ABILITY_LASER);
    cryo = AbilityStateAt(abilities, ABILITY_CRYO);

    if (laser->active) {
        DrawLineEx(laser->origin, laser->endpoint, 4.0f,
                   (Color){255, 45, 12, 84});
        DrawLineEx(laser->origin, laser->endpoint, 1.85f,
                   (Color){255, 72, 24, 230});
        DrawLineEx(laser->origin, laser->endpoint, 0.68f,
                   (Color){255, 238, 166, 255});
        AbilityContactBlocks(laser->endpoint, laser->hit ? 3.0f : 1.0f, 1.0f,
                             (Color){255, 126, 34, laser->hit ? 230 : 150});
    }
    if (cryo->active) {
        DrawLineEx(cryo->origin, cryo->endpoint, 2.4f,
                   (Color){69, 177, 242, 70});
        DrawLineEx(cryo->origin, cryo->endpoint, 1.15f,
                   (Color){104, 205, 255, 130});
        AbilityContactBlocks(cryo->endpoint, cryo->hit ? 2.4f : 0.8f, 1.0f,
                             (Color){167, 232, 255, 130});
    }
}
