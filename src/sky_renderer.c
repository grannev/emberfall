/* Clouds and space. See sky_renderer.h. */
#include "sky_renderer.h"

#include <math.h>
#include <string.h>

#include "beam_render.h"
#include "world.h"

/* One hash for everything the sky is made of. Same mixer as the beams use, so
   the whole presentation layer draws its randomness from one place. */
static float SkyUnit(uint64_t seed, int a, int b, int salt)
{
    unsigned int mixed = (unsigned int)seed ^
                         (unsigned int)(seed >> 32) * 2654435761u;

    return BeamNoise((int)((unsigned int)a * 2654435761u + mixed), b, salt);
}

void SkyRendererInit(SkyRenderer *sky, uint64_t seed)
{
    if (sky == NULL) {
        return;
    }
    memset(sky, 0, sizeof(*sky));
    sky->seed = seed;
}

void SkyRendererSyncSeed(SkyRenderer *sky, uint64_t seed)
{
    if (sky == NULL) {
        return;
    }
    sky->seed = seed;
}

const SkyRendererStats *SkyRendererStatistics(const SkyRenderer *sky)
{
    static const SkyRendererStats empty = {0};

    return sky != NULL ? &sky->stats : &empty;
}

/* One lump of a cloud: a squashed disc of blocks with its edge eaten away, and
   a brighter rank along the top where the light lands. */
static void SkyCloudLump(uint64_t seed, float centreX, float centreY,
                         float radiusX, float radiusY, int salt, Color body,
                         Color lit, float block)
{
    float y;

    for (y = -radiusY; y <= radiusY; y += block) {
        float x;

        for (x = -radiusX; x <= radiusX; x += block) {
            float unitX = x / radiusX;
            float unitY = y / radiusY;
            float distance = unitX * unitX + unitY * unitY;
            bool top;

            if (distance > 1.0f) continue;
            /* A ragged edge rather than an ellipse: the outer third is eaten
               away by the hash, which is what stops a cloud reading as a
               drawn shape. */
            if (distance > 0.42f &&
                SkyUnit(seed, (int)(x / block), (int)(y / block), salt) <
                    (distance - 0.42f) * 1.5f) {
                continue;
            }
            top = y < -radiusY * 0.45f;
            BeamBlock(centreX + x, centreY + y, block, top ? lit : body);
        }
    }
}

/* Where a cloud slot's cloud is and how big it is. Everything about a cloud is
   derived here so the scene and emissive passes cannot disagree about it. */
static void SkyCloudAt(uint64_t seed, int slot, int worldHeight, float time,
                       float *x, float *y, float *radiusX, float *radiusY)
{
    float band = (float)worldHeight * (WORLD_CLOUD_LINE - WORLD_SPACE_LINE);
    float drift = 4.0f + SkyUnit(seed, slot, 0, 3) * 9.0f;
    float offset = SkyUnit(seed, slot, 0, 5) * (float)SKY_CLOUD_SPACING;

    *x = (float)slot * (float)SKY_CLOUD_SPACING + offset + time * drift;
    /* Kept inside the band, and never quite touching its edges: a cloud sitting
       on the space line would read as the ceiling of the world. */
    *y = (float)worldHeight * WORLD_SPACE_LINE + band * 0.2f +
         SkyUnit(seed, slot, 0, 7) * band * 0.62f;
    *radiusX = 30.0f + SkyUnit(seed, slot, 0, 11) * 52.0f;
    *radiusY = *radiusX * (0.24f + SkyUnit(seed, slot, 0, 13) * 0.16f);
}

static void SkyDrawClouds(SkyRenderer *sky, Rectangle visible, int worldHeight,
                          float daylight, float time, bool emissive)
{
    /* The drift carries a cloud out of its own slot, so the range is widened by
       the widest a cloud can be plus the furthest it can have drifted. */
    int margin = SKY_CLOUD_SPACING * 2;
    int firstSlot = (int)floorf((visible.x - (float)margin) /
                                (float)SKY_CLOUD_SPACING);
    int lastSlot = (int)floorf((visible.x + visible.width + (float)margin) /
                               (float)SKY_CLOUD_SPACING);
    float block = emissive ? 2.0f : 1.0f;
    int slot;

    for (slot = firstSlot; slot <= lastSlot; ++slot) {
        float x;
        float y;
        float radiusX;
        float radiusY;
        Color body;
        Color lit;
        int lump;
        int lumps;

        SkyCloudAt(sky->seed, slot, worldHeight, time, &x, &y, &radiusX,
                   &radiusY);
        if (x + radiusX * 2.0f < visible.x ||
            x - radiusX * 2.0f > visible.x + visible.width) {
            continue;
        }
        if (y + radiusY < visible.y ||
            y - radiusY > visible.y + visible.height) {
            continue;
        }

        if (emissive) {
            /* Only the lit rank contributes to the glow: a whole cloud in the
               emissive pass turns into a white smear once the bloom has been
               over it. */
            unsigned char level = (unsigned char)(40.0f + 130.0f * daylight);

            body = (Color){0, 0, 0, 0};
            lit = (Color){level, level, (unsigned char)(level + 20u), 255};
        } else {
            float level = 0.24f + 0.76f * daylight;

            /* Bright enough to survive the air veil the world draws over
               everything above ground: a cloud behind half an atmosphere of
               dark blue loses most of its contrast, and one that reads as
               storm-grey at noon reads as nothing at dusk. */
            body = (Color){(unsigned char)(196.0f * level),
                           (unsigned char)(206.0f * level),
                           (unsigned char)(228.0f * level), 250};
            lit = (Color){(unsigned char)(246.0f * level),
                          (unsigned char)(250.0f * level),
                          (unsigned char)(255.0f * level), 252};
        }

        /* Three overlapping lumps, so a cloud has a silhouette rather than an
           outline. */
        lumps = 3;
        for (lump = 0; lump < lumps; ++lump) {
            float spread = ((float)lump / (float)(lumps - 1)) - 0.5f;
            float lumpX = x + spread * radiusX * 1.15f;
            float lumpY = y + (SkyUnit(sky->seed, slot, lump, 17) - 0.5f) *
                                  radiusY * 0.7f;
            float scale = 0.62f + SkyUnit(sky->seed, slot, lump, 19) * 0.5f;

            SkyCloudLump(sky->seed, lumpX, lumpY, radiusX * 0.62f * scale,
                         radiusY * scale, slot * 7 + lump, body, lit, block);
        }
        ++sky->stats.cloudsDrawn;
    }
}

