#ifndef GAME_AUDIO_H
#define GAME_AUDIO_H

#include <stdbool.h>

#include <raylib.h>

typedef struct GameAudio {
    Sound laser;
    Sound explosion;
    Sound reaction;
    float reactionCooldown;
    bool ready;
} GameAudio;

bool GameAudioInit(GameAudio *audio);
void GameAudioUpdate(GameAudio *audio, bool laserActive, float deltaTime);
void GameAudioPlayExplosion(GameAudio *audio);
void GameAudioPlayReaction(GameAudio *audio);
void GameAudioUnload(GameAudio *audio);

#endif
