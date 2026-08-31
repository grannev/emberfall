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

static void DrawShockwave(const AbilityState *state, float duration)
{
    float progress = 1.0f - state->effectTime / duration;
    float radius = (float)ABILITY_EXPLOSION_CORE_RADIUS +
                   (ABILITY_EXPLOSION_SHOCK_RADIUS -
                    (float)ABILITY_EXPLOSION_CORE_RADIUS) * progress;
    Color ring = Fade((Color){255, 207, 118, 255}, 1.0f - progress);

    DrawCircleLinesV(state->origin, radius, ring);
    DrawCircleLinesV(state->origin, radius + 0.8f, Fade(ring, 0.35f));
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
        DrawLineEx(laser->origin, laser->endpoint, 1.6f, (Color){255, 81, 43, 210});
        DrawLineEx(laser->origin, laser->endpoint, 0.55f,
                   (Color){255, 244, 188, 255});
        if (laser->hit) {
            DrawCircleV(laser->endpoint, 4.2f, (Color){255, 74, 24, 70});
            DrawCircleV(laser->endpoint, 2.5f, (Color){255, 161, 43, 190});
            DrawCircleV(laser->endpoint, 1.1f, (Color){255, 248, 203, 255});
        } else {
            DrawCircleV(laser->endpoint, 0.9f, (Color){255, 198, 88, 180});
        }
    }

    if (cryo->active) {
        DrawBeam(cryo, (Color){232, 250, 255, 220}, (Color){126, 214, 255, 150},
                 1.4f, 0.5f, 3.6f);
    }

    if (force->effectTime > 0.0f) {
        DrawForceArc(force, AbilityDefinitionAt(ABILITY_FORCE)->effectTime);
    }

    if (explosion->effectTime > 0.0f) {
        DrawShockwave(explosion, AbilityDefinitionAt(ABILITY_EXPLOSION)->effectTime);
    }

    DrawCircleLinesV(aimPosition, 4.0f, crosshair);
    DrawLineV((Vector2){aimPosition.x - 6.0f, aimPosition.y},
              (Vector2){aimPosition.x + 6.0f, aimPosition.y}, crosshair);
    DrawLineV((Vector2){aimPosition.x, aimPosition.y - 6.0f},
              (Vector2){aimPosition.x, aimPosition.y + 6.0f}, crosshair);
}
