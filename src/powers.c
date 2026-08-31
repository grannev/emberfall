#include "powers.h"

#include <math.h>
#include <stddef.h>

#include <raymath.h>

static Vector2 LaserEndAtWorldEdge(const World *world, Vector2 origin, Vector2 direction,
                                   float maxLength)
{
    float length = maxLength;

    if (direction.x > 0.001f) {
        length = fminf(length, ((float)world->width - 0.5f - origin.x) / direction.x);
    } else if (direction.x < -0.001f) {
        length = fminf(length, (0.5f - origin.x) / direction.x);
    }
    if (direction.y > 0.001f) {
        length = fminf(length, ((float)world->height - 0.5f - origin.y) / direction.y);
    } else if (direction.y < -0.001f) {
        length = fminf(length, (0.5f - origin.y) / direction.y);
    }

    length = fmaxf(length, 0.0f);
    return (Vector2){origin.x + direction.x * length,
                     origin.y + direction.y * length};
}

void PowersInit(PowerSystem *powers)
{
    if (powers == NULL) {
        return;
    }

    powers->current = POWER_LASER;
    powers->explosionCooldown = 0.0f;
    powers->explosionCooldownMax = 0.7f;
    powers->laserActive = false;
    powers->laserHit = false;
    powers->origin = (Vector2){0.0f, 0.0f};
    powers->laserStart = (Vector2){0.0f, 0.0f};
    powers->laserEnd = (Vector2){0.0f, 0.0f};
    powers->laserHitMaterial = MATERIAL_EMPTY;
    powers->forceTriggered = false;
    powers->forceDirection = (Vector2){1.0f, 0.0f};
    powers->forceOrigin = (Vector2){0.0f, 0.0f};
    powers->forceCooldown = 0.0f;
    powers->forceCooldownMax = 0.42f;
    powers->forceTime = 0.0f;
    powers->forceDuration = 0.26f;
    powers->forceLength = 84.0f;
    powers->forceSpreadCosine = 0.78f;
    powers->forceReach = 54;
    powers->forceRecoil = 132.0f;
    powers->chillActive = false;
    powers->chillHit = false;
    powers->chillEnd = (Vector2){0.0f, 0.0f};
    powers->explosionTriggered = false;
    powers->explosionPosition = (Vector2){0.0f, 0.0f};
    powers->explosionShockRadius = 42.0f;
    powers->shockwaveTime = 0.0f;
    powers->shockwaveDuration = 0.32f;
}

void PowersUpdate(PowerSystem *powers, World *world, ParticleSystem *particles,
                  Vector2 origin, Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed, bool forcePressed,
                  bool chillHeld)
{
    Vector2 direction = {aimPosition.x - origin.x, aimPosition.y - origin.y};
    float directionLength = sqrtf(direction.x * direction.x + direction.y * direction.y);

    if (powers == NULL || world == NULL) {
        return;
    }

    powers->origin = origin;
    powers->explosionCooldown = fmaxf(0.0f, powers->explosionCooldown - deltaTime);
    powers->shockwaveTime = fmaxf(0.0f, powers->shockwaveTime - deltaTime);
    powers->laserActive = false;
    powers->laserHit = false;
    powers->forceTriggered = false;
    powers->forceCooldown = fmaxf(0.0f, powers->forceCooldown - deltaTime);
    powers->forceTime = fmaxf(0.0f, powers->forceTime - deltaTime);
    powers->chillActive = false;
    powers->chillHit = false;
    powers->explosionTriggered = false;

    if (directionLength > 0.001f) {
        direction.x /= directionLength;
        direction.y /= directionLength;
    } else {
        direction = (Vector2){1.0f, 0.0f};
    }

    if (laserHeld) {
        LaserResult result;
        Vector2 maximumEnd;

        powers->current = POWER_LASER;
        powers->laserActive = true;
        powers->laserStart = origin;
        maximumEnd = LaserEndAtWorldEdge(world, origin, direction, 280.0f);
        result = WorldApplyLaser(world, powers->laserStart, maximumEnd, 2.25f, deltaTime);
        powers->laserEnd = result.position;
        powers->laserHit = result.hit;
        powers->laserHitMaterial = result.material;

        if (result.hit && GetRandomValue(0, 1) == 0) {
            ParticlesSpawnLaserSparks(particles, result.position, direction);
        }
    }

    if (chillHeld) {
        LaserResult result;
        Vector2 maximumEnd;

        powers->current = POWER_CHILL;
        powers->chillActive = true;
        maximumEnd = LaserEndAtWorldEdge(world, origin, direction, 190.0f);
        result = WorldApplyChill(world, origin, maximumEnd, 2.6f, deltaTime);
        powers->chillEnd = result.position;
        powers->chillHit = result.hit;
    }

    if (forcePressed) {
        powers->current = POWER_FORCE;
        if (powers->forceCooldown <= 0.0f) {
            WorldApplyForceBlast(world, origin, direction, powers->forceLength,
                                 powers->forceSpreadCosine, powers->forceReach);
            ParticlesSpawnForceBlast(particles, origin, direction);
            powers->forceTriggered = true;
            powers->forceDirection = direction;
            powers->forceOrigin = origin;
            powers->forceCooldown = powers->forceCooldownMax;
            powers->forceTime = powers->forceDuration;
        }
    }

    if (explosionPressed) {
        powers->current = POWER_EXPLOSION;
        if (powers->explosionCooldown <= 0.0f) {
            WorldDestroyCircle(world, (int)aimPosition.x, (int)aimPosition.y, 17, 0.38f);
            WorldApplyShockwave(world, (int)aimPosition.x, (int)aimPosition.y, 17,
                                (int)powers->explosionShockRadius);
            ParticlesSpawnExplosion(particles, aimPosition);
            powers->explosionCooldown = powers->explosionCooldownMax;
            powers->explosionTriggered = true;
            powers->explosionPosition = aimPosition;
            powers->shockwaveTime = powers->shockwaveDuration;
        }
    }
}

void PowersDrawWorld(const PowerSystem *powers, Vector2 aimPosition)
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

const char *PowersCurrentName(const PowerSystem *powers)
{
    if (powers == NULL) {
        return "UNKNOWN";
    }
    switch (powers->current) {
    case POWER_EXPLOSION:
        return "EXPLOSION (RMB)";
    case POWER_FORCE:
        return "FORCE (Q)";
    case POWER_CHILL:
        return "CRYO (E)";
    default:
        return "LASER (LMB)";
    }
}
