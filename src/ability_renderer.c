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

static void DrawBeam(const AbilityState *state, Color core, Color glow,
                     float glowWidth, float coreWidth, float hitRadius)
{
    DrawLineEx(state->origin, state->endpoint, glowWidth, glow);
    DrawLineEx(state->origin, state->endpoint, coreWidth, core);
    DrawCircleV(state->endpoint, state->hit ? hitRadius : hitRadius * 0.3f, glow);
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
        DrawCircleSectorLines(state->origin, radius, angle - halfAngle,
                              angle + halfAngle, 20,
                              (Color){182, 216, 255, alpha});
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

void AbilityRendererDraw(const AbilitySystem *abilities, Vector2 aimPosition)
{
    const AbilityState *laser;
    const AbilityState *cryo;
    const AbilityState *force;
    const AbilityState *explosion;
    Color crosshair;

    if (abilities == NULL) {
        return;
    }
    laser = AbilityStateAt(abilities, ABILITY_LASER);
    cryo = AbilityStateAt(abilities, ABILITY_CRYO);
    force = AbilityStateAt(abilities, ABILITY_FORCE);
    explosion = AbilityStateAt(abilities, ABILITY_EXPLOSION);
    crosshair = explosion->cooldown <= 0.0f ? (Color){255, 232, 118, 230}
                                            : (Color){180, 188, 199, 190};

    if (laser->active) {
        DrawLineEx(laser->origin, laser->endpoint, 1.7f,
                   (Color){255, 74, 31, 225});
        DrawLineEx(laser->origin, laser->endpoint, 0.62f,
                   (Color){255, 244, 188, 255});
        if (laser->hit) {
            Vector2 normal = {-laser->direction.y, laser->direction.x};

            DrawCircleV(laser->endpoint, 4.2f, (Color){255, 74, 24, 62});
            DrawCircleV(laser->endpoint, 2.5f, (Color){255, 161, 43, 205});
            DrawCircleV(laser->endpoint, 1.1f, (Color){255, 248, 203, 255});
            DrawLineEx(Vector2Add(laser->endpoint, Vector2Scale(normal, -3.6f)),
                       Vector2Add(laser->endpoint, Vector2Scale(normal, 3.6f)),
                       0.55f, (Color){255, 203, 106, 210});
        } else {
            DrawCircleV(laser->endpoint, 0.9f, (Color){255, 198, 88, 180});
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

    DrawCircleLinesV(aimPosition, 4.0f, crosshair);
    DrawLineV((Vector2){aimPosition.x - 6.0f, aimPosition.y},
              (Vector2){aimPosition.x + 6.0f, aimPosition.y}, crosshair);
    DrawLineV((Vector2){aimPosition.x, aimPosition.y - 6.0f},
              (Vector2){aimPosition.x, aimPosition.y + 6.0f}, crosshair);
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
        DrawCircleV(laser->endpoint, laser->hit ? 3.0f : 1.0f,
                    (Color){255, 126, 34, laser->hit ? 230 : 150});
    }
    if (cryo->active) {
        DrawLineEx(cryo->origin, cryo->endpoint, 2.4f,
                   (Color){69, 177, 242, 70});
        DrawLineEx(cryo->origin, cryo->endpoint, 1.15f,
                   (Color){104, 205, 255, 130});
        DrawCircleV(cryo->endpoint, cryo->hit ? 2.4f : 0.8f,
                    (Color){167, 232, 255, 130});
    }
}
