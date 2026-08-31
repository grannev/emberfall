#ifndef RNG_H
#define RNG_H

/* The deterministic random source for everything that affects gameplay.
 *
 * raylib's GetRandomValue draws from one process-wide generator shared with
 * anything else that happens to call it, which makes a world unreproducible:
 * the same seed gives a different map depending on how many times the frame
 * loop, a particle burst or a camera shake happened to draw first. Gameplay
 * therefore owns its own state, passed explicitly, so that the same seed and
 * the same inputs give the same world and the same simulation. That is what
 * makes a bug report, a regression test and a benchmark scenario repeatable.
 *
 * Presentation-only randomness — camera shake, the jitter on a spark — may
 * still use raylib's generator: it cannot change what the simulation does.
 */

#include <stdbool.h>
#include <stdint.h>

/* SplitMix64. Ten lines, no weak seeds (every 64-bit value including zero is a
   valid state), and easily good enough for terrain and debris. It is also the
   standard way to expand one user-facing seed into the several independent
   streams below. */
typedef struct Rng {
    uint64_t state;
} Rng;

static inline void RngSeed(Rng *rng, uint64_t seed)
{
    rng->state = seed;
}

static inline uint64_t RngNext(Rng *rng)
{
    uint64_t value = (rng->state += 0x9e3779b97f4a7c15ull);

    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

/* Derives an independent stream from a seed and a label, so two systems that
   share a world seed never walk the same sequence. */
static inline uint64_t RngStreamSeed(uint64_t seed, uint64_t stream)
{
    Rng mixer;

    RngSeed(&mixer, seed ^ (stream * 0xd1342543de82ef95ull));
    return RngNext(&mixer);
}

/* Inclusive on both ends, matching the raylib call this replaces. The modulo
   leaves a bias of at most one part in 2^64 divided by the span, which for the
   ranges used here is far below anything observable. */
static inline int RngRange(Rng *rng, int minimum, int maximum)
{
    uint64_t span;

    if (maximum < minimum) {
        int swap = minimum;

        minimum = maximum;
        maximum = swap;
    }
    span = (uint64_t)((int64_t)maximum - (int64_t)minimum) + 1ull;
    return (int)((int64_t)minimum + (int64_t)(RngNext(rng) % span));
}

static inline float RngFloat(Rng *rng, float minimum, float maximum)
{
    /* 24 bits is the whole mantissa of a float; taking more would be thrown
       away by the conversion. */
    float unit = (float)(RngNext(rng) >> 40) / (float)(1u << 24);

    return minimum + unit * (maximum - minimum);
}

/* True with probability numerator/denominator. */
static inline bool RngChance(Rng *rng, int numerator, int denominator)
{
    if (numerator <= 0 || denominator <= 0) {
        return false;
    }
    return RngRange(rng, 0, denominator - 1) < numerator;
}

#endif
