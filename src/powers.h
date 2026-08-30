#ifndef POWERS_H
#define POWERS_H

#include <stdbool.h>

#include <raylib.h>

#include "particles.h"
#include "world.h"

typedef enum PowerKind {
    POWER_LASER = 0,
    POWER_EXPLOSION
} PowerKind;

typedef struct PowerSystem {
    PowerKind current;
    float explosionCooldown;
    float explosionCooldownMax;
    bool laserActive;
    bool laserHit;
    Vector2 laserStart;
    Vector2 laserEnd;
    CellMaterial laserHitMaterial;
    bool explosionTriggered;
    Vector2 explosionPosition;
    float explosionShockRadius;
    float shockwaveTime;
    float shockwaveDuration;
} PowerSystem;

void PowersInit(PowerSystem *powers);
void PowersUpdate(PowerSystem *powers, World *world, ParticleSystem *particles,
                  Vector2 origin, Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed);
void PowersDrawWorld(const PowerSystem *powers, Vector2 aimPosition);
const char *PowersCurrentName(const PowerSystem *powers);

#endif
