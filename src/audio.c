#include "audio.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <raymath.h>

typedef enum SynthKind {
    SYNTH_LASER = 0,
    SYNTH_LASER_IMPACT,
    SYNTH_EXPLOSION_ATTACK,
    SYNTH_EXPLOSION_BODY,
    SYNTH_EXPLOSION_TAIL,
    SYNTH_REACTION,
    SYNTH_DRILL,
    SYNTH_IMPACT,
    SYNTH_FORCE,
    SYNTH_CHILL,
    SYNTH_CHILL_IMPACT,
    SYNTH_BOOST,
    SYNTH_SONIC,
    SYNTH_SPLASH,
    SYNTH_REENTRY
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

    if (kind == SYNTH_LASER_IMPACT) {
        float decay = expf(-31.0f * time);
        float snap = sinf(time * 2.0f * PI * (1260.0f - 520.0f * time / duration));
        float spit = SynthNoise(noiseState) * 0.42f;

        return (snap * 0.72f + spit) * decay * release;
    }

    if (kind == SYNTH_EXPLOSION_ATTACK) {
        float decay = expf(-38.0f * time);
        float crack = SynthNoise(noiseState) * 0.82f;
        float snap = sinf(time * 2.0f * PI *
                          (620.0f - 380.0f * time / duration));

        return (crack + snap * 0.55f) * decay * release * 0.82f;
    }

    if (kind == SYNTH_EXPLOSION_BODY) {
        float decay = expf(-6.3f * time);
        float rumbleFrequency = 86.0f - 48.0f * (time / duration);
        float rumble = sinf(time * 2.0f * PI * rumbleFrequency) * 0.58f;
        float noise = SynthNoise(noiseState) * 0.72f;
        return (rumble + noise) * decay * release * 0.72f;
    }

    if (kind == SYNTH_EXPLOSION_TAIL) {
        float decay = expf(-3.8f * time);
        float wash = SynthLowPass(SynthNoise(noiseState), filterState, 0.10f);
        float distant = sinf(time * 2.0f * PI *
                             (52.0f - 20.0f * time / duration));

        return (wash * 2.1f + distant * 0.48f) * decay * release * 0.56f;
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
        /* One blow, not a stream: a hard front edge and a fast decay, so the
           sound has the moment of impact the old held gust never had. */
        float decay = expf(-9.0f * time);
        float breath = SynthLowPass(SynthNoise(noiseState), filterState, 0.3f);
        float thump = sinf(time * 2.0f * PI * (72.0f - 34.0f * (time / duration)));

        return (breath * 2.6f + thump * 0.75f) * decay * release * 0.72f;
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

    if (kind == SYNTH_CHILL_IMPACT) {
        float decay = expf(-24.0f * time);
        float crack = SynthNoise(noiseState) -
                      SynthLowPass(SynthNoise(noiseState), filterState, 0.62f);
        float chime = sinf(time * 2.0f * PI *
                           (1880.0f + 340.0f * time / duration));

        return (crack * 0.42f + chime * 0.34f) * decay * release;
    }

    if (kind == SYNTH_BOOST) {
        /* One source pitched lower and louder at each stage. A falling thump is
           the body of the kick; filtered noise is the air being left behind. */
        float decay = expf(-6.5f * time);
        float rush = SynthLowPass(SynthNoise(noiseState), filterState, 0.22f);
        float thump = sinf(time * 2.0f * PI *
                           (96.0f - 58.0f * (time / duration)));
        float crack = sinf(time * 2.0f * PI * 410.0f) * expf(-28.0f * time);

        return (thump * 0.72f + rush * 1.8f + crack * 0.28f) * decay * release;
    }

    if (kind == SYNTH_SONIC) {
        float snap = SynthNoise(noiseState) * expf(-43.0f * time);
        float body = sinf(time * 2.0f * PI *
                          (168.0f - 103.0f * time / duration)) * expf(-11.0f * time);
        float rush = SynthLowPass(SynthNoise(noiseState), filterState, 0.14f) *
                     expf(-6.0f * time);

        return (snap * 0.8f + body * 0.74f + rush * 1.5f) * release * 0.7f;
    }

    if (kind == SYNTH_REENTRY) {
        /* Not a bang: a wall of air. Broad noise under a low tone that
           wavers, held flat rather than struck, so a burn that lasts seconds
           is one continuous roar and not sixty impacts. */
        float roar = SynthLowPass(SynthNoise(noiseState), filterState, 0.09f);
        float body = sinf(time * 2.0f * PI * (58.0f + 9.0f * sinf(time * 13.0f)));
        float swell = time < 0.06f ? time / 0.06f : 1.0f;

        return (roar * 2.6f + body * 0.5f) * swell * release * 0.6f;
    }

    if (kind == SYNTH_SPLASH) {
        /* A slap and a wash: a short low thump for the surface giving way,
           then filtered noise falling away as the spray comes down. */
        float slap = sinf(time * 2.0f * PI * (180.0f - 110.0f * time / duration)) *
                     expf(-22.0f * time);
        float wash = SynthLowPass(SynthNoise(noiseState), filterState, 0.18f) *
                     expf(-5.5f * time);

        return (slap * 0.7f + wash * 2.4f) * release * 0.7f;
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

    audio->randomState = 0x86e6be11u;
    audio->explosionBodyDelay = -1.0f;
    audio->explosionTailDelay = -1.0f;
    audio->explosionStrength = 1.0f;
    audio->laser = SynthCreateSound(SYNTH_LASER, 0.14f);
    audio->laserImpact = SynthCreateSound(SYNTH_LASER_IMPACT, 0.09f);
    audio->explosionAttack = SynthCreateSound(SYNTH_EXPLOSION_ATTACK, 0.12f);
    audio->explosionBody = SynthCreateSound(SYNTH_EXPLOSION_BODY, 0.54f);
    audio->explosionTail = SynthCreateSound(SYNTH_EXPLOSION_TAIL, 0.82f);
    audio->reaction = SynthCreateSound(SYNTH_REACTION, 0.34f);
    audio->drill = SynthCreateSound(SYNTH_DRILL, 0.18f);
    audio->impact = SynthCreateSound(SYNTH_IMPACT, 0.16f);
    audio->force = SynthCreateSound(SYNTH_FORCE, 0.42f);
    audio->chill = SynthCreateSound(SYNTH_CHILL, 0.2f);
    audio->chillImpact = SynthCreateSound(SYNTH_CHILL_IMPACT, 0.12f);
    audio->boost = SynthCreateSound(SYNTH_BOOST, 0.52f);
    audio->sonic = SynthCreateSound(SYNTH_SONIC, 0.36f);
    audio->splash = SynthCreateSound(SYNTH_SPLASH, 0.46f);
    audio->reentry = SynthCreateSound(SYNTH_REENTRY, 0.60f);
    SetMasterVolume(0.72f);

    if (IsSoundValid(audio->laser)) SetSoundVolume(audio->laser, 0.34f);
    if (IsSoundValid(audio->laserImpact)) SetSoundVolume(audio->laserImpact, 0.34f);
    if (IsSoundValid(audio->explosionAttack)) SetSoundVolume(audio->explosionAttack, 0.68f);
    if (IsSoundValid(audio->explosionBody)) SetSoundVolume(audio->explosionBody, 0.70f);
    if (IsSoundValid(audio->explosionTail)) SetSoundVolume(audio->explosionTail, 0.43f);
    if (IsSoundValid(audio->reaction)) SetSoundVolume(audio->reaction, 0.28f);
    if (IsSoundValid(audio->drill)) SetSoundVolume(audio->drill, 0.3f);
    if (IsSoundValid(audio->impact)) SetSoundVolume(audio->impact, 0.5f);
    if (IsSoundValid(audio->force)) SetSoundVolume(audio->force, 0.74f);
    if (IsSoundValid(audio->chill)) SetSoundVolume(audio->chill, 0.24f);
    if (IsSoundValid(audio->chillImpact)) SetSoundVolume(audio->chillImpact, 0.26f);
    if (IsSoundValid(audio->boost)) SetSoundVolume(audio->boost, 0.45f);
    if (IsSoundValid(audio->sonic)) SetSoundVolume(audio->sonic, 0.54f);
    if (IsSoundValid(audio->reentry)) SetSoundVolume(audio->reentry, 0.4f);
    if (IsSoundValid(audio->splash)) SetSoundVolume(audio->splash, 0.5f);
    return true;
}

static uint32_t GameAudioRandomU32(GameAudio *audio)
{
    uint32_t value = audio->randomState;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    audio->randomState = value;
    return value;
}

static float GameAudioRandomRange(GameAudio *audio, float minimum,
                                  float maximum)
{
    float unit = (float)(GameAudioRandomU32(audio) & 0xffffu) / 65535.0f;

    return minimum + (maximum - minimum) * unit;
}

static void GameAudioPlayPendingExplosionLayer(GameAudio *audio,
                                               Sound sound,
                                               float baseVolume)
{
    if (!audio->ready || !IsSoundValid(sound)) {
        return;
    }
    SetSoundPitch(sound, GameAudioRandomRange(audio, 0.94f, 1.05f) /
                             sqrtf(audio->explosionStrength));
    SetSoundVolume(sound, baseVolume *
                              (0.72f + audio->explosionStrength * 0.28f) *
                              GameAudioRandomRange(audio, 0.92f, 1.06f));
    PlaySound(sound);
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
    case MATERIAL_METAL:
        /* Plating shrieks. */
        return 1.8f;
    case MATERIAL_RELIC:
        return 0.7f;
    default:
        return 1.0f;
    }
}

void GameAudioUpdate(GameAudio *audio, GameAudioState state, float deltaTime)
{
    /* Placeholders: windStrength, rainIntensity, thunder, leavesRustle,
       sandBlowing and fauna have no sounds yet. The whole sound set is to be replaced
       with recorded sounds and music; until then the weather is silent. */
    (void)state.windStrength;
    (void)state.rainIntensity;
    (void)state.thunder;
    (void)state.leavesRustle;
    (void)state.sandBlowing;
    (void)state.fauna;

    if (audio == NULL) {
        return;
    }

    audio->reactionCooldown = fmaxf(0.0f, audio->reactionCooldown - deltaTime);
    audio->splashCooldown = fmaxf(0.0f, audio->splashCooldown - deltaTime);
    audio->reentryCooldown = fmaxf(0.0f, audio->reentryCooldown - deltaTime);
    audio->impactCooldown = fmaxf(0.0f, audio->impactCooldown - deltaTime);
    audio->laserImpactCooldown = fmaxf(
        0.0f, audio->laserImpactCooldown - deltaTime);
    audio->chillImpactCooldown = fmaxf(
        0.0f, audio->chillImpactCooldown - deltaTime);
    if (!audio->ready) {
        return;
    }

    if (audio->explosionBodyDelay >= 0.0f) {
        audio->explosionBodyDelay -= deltaTime;
        if (audio->explosionBodyDelay <= 0.0f) {
            GameAudioPlayPendingExplosionLayer(audio, audio->explosionBody,
                                               0.70f);
            audio->explosionBodyDelay = -1.0f;
        }
    }
    if (audio->explosionTailDelay >= 0.0f) {
        audio->explosionTailDelay -= deltaTime;
        if (audio->explosionTailDelay <= 0.0f) {
            GameAudioPlayPendingExplosionLayer(audio, audio->explosionTail,
                                               0.43f);
            audio->explosionTailDelay = -1.0f;
        }
    }

    if (state.drilling && IsSoundValid(audio->drill)) {
        SetSoundPitch(audio->drill, DrillPitchFor(state.drillMaterial));
    }
    GameAudioHold(audio->laser, state.laser);
    GameAudioHold(audio->drill, state.drilling);
    GameAudioHold(audio->chill, state.chill);
}

void GameAudioPlayForce(GameAudio *audio)
{
    if (audio != NULL && audio->ready && IsSoundValid(audio->force)) {
        SetSoundPitch(audio->force, GameAudioRandomRange(audio, 0.95f, 1.04f));
        PlaySound(audio->force);
    }
}

void GameAudioPlayBoost(GameAudio *audio)
{
    if (audio == NULL || !audio->ready || !IsSoundValid(audio->boost)) {
        return;
    }

    SetSoundPitch(audio->boost, 0.94f * GameAudioRandomRange(audio, 0.97f, 1.03f));
    SetSoundVolume(audio->boost, 0.62f);
    PlaySound(audio->boost);
}

void GameAudioPlaySonic(GameAudio *audio)
{
    if (audio == NULL || !audio->ready || !IsSoundValid(audio->sonic)) return;
    SetSoundPitch(audio->sonic, GameAudioRandomRange(audio, 0.96f, 1.04f));
    PlaySound(audio->sonic);
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

void GameAudioPlayExplosion(GameAudio *audio, float strength)
{
    if (audio == NULL || !audio->ready) {
        return;
    }
    audio->explosionStrength = Clamp(strength / 145.0f, 0.60f, 1.40f);
    if (IsSoundValid(audio->explosionAttack)) {
        SetSoundPitch(audio->explosionAttack,
                      GameAudioRandomRange(audio, 0.94f, 1.08f) /
                          sqrtf(audio->explosionStrength));
        SetSoundVolume(audio->explosionAttack,
                       0.68f * (0.74f + audio->explosionStrength * 0.26f));
        PlaySound(audio->explosionAttack);
    }
    audio->explosionBodyDelay = 0.025f;
    audio->explosionTailDelay = 0.17f;
}

void GameAudioPlayLaserImpact(GameAudio *audio, float strength)
{
    if (audio == NULL || !audio->ready || audio->laserImpactCooldown > 0.0f ||
        !IsSoundValid(audio->laserImpact)) {
        return;
    }
    SetSoundPitch(audio->laserImpact,
                  GameAudioRandomRange(audio, 0.92f, 1.16f));
    SetSoundVolume(audio->laserImpact,
                   Clamp(0.20f + strength * 0.12f, 0.20f, 0.38f));
    PlaySound(audio->laserImpact);
    audio->laserImpactCooldown = 0.065f;
}

void GameAudioPlayChillImpact(GameAudio *audio)
{
    if (audio == NULL || !audio->ready || audio->chillImpactCooldown > 0.0f ||
        !IsSoundValid(audio->chillImpact)) {
        return;
    }
    SetSoundPitch(audio->chillImpact,
                  GameAudioRandomRange(audio, 0.96f, 1.12f));
    PlaySound(audio->chillImpact);
    audio->chillImpactCooldown = 0.095f;
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

void GameAudioPlayReentry(GameAudio *audio, float heat)
{
    float weight;

    if (audio == NULL || !audio->ready || audio->reentryCooldown > 0.0f ||
        !IsSoundValid(audio->reentry)) {
        return;
    }
    weight = Clamp(heat, 0.0f, 1.0f);
    SetSoundPitch(audio->reentry, 0.82f + 0.3f * weight);
    SetSoundVolume(audio->reentry, 0.12f + 0.4f * weight);
    PlaySound(audio->reentry);
    /* Just under the sound's own length, so the roar overlaps into itself
       and reads as continuous rather than as a stutter. */
    audio->reentryCooldown = 0.5f;
}

void GameAudioPlaySplash(GameAudio *audio, float strength)
{
    float weight;

    if (audio == NULL || !audio->ready || audio->splashCooldown > 0.0f ||
        !IsSoundValid(audio->splash)) {
        return;
    }
    weight = Clamp(strength / 300.0f, 0.0f, 1.0f);
    SetSoundPitch(audio->splash, 1.25f - 0.45f * weight);
    SetSoundVolume(audio->splash, 0.28f + 0.42f * weight);
    PlaySound(audio->splash);
    audio->splashCooldown = 0.11f;
}

void GameAudioUnload(GameAudio *audio)
{
    if (audio == NULL) {
        return;
    }

    if (audio->ready) {
        if (IsSoundValid(audio->laser)) UnloadSound(audio->laser);
        if (IsSoundValid(audio->laserImpact)) UnloadSound(audio->laserImpact);
        if (IsSoundValid(audio->explosionAttack)) UnloadSound(audio->explosionAttack);
        if (IsSoundValid(audio->explosionBody)) UnloadSound(audio->explosionBody);
        if (IsSoundValid(audio->explosionTail)) UnloadSound(audio->explosionTail);
        if (IsSoundValid(audio->reaction)) UnloadSound(audio->reaction);
        if (IsSoundValid(audio->drill)) UnloadSound(audio->drill);
        if (IsSoundValid(audio->impact)) UnloadSound(audio->impact);
        if (IsSoundValid(audio->force)) UnloadSound(audio->force);
        if (IsSoundValid(audio->chill)) UnloadSound(audio->chill);
        if (IsSoundValid(audio->chillImpact)) UnloadSound(audio->chillImpact);
        if (IsSoundValid(audio->splash)) UnloadSound(audio->splash);
        if (IsSoundValid(audio->reentry)) UnloadSound(audio->reentry);
        if (IsSoundValid(audio->sonic)) UnloadSound(audio->sonic);
        if (IsSoundValid(audio->boost)) UnloadSound(audio->boost);
        CloseAudioDevice();
    }
    memset(audio, 0, sizeof(*audio));
}
