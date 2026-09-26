#ifndef GAME_AUDIO_H
#define GAME_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include <raylib.h>

#include "world.h"

typedef struct GameAudio {
    Sound laser;
    Sound laserImpact;
    Sound explosionAttack;
    Sound explosionBody;
    Sound explosionTail;
    Sound reaction;
    Sound drill;
    Sound impact;
    Sound force;
    Sound chill;
    Sound chillImpact;
    Sound boost;
    Sound sonic;
    Sound splash;
    Sound reentry;
    float reactionCooldown;
    float splashCooldown;
    float reentryCooldown;
    float impactCooldown;
    float laserImpactCooldown;
    float chillImpactCooldown;
    float explosionBodyDelay;
    float explosionTailDelay;
    float explosionStrength;
    uint32_t randomState;
    bool ready;
} GameAudio;

/* What is sounding this frame. A struct rather than a growing argument list:
   every held state has to be passed every frame, and six positional booleans at
   a call site say nothing about which is which. */
typedef struct GameAudioState {
    bool laser;
    bool drilling;
    /* What the drill is currently chewing, so rock does not sound like dirt. */
    CellMaterial drillMaterial;
    bool chill;
    /* The weather and the living world, for the sound rework to come. Kept
       here so the game already says what it would play — wind by strength,
       rain, a thunderclap, leaves, sand — while the current sound set has
       nothing to play for it. GameAudioUpdate records it and plays nothing. */
    float windStrength;
    float rainIntensity;
    bool thunder;
    bool leavesRustle;
    bool sandBlowing;
} GameAudioState;

bool GameAudioInit(GameAudio *audio);
void GameAudioUpdate(GameAudio *audio, GameAudioState state, float deltaTime);
void GameAudioPlayExplosion(GameAudio *audio, float strength);
void GameAudioPlayLaserImpact(GameAudio *audio, float strength);
void GameAudioPlayChillImpact(GameAudio *audio);
void GameAudioPlayReaction(GameAudio *audio);
void GameAudioPlayImpact(GameAudio *audio, float strength);
void GameAudioPlayForce(GameAudio *audio);
void GameAudioPlayBoost(GameAudio *audio);
void GameAudioPlaySonic(GameAudio *audio);
/* A surface broken: louder and lower the harder the hit, `strength` being the
   speed of what hit it. */
void GameAudioPlaySplash(GameAudio *audio, float strength);
/* The roar of the air on something burning through it. `heat` is 0..1. */
void GameAudioPlayReentry(GameAudio *audio, float heat);
void GameAudioUnload(GameAudio *audio);

#endif
