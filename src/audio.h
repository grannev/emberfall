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
    float reactionCooldown;
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
void GameAudioUnload(GameAudio *audio);

#endif
