/* The backdrop of open space. See space_renderer.h for what it is for; this
 * file records how its pictures are made and laid out.
 */
#include "space_renderer.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <rlgl.h>

#define SPACE_NEBULA_WIDTH 320
#define SPACE_NEBULA_HEIGHT 180
#define SPACE_STARS_WIDTH 960
#define SPACE_STARS_HEIGHT 540

static uint32_t SpaceHash(uint64_t seed, int x, int y, uint32_t salt)
{
    uint64_t value = seed ^ ((uint64_t)(uint32_t)x * 0x9e3779b185ebca87ull) ^
                     ((uint64_t)(uint32_t)y * 0xc2b2ae3d27d4eb4full) ^
                     ((uint64_t)salt * 0x165667b19e3779f9ull);

    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return (uint32_t)value;
}

static float SpaceUnit(uint64_t seed, int x, int y, uint32_t salt)
{
    return (float)(SpaceHash(seed, x, y, salt) >> 8) / 16777216.0f;
}

/* Value noise on a lattice `scale` texels apart, wrapping at `periodX`
   lattice cells across so the picture tiles round the world. */
static float SpaceNoise(uint64_t seed, float x, float y, float scale, int periodX,
                        uint32_t salt)
{
    float fx = x / scale;
    float fy = y / scale;
    int ix = (int)floorf(fx);
    int iy = (int)floorf(fy);
    float tx = fx - (float)ix;
    float ty = fy - (float)iy;
    int x0 = ((ix % periodX) + periodX) % periodX;
    int x1 = (x0 + 1) % periodX;
    float a = SpaceUnit(seed, x0, iy, salt);
    float b = SpaceUnit(seed, x1, iy, salt);
    float c = SpaceUnit(seed, x0, iy + 1, salt);
    float d = SpaceUnit(seed, x1, iy + 1, salt);

    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

static float SpaceFractal(uint64_t seed, float x, float y, float scale, int width,
                          uint32_t salt)
{
    float total = 0.0f;
    float weight = 0.5f;
    int octave;

    for (octave = 0; octave < 5; ++octave) {
        int period = (int)((float)width / scale + 0.5f);

        if (period < 1) period = 1;
        total += SpaceNoise(seed, x, y, scale, period, salt + (uint32_t)octave) * weight;
        scale *= 0.5f;
        weight *= 0.5f;
    }
    return total;
}

static Color SpaceMix(Color a, Color b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (Color){(unsigned char)((float)a.r + ((float)b.r - (float)a.r) * t),
                   (unsigned char)((float)a.g + ((float)b.g - (float)a.g) * t),
                   (unsigned char)((float)a.b + ((float)b.b - (float)a.b) * t),
                   (unsigned char)((float)a.a + ((float)b.a - (float)a.a) * t)};
}

/* The nebula: two colours of soft cloud over nothing, lit brighter where
   both overlap, with dark lanes through it. The colours are the seed's:
   violet and teal, rust and gold, or blue and rose. */
static Image SpaceBuildNebula(uint64_t seed)
{
    /* Muted, as the backdrops' own far ranges are: a haze of colour, not
       a poster. */
    static const Color PALETTES[3][2] = {
        {{84, 62, 128, 255}, {46, 104, 124, 255}},
        {{118, 66, 60, 255}, {138, 112, 76, 255}},
        {{56, 74, 132, 255}, {120, 70, 108, 255}},
    };
    const Color *palette = PALETTES[SpaceHash(seed, 0, 0, 91) % 3u];
    Image image = GenImageColor(SPACE_NEBULA_WIDTH, SPACE_NEBULA_HEIGHT, BLANK);
    Color *pixels = image.data;
    int y;

    for (y = 0; y < SPACE_NEBULA_HEIGHT; ++y) {
        int x;
        /* Densest across a band through the middle, like a galaxy seen
           edge on, thinning above and below. */
        float band = 1.0f - fabsf((float)y / (float)SPACE_NEBULA_HEIGHT - 0.45f) * 1.9f;

        for (x = 0; x < SPACE_NEBULA_WIDTH; ++x) {
            float first = SpaceFractal(seed, (float)x, (float)y, 90.0f, SPACE_NEBULA_WIDTH, 11);
            float second = SpaceFractal(seed, (float)x, (float)y, 60.0f, SPACE_NEBULA_WIDTH, 23);
            float lanes = SpaceFractal(seed, (float)x, (float)y, 34.0f, SPACE_NEBULA_WIDTH, 37);
            float density = fmaxf(0.0f, first - 0.42f) * 2.2f * fmaxf(0.0f, band);
            float other = fmaxf(0.0f, second - 0.46f) * 2.4f * fmaxf(0.0f, band + 0.2f);
            float dark = fmaxf(0.0f, lanes - 0.55f) * 2.5f;
            Color colour = SpaceMix(palette[0], palette[1], other / (density + other + 0.001f));
            float alpha = fminf(1.0f, (density + other) * (1.0f - dark * 0.8f));

            /* Quantised, so the cloud is made of steps like everything else. */
            alpha = floorf(alpha * 5.0f) / 5.0f;
            colour.a = (unsigned char)(alpha * 110.0f);
            pixels[y * SPACE_NEBULA_WIDTH + x] = colour;
        }
    }
    return image;
}

/* A layer of stars: `count` of them, most a single dim texel, some brighter,
   a few with a cross of light. Their colours are the colours of stars —
   blue-white, white, pale gold, orange, a rare red. */
static Image SpaceBuildStars(uint64_t seed, int count, float brightness, uint32_t salt)
{
    static const Color TINTS[6] = {
        {170, 200, 255, 255}, {235, 240, 255, 255}, {255, 250, 235, 255},
        {255, 225, 170, 255}, {255, 190, 140, 255}, {255, 140, 130, 255},
    };
    Image image = GenImageColor(SPACE_STARS_WIDTH, SPACE_STARS_HEIGHT, BLANK);
    Color *pixels = image.data;
    int index;

    for (index = 0; index < count; ++index) {
        int x = (int)(SpaceHash(seed, index, 1, salt) % SPACE_STARS_WIDTH);
        int y = (int)(SpaceHash(seed, index, 2, salt) % SPACE_STARS_HEIGHT);
        float roll = SpaceUnit(seed, index, 3, salt);
        Color tint = TINTS[SpaceHash(seed, index, 4, salt) % 6u];
        float level = brightness * (0.35f + 0.65f * roll * roll);

        tint.a = (unsigned char)(255.0f * fminf(1.0f, level));
        pixels[y * SPACE_STARS_WIDTH + x] = tint;
        /* The brightest few carry a cross. */
        if (roll > 0.996f) {
            Color arm = tint;
            int reach;

            for (reach = 1; reach <= 2; ++reach) {
                arm.a = (unsigned char)((float)tint.a * (reach == 1 ? 0.55f : 0.22f));
                pixels[y * SPACE_STARS_WIDTH + (x + reach) % SPACE_STARS_WIDTH] = arm;
                pixels[y * SPACE_STARS_WIDTH + (x - reach + SPACE_STARS_WIDTH) % SPACE_STARS_WIDTH] = arm;
                if (y - reach >= 0) pixels[(y - reach) * SPACE_STARS_WIDTH + x] = arm;
                if (y + reach < SPACE_STARS_HEIGHT) pixels[(y + reach) * SPACE_STARS_WIDTH + x] = arm;
            }
        }
    }
    return image;
}

static void SpaceUnloadTextures(SpaceRenderer *space)
{
    if (space->nebula.id != 0u) UnloadTexture(space->nebula);
    if (space->starsFar.id != 0u) UnloadTexture(space->starsFar);
    if (space->starsNear.id != 0u) UnloadTexture(space->starsNear);
    space->nebula = (Texture2D){0};
    space->starsFar = (Texture2D){0};
    space->starsNear = (Texture2D){0};
    space->ready = false;
}

static Texture2D SpaceUpload(Image image)
{
    Texture2D texture = LoadTextureFromImage(image);

    UnloadImage(image);
    if (texture.id != 0u) {
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    }
    return texture;
}

static void SpaceBuild(SpaceRenderer *space, uint64_t seed)
{
    SpaceUnloadTextures(space);
    space->seed = seed;
    space->nebula = SpaceUpload(SpaceBuildNebula(seed));
    space->starsFar = SpaceUpload(SpaceBuildStars(seed, 5200, 0.5f, 1));
    space->starsNear = SpaceUpload(SpaceBuildStars(seed, 700, 0.95f, 2));
    space->ready = space->nebula.id != 0u && space->starsFar.id != 0u &&
                   space->starsNear.id != 0u;
}

bool SpaceRendererInit(SpaceRenderer *space, uint64_t seed)
{
    if (space == NULL) {
        return false;
    }
    memset(space, 0, sizeof(*space));
    SpaceBuild(space, seed);
    return space->ready;
}

void SpaceRendererSyncSeed(SpaceRenderer *space, uint64_t seed)
{
    if (space == NULL || (space->ready && space->seed == seed)) {
        return;
    }
    SpaceBuild(space, seed);
}

void SpaceRendererUnload(SpaceRenderer *space)
{
    if (space == NULL) {
        return;
    }
    SpaceUnloadTextures(space);
}

/* One block of the backdrop: the size of a world cell on screen. */
static float SpaceBlock(int height)
{
    float block = floorf((float)height / 240.0f);

    return block < 1.0f ? 1.0f : block;
}

/* One textured quad whose alpha runs from `alphaTop` at its top edge to
   `alphaBottom` at its bottom: how a layer thins out toward the air. */
static void SpaceQuad(Texture2D texture, Rectangle source, Rectangle target,
                      float alphaTop, float alphaBottom)
{
    float u0 = source.x / (float)texture.width;
    float v0 = source.y / (float)texture.height;
    float u1 = (source.x + source.width) / (float)texture.width;
    float v1 = (source.y + source.height) / (float)texture.height;
    unsigned char top = (unsigned char)(255.0f * fminf(1.0f, fmaxf(0.0f, alphaTop)));
    unsigned char bottom = (unsigned char)(255.0f * fminf(1.0f, fmaxf(0.0f, alphaBottom)));

    if (top == 0u && bottom == 0u) {
        return;
    }
    rlSetTexture(texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(255, 255, 255, top);
    rlTexCoord2f(u0, v0);
    rlVertex2f(target.x, target.y);
    rlColor4ub(255, 255, 255, bottom);
    rlTexCoord2f(u0, v1);
    rlVertex2f(target.x, target.y + target.height);
    rlColor4ub(255, 255, 255, bottom);
    rlTexCoord2f(u1, v1);
    rlVertex2f(target.x + target.width, target.y + target.height);
    rlColor4ub(255, 255, 255, top);
    rlTexCoord2f(u1, v0);
    rlVertex2f(target.x + target.width, target.y);
    rlEnd();
    rlSetTexture(0);
}

/* Visibility of the layer at screen row `y`: `amount` above `fullY`, none
   below `clearY`, a straight fade between. */
static float SpaceMaskAt(float y, float amount, float fullY, float clearY)
{
    if (y <= fullY) return amount;
    if (y >= clearY) return 0.0f;
    return amount * (clearY - y) / (clearY - fullY);
}

/* Tiles `texture` across the target at `scale` screen pixels per texel,
   scrolled by `shiftX`, placed at `top`, cut into horizontal strips so the
   mask can run down it. */
static void SpaceTile(Texture2D texture, float shiftX, float top, float scale, int width,
                      float amount, float fullY, float clearY)
{
    float tileWidth = (float)texture.width * scale;
    float tileHeight = (float)texture.height * scale;
    float start = -fmodf(shiftX, tileWidth);
    float cuts[4];
    int cutCount = 0;
    float x;

    if (start > 0.0f) start -= tileWidth;
    cuts[cutCount++] = top;
    if (fullY > top && fullY < top + tileHeight) cuts[cutCount++] = fullY;
    if (clearY > top && clearY < top + tileHeight && clearY > fullY) cuts[cutCount++] = clearY;
    cuts[cutCount++] = top + tileHeight;
    for (x = start; x < (float)width; x += tileWidth) {
        int strip;

        for (strip = 0; strip + 1 < cutCount; ++strip) {
            float from = cuts[strip];
            float to = cuts[strip + 1];

            if (to <= from) continue;
            SpaceQuad(texture,
                      (Rectangle){0.0f, (from - top) / scale, (float)texture.width,
                                  (to - from) / scale},
                      (Rectangle){floorf(x), from, tileWidth, to - from},
                      SpaceMaskAt(from, amount, fullY, clearY),
                      SpaceMaskAt(to, amount, fullY, clearY));
        }
    }
}

static float SpaceShift(Camera2D camera, float travel, float parallax)
{
    return (camera.target.x + travel) * camera.zoom * parallax;
}

void SpaceRendererDraw(const SpaceRenderer *space, Camera2D camera, float travel,
                       int width, int height, float amount, float fullY, float clearY)
{
    float block;
    float lift;

    if (space == NULL || !space->ready || amount <= 0.004f || width <= 0 || height <= 0) {
        return;
    }
    if (amount > 1.0f) amount = 1.0f;
    block = SpaceBlock(height);
    /* The whole picture lifts a little as the camera climbs, the deepest
       layer least. */
    lift = -camera.target.y * camera.zoom * 0.002f;
    SpaceTile(space->nebula, SpaceShift(camera, travel, 0.0015f),
              (float)height * 0.5f - (float)SPACE_NEBULA_HEIGHT * block * 1.0f + lift * 0.5f,
              block * 2.0f, width, amount, fullY, clearY);
    SpaceTile(space->starsFar, SpaceShift(camera, travel, 0.003f),
              (float)height * 0.5f - (float)SPACE_STARS_HEIGHT * block * 0.5f + lift,
              block, width, amount, fullY, clearY);
    SpaceTile(space->starsNear, SpaceShift(camera, travel, 0.006f),
              (float)height * 0.5f - (float)SPACE_STARS_HEIGHT * block * 0.5f + lift * 1.5f,
              block, width, amount, fullY, clearY);
}

void SpaceRendererDrawEmissive(const SpaceRenderer *space, Camera2D camera,
                               float travel, int width, int height, float amount,
                               float fullY, float clearY)
{
    float block;
    float lift;

    if (space == NULL || !space->ready || amount <= 0.004f) {
        return;
    }
    if (amount > 1.0f) amount = 1.0f;
    block = SpaceBlock(height);
    lift = -camera.target.y * camera.zoom * 0.002f;
    SpaceTile(space->starsNear, SpaceShift(camera, travel, 0.006f),
              (float)height * 0.5f - (float)SPACE_STARS_HEIGHT * block * 0.5f + lift * 1.5f,
              block, width, amount * 0.63f, fullY, clearY);
}
