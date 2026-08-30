#include "audio.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <raymath.h>

typedef enum SynthKind {
    SYNTH_LASER = 0,
    SYNTH_EXPLOSION,
    SYNTH_REACTION,
    SYNTH_DRILL
} SynthKind;

static float SynthNoise(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return ((float)(*state & 0xffffu) / 32767.5f) - 1.0f;
}

static float SynthSample(SynthKind kind, float time, float duration, uint32_t *noiseState)
{
    float attack = Clamp(time / 0.008f, 0.0f, 1.0f);
    float release = Clamp((duration - time) / 0.025f, 0.0f, 1.0f);
    float envelope = attack * release;

    if (kind == SYNTH_LASER) {
        float wobble = sinf(time * 2.0f * PI * 8.0f) * 18.0f;
        float tone = sinf(time * 2.0f * PI * (205.0f + wobble));
        float harmonic = sinf(time * 2.0f * PI * 414.0f) * 0.33f;
        return (tone + harmonic) * envelope * 0.42f;
    }

    if (kind == SYNTH_EXPLOSION) {
        float decay = expf(-6.3f * time);
        float rumbleFrequency = 86.0f - 48.0f * (time / duration);
        float rumble = sinf(time * 2.0f * PI * rumbleFrequency) * 0.58f;
        float noise = SynthNoise(noiseState) * 0.72f;
        return (rumble + noise) * decay * release * 0.72f;
    }

    if (kind == SYNTH_DRILL) {
        float grind = sinf(time * 2.0f * PI * 61.0f) * 0.52f;
        float rasp = sinf(time * 2.0f * PI * 147.0f) * 0.26f;
        float grit = SynthNoise(noiseState) * 0.5f;
        return (grind + rasp + grit) * envelope * 0.5f;
    }

    {
        float decay = expf(-4.5f * time);
        float hiss = SynthNoise(noiseState);
        float fizz = sinf(time * 2.0f * PI * 920.0f) * 0.12f;
        return (hiss * 0.32f + fizz) * decay * envelope;
    }
}

static Sound SynthCreateSound(SynthKind kind, float duration)
{
    const unsigned int sampleRate = 22050u;
    unsigned int frameCount = (unsigned int)((float)sampleRate * duration);
    int16_t *samples = malloc((size_t)frameCount * sizeof(*samples));
    Sound sound = {0};
    Wave wave;
    uint32_t noiseState = 0x6d2b79f5u + (uint32_t)kind * 0x9e3779b9u;
    unsigned int frame;

    if (samples == NULL) {
        return sound;
    }

    for (frame = 0; frame < frameCount; ++frame) {
        float time = (float)frame / (float)sampleRate;
        float sample = Clamp(SynthSample(kind, time, duration, &noiseState), -1.0f, 1.0f);
        samples[frame] = (int16_t)(sample * 32767.0f);
    }

    wave = (Wave){frameCount, sampleRate, 16u, 1u, samples};
    sound = LoadSoundFromWave(wave);
    free(samples);
    return sound;
}

bool GameAudioInit(GameAudio *audio)
{
    if (audio == NULL) {
        return false;
    }

    memset(audio, 0, sizeof(*audio));
    InitAudioDevice();
    audio->ready = IsAudioDeviceReady();
    if (!audio->ready) {
        return false;
    }

    audio->laser = SynthCreateSound(SYNTH_LASER, 0.14f);
    audio->explosion = SynthCreateSound(SYNTH_EXPLOSION, 0.58f);
    audio->reaction = SynthCreateSound(SYNTH_REACTION, 0.34f);
    audio->drill = SynthCreateSound(SYNTH_DRILL, 0.18f);
    SetMasterVolume(0.72f);

    if (IsSoundValid(audio->laser)) SetSoundVolume(audio->laser, 0.34f);
    if (IsSoundValid(audio->explosion)) SetSoundVolume(audio->explosion, 0.72f);
    if (IsSoundValid(audio->reaction)) SetSoundVolume(audio->reaction, 0.28f);
    if (IsSoundValid(audio->drill)) SetSoundVolume(audio->drill, 0.3f);
    return true;
}

/* Laser and drill are held states: retrigger the short wave while the state
   lasts and stop it the moment it ends, instead of stacking one-shots. */
static void GameAudioHold(Sound sound, bool active)
{
    if (!IsSoundValid(sound)) {
        return;
    }

    if (active) {
        if (!IsSoundPlaying(sound)) {
            PlaySound(sound);
        }
    } else if (IsSoundPlaying(sound)) {
        StopSound(sound);
    }
}

void GameAudioUpdate(GameAudio *audio, bool laserActive, bool drilling,
                     float deltaTime)
{
    if (audio == NULL) {
        return;
    }

    audio->reactionCooldown = fmaxf(0.0f, audio->reactionCooldown - deltaTime);
    if (!audio->ready) {
        return;
    }

    GameAudioHold(audio->laser, laserActive);
    GameAudioHold(audio->drill, drilling);
}

void GameAudioPlayExplosion(GameAudio *audio)
{
    if (audio != NULL && audio->ready && IsSoundValid(audio->explosion)) {
        PlaySound(audio->explosion);
    }
}

void GameAudioPlayReaction(GameAudio *audio)
{
    if (audio == NULL || !audio->ready || audio->reactionCooldown > 0.0f ||
        !IsSoundValid(audio->reaction)) {
        return;
    }

    PlaySound(audio->reaction);
    audio->reactionCooldown = 0.13f;
}

void GameAudioUnload(GameAudio *audio)
{
    if (audio == NULL) {
        return;
    }

    if (audio->ready) {
        if (IsSoundValid(audio->laser)) UnloadSound(audio->laser);
        if (IsSoundValid(audio->explosion)) UnloadSound(audio->explosion);
        if (IsSoundValid(audio->reaction)) UnloadSound(audio->reaction);
        if (IsSoundValid(audio->drill)) UnloadSound(audio->drill);
        CloseAudioDevice();
    }
    memset(audio, 0, sizeof(*audio));
}
