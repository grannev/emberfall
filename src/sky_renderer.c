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

/* Far and near. The far layer is the weather on the horizon's side of the
   sky: small, faint, high, and barely moving against the camera. The near one
   is the cloud the player flies through. The radii are bounded by the
   texture: a cloud reaches 2.6 radii sideways and 1.65 up from its centre, and
   both must stay inside SKY_CLOUD_TEXTURE_* blocks of SKY_CLOUD_BLOCK cells. */
static const SkyCloudLayer LAYERS[SKY_CLOUD_LAYERS] = {
    {
        .spacing = 200.0f,
        .drift = 2.5f,
        .parallax = 0.55f,
        .radius = 15.0f,
        .bandLow = 0.20f,
        .bandHigh = 1.00f,
        .alpha = 0.42f,
    },
    {
        .spacing = 170.0f,
        .drift = 5.0f,
        .parallax = 0.78f,
        .radius = 23.0f,
        .bandLow = 0.50f,
        .bandHigh = 1.90f,
        .alpha = 0.62f,
    },
};

/* A cloud's offset inside its slot never reaches the next slot, so the slot
   order is the left-to-right order and the range of slots that can reach a
   view is exact. */
#define SKY_SLOT_OFFSET_FRACTION 0.6f
#define SKY_CLOUD_HALF_WIDTH \
    ((float)(SKY_CLOUD_TEXTURE_WIDTH * SKY_CLOUD_BLOCK) * 0.5f)
#define SKY_CLOUD_HALF_HEIGHT \
    ((float)(SKY_CLOUD_TEXTURE_HEIGHT * SKY_CLOUD_BLOCK) * 0.5f)

void SkyRendererInit(SkyRenderer *sky, uint64_t seed)
{
    int index;

    if (sky == NULL) {
        return;
    }
    memset(sky, 0, sizeof(*sky));
    sky->seed = seed;
    for (index = 0; index < SKY_CLOUD_CACHE; ++index) {
        sky->clouds[index].layer = -1;
    }
}

void SkyRendererSyncSeed(SkyRenderer *sky, uint64_t seed)
{
    int index;

    if (sky == NULL || sky->seed == seed) {
        return;
    }
    sky->seed = seed;
    /* Every cached shape belonged to the old sky. */
    for (index = 0; index < sky->cloudCapacity; ++index) {
        sky->clouds[index].bound = false;
    }
}

bool SkyRendererLoad(SkyRenderer *sky)
{
    Image blank;
    int index;

    if (sky == NULL) {
        return false;
    }
    SkyRendererUnload(sky);
    blank = GenImageColor(SKY_CLOUD_TEXTURE_WIDTH, SKY_CLOUD_TEXTURE_HEIGHT, BLANK);
    for (index = 0; index < SKY_CLOUD_CACHE; ++index) {
        SkyCloudTexture *entry = &sky->clouds[index];

        entry->texture = LoadTextureFromImage(blank);
        if (entry->texture.id == 0u) {
            break;
        }
        /* Point filtered: a block is a block at every zoom. */
        SetTextureFilter(entry->texture, TEXTURE_FILTER_POINT);
        SetTextureWrap(entry->texture, TEXTURE_WRAP_CLAMP);
        entry->bound = false;
        entry->layer = -1;
    }
    UnloadImage(blank);
    sky->cloudCapacity = index;
    return sky->cloudCapacity > 0;
}

void SkyRendererUnload(SkyRenderer *sky)
{
    int index;

    if (sky == NULL) {
        return;
    }
    for (index = 0; index < sky->cloudCapacity; ++index) {
        if (sky->clouds[index].texture.id != 0u) {
            UnloadTexture(sky->clouds[index].texture);
        }
        sky->clouds[index] = (SkyCloudTexture){0};
        sky->clouds[index].layer = -1;
    }
    sky->cloudCapacity = 0;
}

const SkyRendererStats *SkyRendererStatistics(const SkyRenderer *sky)
{
    static const SkyRendererStats empty = {0};

    return sky != NULL ? &sky->stats : &empty;
}

const SkyCloudLayer *SkyRendererLayer(int layer)
{
    if (layer < 0 || layer >= SKY_CLOUD_LAYERS) {
        return NULL;
    }
    return &LAYERS[layer];
}

/* How far the layer's field is shifted for this view: a fraction of the
   camera's position, so the layer scrolls slower than the ground. */
static float SkyLayerShift(const SkyCloudLayer *layer, Rectangle visible)
{
    return (visible.x + visible.width * 0.5f) * (1.0f - layer->parallax);
}

