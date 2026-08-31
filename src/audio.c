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
    SYNTH_DRILL,
    SYNTH_IMPACT,
    SYNTH_FORCE,
    SYNTH_CHILL
} SynthKind;

static float SynthNoise(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return ((float)(*state & 0xffffu) / 32767.5f) - 1.0f;
}

/* One-pole low pass. Raw white noise reads as static; rolling the top off turns
   it into air moving, which is what the force cone needs to sound like. */
static float SynthLowPass(float input, float *state, float amount)
{
    *state += (input - *state) * amount;
    return *state;
}

static float SynthSample(SynthKind kind, float time, float duration,
                         uint32_t *noiseState, float *filterState)
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

    if (kind == SYNTH_IMPACT) {
        /* A knock, not a boom: the pitch drops away fast so it reads as the
           player hitting something rather than as a small explosion. */
        float decay = expf(-26.0f * time);
        float thud = sinf(time * 2.0f * PI * (134.0f - 78.0f * (time / duration)));
        float knock = SynthNoise(noiseState) * 0.38f;
        return (thud * 0.8f + knock) * decay * 0.9f;
    }

    if (kind == SYNTH_FORCE) {
        float breath = SynthLowPass(SynthNoise(noiseState), filterState, 0.22f);
        float sweep = sinf(time * 2.0f * PI * (58.0f + 26.0f * sinf(time * 9.0f)));

        return (breath * 2.4f + sweep * 0.22f) * envelope * 0.5f;
    }

    if (kind == SYNTH_CHILL) {
        /* Bright and thin, the opposite end of the spectrum from the laser, so
           the two beams are told apart by ear alone. */
        float hiss = SynthNoise(noiseState) - SynthLowPass(SynthNoise(noiseState),
                                                           filterState, 0.5f);
        float shimmer = sinf(time * 2.0f * PI * (1480.0f +
                                                 40.0f * sinf(time * 21.0f)));
        return (hiss * 0.5f + shimmer * 0.16f) * envelope * 0.44f;
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
    float filterState = 0.0f;
    unsigned int frame;

    if (samples == NULL) {
        return sound;
    }

    for (frame = 0; frame < frameCount; ++frame) {
        float time = (float)frame / (float)sampleRate;
        float sample = Clamp(
            SynthSample(kind, time, duration, &noiseState, &filterState), -1.0f,
            1.0f);
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
    audio->impact = SynthCreateSound(SYNTH_IMPACT, 0.16f);
    audio->force = SynthCreateSound(SYNTH_FORCE, 0.24f);
    audio->chill = SynthCreateSound(SYNTH_CHILL, 0.2f);
    SetMasterVolume(0.72f);

    if (IsSoundValid(audio->laser)) SetSoundVolume(audio->laser, 0.34f);
    if (IsSoundValid(audio->explosion)) SetSoundVolume(audio->explosion, 0.72f);
    if (IsSoundValid(audio->reaction)) SetSoundVolume(audio->reaction, 0.28f);
    if (IsSoundValid(audio->drill)) SetSoundVolume(audio->drill, 0.3f);
    if (IsSoundValid(audio->impact)) SetSoundVolume(audio->impact, 0.5f);
    if (IsSoundValid(audio->force)) SetSoundVolume(audio->force, 0.26f);
    if (IsSoundValid(audio->chill)) SetSoundVolume(audio->chill, 0.24f);
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

/* Pitch of the drill by what it is cutting. Rock grinds low and slow, loose
   material scrapes higher, ice is brittle and bright. */
static float DrillPitchFor(CellMaterial material)
{
    switch (material) {
    case MATERIAL_ROCK:
        return 0.78f;
    case MATERIAL_SAND:
        return 1.24f;
    case MATERIAL_ICE:
        return 1.55f;
    default:
        return 1.0f;
    }
}

void GameAudioUpdate(GameAudio *audio, GameAudioState state, float deltaTime)
{
    if (audio == NULL) {
        return;
    }

    audio->reactionCooldown = fmaxf(0.0f, audio->reactionCooldown - deltaTime);
    audio->impactCooldown = fmaxf(0.0f, audio->impactCooldown - deltaTime);
    if (!audio->ready) {
        return;
    }

    if (state.drilling && IsSoundValid(audio->drill)) {
        SetSoundPitch(audio->drill, DrillPitchFor(state.drillMaterial));
    }
    GameAudioHold(audio->laser, state.laser);
    GameAudioHold(audio->drill, state.drilling);
    GameAudioHold(audio->force, state.force);
    GameAudioHold(audio->chill, state.chill);
}

void GameAudioPlayImpact(GameAudio *audio, float strength)
{
    if (audio == NULL || !audio->ready || audio->impactCooldown > 0.0f ||
        strength < 14.0f || !IsSoundValid(audio->impact)) {
        return;
    }

    /* A harder hit is louder and lower. The cooldown keeps a player scraping
       along a wall from firing the knock every single frame. */
    SetSoundVolume(audio->impact, Clamp(0.2f + strength / 190.0f, 0.2f, 0.72f));
    SetSoundPitch(audio->impact, Clamp(1.24f - strength / 340.0f, 0.72f, 1.24f));
    PlaySound(audio->impact);
    audio->impactCooldown = 0.09f;
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
        if (IsSoundValid(audio->impact)) UnloadSound(audio->impact);
        if (IsSoundValid(audio->force)) UnloadSound(audio->force);
        if (IsSoundValid(audio->chill)) UnloadSound(audio->chill);
        CloseAudioDevice();
    }
    memset(audio, 0, sizeof(*audio));
}
