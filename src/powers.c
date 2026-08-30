#include "powers.h"

#include <math.h>
#include <stddef.h>

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
    powers->laserStart = (Vector2){0.0f, 0.0f};
    powers->laserEnd = (Vector2){0.0f, 0.0f};
}

void PowersUpdate(PowerSystem *powers, World *world, ParticleSystem *particles,
                  Vector2 origin, Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed)
{
    Vector2 direction = {aimPosition.x - origin.x, aimPosition.y - origin.y};
    float directionLength = sqrtf(direction.x * direction.x + direction.y * direction.y);

    if (powers == NULL || world == NULL) {
        return;
    }

    powers->explosionCooldown = fmaxf(0.0f, powers->explosionCooldown - deltaTime);
    powers->laserActive = false;

    if (directionLength > 0.001f) {
        direction.x /= directionLength;
        direction.y /= directionLength;
    } else {
        direction = (Vector2){1.0f, 0.0f};
    }

    if (laserHeld) {
        powers->current = POWER_LASER;
        powers->laserActive = true;
        powers->laserStart = origin;
        powers->laserEnd = LaserEndAtWorldEdge(world, origin, direction, 280.0f);
        WorldApplyLaser(world, powers->laserStart, powers->laserEnd, 2.25f, deltaTime);

        if (GetRandomValue(0, 2) == 0) {
            float sparkDistance = fminf(directionLength, 115.0f);
            Vector2 sparkPosition = {origin.x + direction.x * sparkDistance,
                                     origin.y + direction.y * sparkDistance};
            ParticlesSpawnLaserSparks(particles, sparkPosition, direction);
        }
    }

    if (explosionPressed) {
        powers->current = POWER_EXPLOSION;
        if (powers->explosionCooldown <= 0.0f) {
            WorldDestroyCircle(world, (int)aimPosition.x, (int)aimPosition.y, 17, 0.38f);
            ParticlesSpawnExplosion(particles, aimPosition);
            powers->explosionCooldown = powers->explosionCooldownMax;
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
        DrawCircleV(powers->laserEnd, 2.2f, (Color){255, 165, 53, 220});
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
    return powers->current == POWER_EXPLOSION ? "EXPLOSION (RMB)" : "LASER (LMB)";
}