/* A cloud's body radius, from the seed alone. */
static float SkyCloudRadius(uint64_t seed, const SkyCloudLayer *layer,
                            int layerIndex, int slot)
{
    return layer->radius * (0.6f + 0.4f * SkyUnit(seed, slot, layerIndex, 11));
}

/* Where a slot's cloud is. Everything about a cloud's place is derived here so
   the scene and emissive passes cannot disagree about it; its shape is
   derived in SkyCloudPuffLocal, relative to this point, so it is the same
   shape wherever the cloud has drifted to. */
static void SkyCloudAt(uint64_t seed, const SkyCloudLayer *layer, int layerIndex,
                       int slot, int worldHeight, float time, Rectangle visible,
                       float *x, float *y)
{
    float band = (float)worldHeight * (WORLD_CLOUD_LINE - WORLD_SPACE_LINE);

    *x = (float)slot * layer->spacing +
         SkyUnit(seed, slot, layerIndex, 5) * layer->spacing *
             SKY_SLOT_OFFSET_FRACTION +
         time * layer->drift + SkyLayerShift(layer, visible);
    /* Measured down from the space line in units of the band between it and
       the cloud line, and never quite touching the space line: a cloud sitting
       on it would read as the ceiling of the world. The near layer reaches
       well below the cloud line — that line is where weight fades, and weather
       has no reason to stop there. */
    *y = (float)worldHeight * WORLD_SPACE_LINE +
         band * (layer->bandLow +
                 SkyUnit(seed, slot, layerIndex, 7 + layerIndex * 100) *
                     (layer->bandHigh - layer->bandLow));
}

/* One puff relative to the cloud's centre. Puffs are strung along the cloud,
   and the ones in the middle sit higher and grow larger: a cumulus is a dome
   on a flat base, not a row of balls. */
static void SkyCloudPuffLocal(uint64_t seed, int layerIndex, int slot,
                              float cloudRadius, int puff, Vector2 *offset,
                              float *radius)
{
    float along = ((float)puff / (float)(SKY_CLOUD_PUFFS - 1)) * 2.0f - 1.0f;
    float dome = 1.0f - along * along;
    int salt = puff + layerIndex * 32;

    offset->x = along * cloudRadius * 1.3f +
                (SkyUnit(seed, slot, salt, 17) - 0.5f) * cloudRadius * 0.3f;
    offset->y = (SkyUnit(seed, slot, salt, 19) - 0.5f) * cloudRadius * 0.5f -
                dome * cloudRadius * 0.25f;
    *radius = cloudRadius * (0.55f + 0.45f * dome) *
              (0.85f + 0.3f * SkyUnit(seed, slot, salt, 23));
}

void SkyRendererCloudPuff(const SkyRenderer *sky, int layer, int slot,
                          int worldHeight, float time, Rectangle visible,
                          int puff, Vector2 *centre, float *radius)
{
    const SkyCloudLayer *shape;
    Vector2 offset;
    float cloudX;
    float cloudY;

    if (sky == NULL || centre == NULL || radius == NULL || layer < 0 ||
        layer >= SKY_CLOUD_LAYERS) {
        return;
    }
    if (puff < 0) puff = 0;
    if (puff >= SKY_CLOUD_PUFFS) puff = SKY_CLOUD_PUFFS - 1;
    shape = &LAYERS[layer];
    SkyCloudAt(sky->seed, shape, layer, slot, worldHeight, time, visible, &cloudX,
               &cloudY);
    SkyCloudPuffLocal(sky->seed, layer, slot,
                      SkyCloudRadius(sky->seed, shape, layer, slot), puff,
                      &offset, radius);
    centre->x = floorf(cloudX) + offset.x;
    centre->y = floorf(cloudY) + offset.y;
}

Rectangle SkyRendererCloudBounds(const SkyRenderer *sky, int layer, int slot,
                                 int worldHeight, float time, Rectangle visible)
{
    float cloudX;
    float cloudY;

    if (sky == NULL || layer < 0 || layer >= SKY_CLOUD_LAYERS) {
        return (Rectangle){0.0f, 0.0f, 0.0f, 0.0f};
    }
    SkyCloudAt(sky->seed, &LAYERS[layer], layer, slot, worldHeight, time, visible,
               &cloudX, &cloudY);
    /* Whole cells, like everything else that moves through the world. */
    return (Rectangle){floorf(cloudX) - SKY_CLOUD_HALF_WIDTH,
                       floorf(cloudY) - SKY_CLOUD_HALF_HEIGHT,
                       SKY_CLOUD_HALF_WIDTH * 2.0f, SKY_CLOUD_HALF_HEIGHT * 2.0f};
}

