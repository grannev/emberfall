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

void PowersInit(PowerSystem *powers, uint64_t seed)
{
    if (powers == NULL) {
        return;
    }

    RngSeed(&powers->rng, seed);
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

        if (result.hit && RngRange(&powers->rng, 0, 1) == 0) {
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
