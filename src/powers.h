#ifndef POWERS_H
#define POWERS_H

#include <stdbool.h>

#include <raylib.h>

#include "particles.h"
#include "world.h"

typedef enum PowerKind {
    POWER_LASER = 0,
    POWER_EXPLOSION,
    POWER_FORCE,
    POWER_CHILL
} PowerKind;

typedef struct PowerSystem {
    PowerKind current;
    float explosionCooldown;
    float explosionCooldownMax;
    bool laserActive;
    bool laserHit;
    /* Where every beam and cone starts. Recorded every update, not only when a
       power fires, so the force cone and cryo beam do not have to borrow the
       laser's stale origin. */
    Vector2 origin;
    Vector2 laserStart;
    Vector2 laserEnd;
    CellMaterial laserHitMaterial;
    /* Force cone: shoves loose material without destroying it. Applied on a
       fixed cadence rather than every frame, so its strength does not depend on
       the frame rate. */
    bool forceActive;
    Vector2 forceDirection;
    float forceCadence;
    float forcePulse;
    /* Cryo beam: the thermal inverse of the laser. */
    bool chillActive;
    bool chillHit;
    Vector2 chillEnd;
    bool explosionTriggered;
    Vector2 explosionPosition;
    float explosionShockRadius;
    float shockwaveTime;
    float shockwaveDuration;
} PowerSystem;

void PowersInit(PowerSystem *powers);
void PowersUpdate(PowerSystem *powers, World *world, ParticleSystem *particles,
                  Vector2 origin, Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed, bool forceHeld,
                  bool chillHeld);
void PowersDrawWorld(const PowerSystem *powers, Vector2 aimPosition);
const char *PowersCurrentName(const PowerSystem *powers);

#endif