void SkyRendererVisibleSlots(const SkyRenderer *sky, int layer, Rectangle visible,
                             float time, int *firstSlot, int *lastSlot)
{
    const SkyCloudLayer *shape;
    float reach;
    float shift;
    float lowest;
    float highest;

    if (firstSlot == NULL || lastSlot == NULL) {
        return;
    }
    *firstSlot = 0;
    *lastSlot = -1;
    if (sky == NULL || layer < 0 || layer >= SKY_CLOUD_LAYERS) {
        return;
    }
    shape = &LAYERS[layer];
    reach = SKY_CLOUD_HALF_WIDTH + 2.0f;
    shift = time * shape->drift + SkyLayerShift(shape, visible);
    /* The slot's own coordinate is its drawn position with the drift and the
       parallax taken back off; an extra slot below covers the offset a cloud
       sits at inside its slot. */
    lowest = visible.x - reach - shift;
    highest = visible.x + visible.width + reach - shift;
    *firstSlot = (int)floorf(lowest / shape->spacing) - 1;
    *lastSlot = (int)floorf(highest / shape->spacing);
}

void SkyRendererBuildCloud(const SkyRenderer *sky, int layer, int slot,
                           Color *texels)
{
    const SkyCloudLayer *shape;
    Vector2 offsets[SKY_CLOUD_PUFFS];
    float radii[SKY_CLOUD_PUFFS];
    float weights[SKY_CLOUD_PUFFS];
    float cloudRadius;
    int puff;
    int row;

    if (sky == NULL || texels == NULL || layer < 0 || layer >= SKY_CLOUD_LAYERS) {
        return;
    }
    shape = &LAYERS[layer];
    cloudRadius = SkyCloudRadius(sky->seed, shape, layer, slot);
    for (puff = 0; puff < SKY_CLOUD_PUFFS; ++puff) {
        SkyCloudPuffLocal(sky->seed, layer, slot, cloudRadius, puff, &offsets[puff],
                          &radii[puff]);
        weights[puff] = 0.7f + 0.3f * SkyUnit(sky->seed, slot, puff + layer * 32, 29);
    }

    for (row = 0; row < SKY_CLOUD_TEXTURE_HEIGHT; ++row) {
        float y = ((float)row + 0.5f) * (float)SKY_CLOUD_BLOCK - SKY_CLOUD_HALF_HEIGHT;
        int column;

        for (column = 0; column < SKY_CLOUD_TEXTURE_WIDTH; ++column) {
            float x = ((float)column + 0.5f) * (float)SKY_CLOUD_BLOCK -
                      SKY_CLOUD_HALF_WIDTH;
            float density = 0.0f;
            float lit;
            float dither;
            int level;
            Color *texel = &texels[row * SKY_CLOUD_TEXTURE_WIDTH + column];

            for (puff = 0; puff < SKY_CLOUD_PUFFS; ++puff) {
                float dx = (x - offsets[puff].x) / radii[puff];
                float dy = (y - offsets[puff].y) / radii[puff];
                float distance = dx * dx + dy * dy;

                if (distance < 1.0f) {
                    /* A soft bell: full in the middle, gone at the rim, and
                       flat at both ends so neither shows as a ring. */
                    float fall = 1.0f - distance;

                    density += weights[puff] * fall * fall;
                }
            }
            if (density > 1.0f) density = 1.0f;
            /* Quantised to a few levels, and dithered along the edge of each
               level by the block's own hash, which is what turns a smooth
               gradient into a ragged pixel edge. */
            dither = SkyUnit(sky->seed, column + slot * 131, row + layer * 61, 37);
            level = (int)(density * (float)SKY_CLOUD_LEVELS + dither);
            if (level > SKY_CLOUD_LEVELS) level = SKY_CLOUD_LEVELS;
            if (density <= 0.0f) level = 0;
            /* The top of the cloud catches the light and the underside does
               not, which is most of what makes a soft blob read as a cloud. */
            lit = -y / (cloudRadius > 0.0f ? cloudRadius : 1.0f);
            if (lit < 0.0f) lit = 0.0f;
            if (lit > 1.0f) lit = 1.0f;
            texel->r = (unsigned char)(188.0f + 58.0f * lit);
            texel->g = (unsigned char)(200.0f + 50.0f * lit);
            texel->b = (unsigned char)(226.0f + 29.0f * lit);
            texel->a = (unsigned char)(255.0f * shape->alpha * (float)level /
                                       (float)SKY_CLOUD_LEVELS);
        }
    }
}