/* The dark above, and the stars in it. Drawn as a veil over the backdrop rather
   than as a replacement for it, so the horizon still shows through at the
   bottom of the band and the transition is a climb rather than a cut. */
static void SkyDrawSpace(SkyRenderer *sky, Rectangle visible, int worldHeight,
                         float daylight, bool emissive)
{
    float spaceY = (float)worldHeight * WORLD_SPACE_LINE;
    float cloudY = (float)worldHeight * WORLD_CLOUD_LINE;
    float top = visible.y;
    float bottom = visible.y + visible.height;
    float step = 2.0f;
    float y;
    int column;

    if (top >= cloudY) {
        return;
    }
    sky->stats.spaceVisible = true;

    if (!emissive) {
        /* Banded rather than a gradient fill: the world is squares, and two
           cells of solid colour at a time is what everything else in the
           picture is made of. */
        for (y = top; y < cloudY && y < bottom; y += step) {
            float amount = 1.0f - (y - spaceY) / (cloudY - spaceY);
            unsigned char alpha;

            if (amount < 0.0f) amount = 0.0f;
            if (amount > 1.0f) amount = 1.0f;
            alpha = (unsigned char)(248.0f * amount * amount);
            DrawRectangleV((Vector2){visible.x, y},
                           (Vector2){visible.width, step},
                           (Color){2, 3, 8, alpha});
        }
    }

    /* Stars on a lattice, one per cell of it, so they are spread rather than
       clustered. The lattice is fine because the band is thin: the whole of
       space is a sixth of the world's height, and a coarse grid put four stars
       in it.

       They are not clipped at the world's top edge. The camera can see above it
       and there is no reason for space to stop where the cell array does — a
       hard line of stars ending in nothing is worse than no stars at all. */
    {
        const float lattice = 10.0f;
        float starTop = top - lattice;
        float starBottom = cloudY < bottom ? cloudY : bottom;
        int row;

        for (column = (int)floorf(visible.x / lattice) - 1;
             column <= (int)floorf((visible.x + visible.width) / lattice) + 1;
             ++column) {
            for (row = (int)floorf(starTop / lattice);
                 row <= (int)floorf(starBottom / lattice) + 1; ++row) {
                float starX = (float)column * lattice +
                              SkyUnit(sky->seed, column, row, 23) * lattice;
                float starY = (float)row * lattice +
                              SkyUnit(sky->seed, column, row, 29) * lattice;
                float depth = 1.0f - (starY - spaceY) / (cloudY - spaceY);
                float brightness;
                unsigned char level;

                if (starY >= cloudY) continue;
                if (depth < 0.0f) depth = 0.0f;
                if (depth > 1.0f) depth = 1.0f;
                if (SkyUnit(sky->seed, column, row, 31) < 0.62f) continue;
                /* Depth decides most of it and daylight only the rest. Washing
                   stars out by day is right at ground level and wrong here:
                   above the air there is nothing left to scatter the light, and
                   a black sky with no stars in it is not space, it is a black
                   rectangle. */
                brightness = depth * (0.52f + 0.48f * (1.0f - daylight));
                if (brightness < 0.05f) continue;
                level = (unsigned char)(255.0f * brightness);
                BeamBlock(starX, starY, emissive ? 2.0f : 1.0f,
                          (Color){level, level,
                                  (unsigned char)(level > 235u ? 255u
                                                              : level + 20u),
                                  255});
                ++sky->stats.starsDrawn;
            }
        }
    }
}

void SkyRendererDraw(SkyRenderer *sky, Rectangle visible, int worldHeight,
                     float daylight, float time)
{
    if (sky == NULL || worldHeight <= 0 || visible.width <= 0.0f ||
        visible.height <= 0.0f) {
        return;
    }
    sky->stats = (SkyRendererStats){0};
    SkyDrawSpace(sky, visible, worldHeight, daylight, false);
    SkyDrawClouds(sky, visible, worldHeight, daylight, time, false);
}

void SkyRendererDrawEmissive(SkyRenderer *sky, Rectangle visible,
                             int worldHeight, float daylight, float time)
{
    if (sky == NULL || worldHeight <= 0 || visible.width <= 0.0f ||
        visible.height <= 0.0f) {
        return;
    }
    SkyDrawSpace(sky, visible, worldHeight, daylight, true);
    SkyDrawClouds(sky, visible, worldHeight, daylight, time, true);
}
