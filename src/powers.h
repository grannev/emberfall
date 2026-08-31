#ifndef POWERS_H
#define POWERS_H

#include <stdbool.h>

#include <raylib.h>

#include "particles.h"
#include "rng.h"
#include "world.h"

typedef enum PowerKind {
    POWER_LASER = 0,
    POWER_EXPLOSION,
    POWER_FORCE,
    POWER_CHILL
} PowerKind;

typedef struct PowerSystem {
    PowerKind current;
    Rng rng;
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
    /* Force blast: one heavy blow, not a stream. A held gust reads as weak
       however strong it is, because nothing about it has a moment of impact. */
    bool forceTriggered;
    Vector2 forceDirection;
    Vector2 forceOrigin;
    float forceCooldown;
    float forceCooldownMax;
    float forceTime;
    float forceDuration;
    /* Gameplay tuning is state, not duplicated literals: PowersUpdate uses the
       cone values, presentation reads the same geometry, and Game publishes
       the recoil when forceTriggered is observed. */
    float forceLength;
    float forceSpreadCosine;
    int forceReach;
    float forceRecoil;
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

void PowersInit(PowerSystem *powers, uint64_t seed);
void PowersUpdate(PowerSystem *powers, World *world, ParticleSystem *particles,
                  Vector2 origin, Vector2 aimPosition, float deltaTime,
                  bool laserHeld, bool explosionPressed, bool forcePressed,
                  bool chillHeld);
const char *PowersCurrentName(const PowerSystem *powers);

#endif