/* The cached texture for a cloud, rasterising it if it is not resident. A
   slot claimed this frame is never evicted; otherwise the least recently seen
   one goes. */
static SkyCloudTexture *SkyAcquireCloud(SkyRenderer *sky, int layer, int slot)
{
    SkyCloudTexture *oldest = NULL;
    int index;

    for (index = 0; index < sky->cloudCapacity; ++index) {
        SkyCloudTexture *entry = &sky->clouds[index];

        if (entry->bound && entry->layer == layer && entry->slot == slot) {
            entry->lastUsedFrame = sky->frame;
            return entry;
        }
        if (entry->bound && entry->lastUsedFrame == sky->frame) {
            continue;
        }
        if (oldest == NULL || !entry->bound ||
            (oldest->bound && entry->lastUsedFrame < oldest->lastUsedFrame)) {
            oldest = entry;
        }
    }
    if (oldest == NULL) {
        return NULL;
    }
    {
        Color texels[SKY_CLOUD_TEXTURE_WIDTH * SKY_CLOUD_TEXTURE_HEIGHT];

        SkyRendererBuildCloud(sky, layer, slot, texels);
        UpdateTexture(oldest->texture, texels);
    }
    oldest->bound = true;
    oldest->layer = layer;
    oldest->slot = slot;
    oldest->lastUsedFrame = sky->frame;
    ++sky->stats.cloudsBuilt;
    return oldest;
}

static void SkyDrawClouds(SkyRenderer *sky, Rectangle visible, int worldHeight,
                          float daylight, float time, bool occluder)
{
    /* Bright enough to survive the air veil the world draws over everything
       above ground: a cloud behind half an atmosphere of dark blue loses most
       of its contrast, and one that reads as storm-grey at noon reads as
       nothing at dusk. */
    unsigned char level = (unsigned char)(255.0f * (0.24f + 0.76f * daylight));
    Color tint = occluder ? BLACK : (Color){level, level, level, 255};
    int layer;

    if (sky->cloudCapacity <= 0) {
        return;
    }
    for (layer = 0; layer < SKY_CLOUD_LAYERS; ++layer) {
        int firstSlot;
        int lastSlot;
        int slot;

        SkyRendererVisibleSlots(sky, layer, visible, time, &firstSlot, &lastSlot);
        for (slot = firstSlot; slot <= lastSlot; ++slot) {
            Rectangle bounds = SkyRendererCloudBounds(sky, layer, slot, worldHeight,
                                                      time, visible);
            SkyCloudTexture *cloud;

            /* Culled against what the cloud actually covers, never against
               the slot it came from: the two are not the same shape. */
            if (bounds.x + bounds.width < visible.x ||
                bounds.x > visible.x + visible.width ||
                bounds.y + bounds.height < visible.y ||
                bounds.y > visible.y + visible.height) {
                continue;
            }
            cloud = SkyAcquireCloud(sky, layer, slot);
            if (cloud == NULL) {
                continue;
            }
            DrawTexturePro(cloud->texture,
                           (Rectangle){0.0f, 0.0f, (float)SKY_CLOUD_TEXTURE_WIDTH,
                                       (float)SKY_CLOUD_TEXTURE_HEIGHT},
                           bounds, (Vector2){0.0f, 0.0f}, 0.0f, tint);
            if (!occluder) {
                ++sky->stats.cloudsDrawn;
            }
        }
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
            /* Nothing at the cloud line, everything at the space line — but the
               darkening starts some way up the band. Spreading it across the
               whole band put a veil over the entire daytime sky and turned every
               backdrop into a silhouette against black: the sky between the
               clouds and space is still sky, and only the top of it is not.
             *
               Where it starts, and how fast it closes, are a fraction of the
               band rather than a distance, so they hold whatever the world's
               height is. They were tuned when that band was a hundred and
               eighty cells deep, and on a taller world the same fractions left
               the ground's painted horizon showing at seventy per cent through
               air the player had already climbed above — hills at eye level,
               seen from orbit. Beginning sooner and closing faster is what
               makes the top of the climb read as space rather than as a dim
               afternoon. */
            float height = (cloudY - y) / (cloudY - spaceY);
            float amount = (height - 0.20f) / 0.80f;
            unsigned char alpha;

            if (amount < 0.0f) amount = 0.0f;
            if (amount > 1.0f) amount = 1.0f;
            alpha = (unsigned char)(250.0f * amount * amount * sqrtf(amount));
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
                if (!emissive) {
                    ++sky->stats.starsDrawn;
                }
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
    ++sky->frame;
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
