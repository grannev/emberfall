#include "ability_renderer.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

void AbilityRendererDraw(const PowerSystem *powers, Vector2 aimPosition)
{
    Color crosshair = powers != NULL && powers->explosionCooldown <= 0.0f
                          ? (Color){255, 232, 118, 230}
                          : (Color){180, 188, 199, 190};

    if (powers == NULL) {
        return;
    }

    if (powers->laserActive) {
        DrawLineEx(powers->laserStart, powers->laserEnd, 1.6f,
                   (Color){255, 81, 43, 210});
        DrawLineEx(powers->laserStart, powers->laserEnd, 0.55f,
                   (Color){255, 244, 188, 255});
        if (powers->laserHit) {
            DrawCircleV(powers->laserEnd, 4.2f, (Color){255, 74, 24, 70});
            DrawCircleV(powers->laserEnd, 2.5f, (Color){255, 161, 43, 190});
            DrawCircleV(powers->laserEnd, 1.1f, (Color){255, 248, 203, 255});
        } else {
            DrawCircleV(powers->laserEnd, 0.9f, (Color){255, 198, 88, 180});
        }
    }

    if (powers->chillActive) {
        DrawLineEx(powers->origin, powers->chillEnd, 1.4f,
                   (Color){126, 214, 255, 150});
        DrawLineEx(powers->origin, powers->chillEnd, 0.5f,
                   (Color){232, 250, 255, 220});
        DrawCircleV(powers->chillEnd, powers->chillHit ? 3.6f : 1.2f,
                    (Color){206, 244, 255, 190});
    }

    if (powers->forceTime > 0.0f) {
        /* An arc racing outward along the cone, so the blow reads as a single
           moment of impact travelling away from the player. */
        float progress = 1.0f - powers->forceTime / powers->forceDuration;
        float angle = atan2f(powers->forceDirection.y, powers->forceDirection.x) *
                      RAD2DEG;
        float halfAngle = acosf(Clamp(powers->forceSpreadCosine, -1.0f, 1.0f)) *
                          RAD2DEG;
        int ring;

        for (ring = 0; ring < 4; ++ring) {
            float radius = 10.0f + (powers->forceLength - 10.0f) * progress -
                           (float)ring * 6.0f;
            unsigned char alpha;

            if (radius <= 0.0f) {
                continue;
            }
            alpha = (unsigned char)Clamp((1.0f - progress) * 210.0f -
                                             (float)ring * 40.0f,
                                         0.0f, 255.0f);
            DrawCircleSectorLines(powers->forceOrigin, radius, angle - halfAngle,
                                  angle + halfAngle, 20,
                                  (Color){182, 216, 255, alpha});
        }
    }

    if (powers->shockwaveTime > 0.0f) {
        float progress = 1.0f - powers->shockwaveTime / powers->shockwaveDuration;
        float radius = 17.0f + (powers->explosionShockRadius - 17.0f) * progress;
        Color ring = Fade((Color){255, 207, 118, 255}, 1.0f - progress);

        DrawCircleLinesV(powers->explosionPosition, radius, ring);
        DrawCircleLinesV(powers->explosionPosition, radius + 0.8f,
                         Fade(ring, 0.35f));
    }

    DrawCircleLinesV(aimPosition, 4.0f, crosshair);
    DrawLineV((Vector2){aimPosition.x - 6.0f, aimPosition.y},
              (Vector2){aimPosition.x + 6.0f, aimPosition.y}, crosshair);
    DrawLineV((Vector2){aimPosition.x, aimPosition.y - 6.0f},
              (Vector2){aimPosition.x, aimPosition.y + 6.0f}, crosshair);
}
