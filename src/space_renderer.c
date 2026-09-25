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
#define SPACE_PLANET_SIZE 176

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
    static const Color PALETTES[3][2] = {
        {{120, 60, 170, 255}, {40, 150, 170, 255}},
        {{170, 70, 50, 255}, {210, 150, 70, 255}},
        {{60, 90, 190, 255}, {190, 80, 140, 255}},
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
            alpha = floorf(alpha * 6.0f) / 6.0f;
            colour.a = (unsigned char)(alpha * 150.0f);
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
        if (roll > 0.985f) {
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

/* The giant: a banded sphere lit from the side, its night side dark, a thin
   bright rim of atmosphere, and a ring across it. */
static Image SpaceBuildPlanet(uint64_t seed)
{
    static const Color BANDS[3][3] = {
        {{196, 150, 104, 255}, {150, 96, 70, 255}, {226, 196, 150, 255}},
        {{110, 150, 190, 255}, {70, 96, 150, 255}, {180, 210, 230, 255}},
        {{170, 120, 170, 255}, {110, 76, 130, 255}, {220, 180, 210, 255}},
    };
    const Color *bands = BANDS[SpaceHash(seed, 0, 0, 57) % 3u];
    Image image = GenImageColor(SPACE_PLANET_SIZE, SPACE_PLANET_SIZE, BLANK);
    Color *pixels = image.data;
    float centre = (float)SPACE_PLANET_SIZE * 0.5f;
    float radius = (float)SPACE_PLANET_SIZE * 0.30f;
    const float lightX = -0.62f;
    const float lightY = -0.35f;
    const float lightZ = 0.70f;
    int y;

    for (y = 0; y < SPACE_PLANET_SIZE; ++y) {
        int x;

        for (x = 0; x < SPACE_PLANET_SIZE; ++x) {
            float dx = ((float)x + 0.5f - centre) / radius;
            float dy = ((float)y + 0.5f - centre) / radius;
            float distance = sqrtf(dx * dx + dy * dy);
            /* The ring, a tilted ellipse, drawn behind the planet above its
               equator and in front of it below. */
            float ringY = (dy - dx * 0.18f) * 4.2f;
            float ring = sqrtf(dx * dx + ringY * ringY);
            /* Bands of the ring: dense and pale inside, a dark gap, a
               thinner outer band. */
            float ringDensity = ring < 1.35f || ring > 2.05f
                                    ? 0.0f
                                    : (ring < 1.62f ? 0.9f : (ring < 1.70f ? 0.15f : 0.55f));
            bool inRing = ringDensity > 0.0f;
            Color colour = BLANK;

            if (distance <= 1.0f) {
                float nz = sqrtf(fmaxf(0.0f, 1.0f - distance * distance));
                float lit = dx * lightX + dy * lightY + nz * lightZ;
                float latitude = dy + 0.08f * SpaceNoise(seed, (float)x, 0.0f, 9.0f, 64, 71);
                float band = SpaceNoise(seed, 0.0f, latitude * 30.0f, 1.7f, 1, 83);
                float shade = fmaxf(0.0f, lit);
                Color base = band < 0.33f ? bands[1] : (band < 0.72f ? bands[0] : bands[2]);

                shade = floorf((0.12f + 0.88f * shade) * 8.0f) / 8.0f;
                colour = (Color){(unsigned char)((float)base.r * shade),
                                 (unsigned char)((float)base.g * shade),
                                 (unsigned char)((float)base.b * shade), 255};
                /* The atmosphere's rim, where the lit side meets the dark. */
                if (distance > 0.93f && lit > -0.1f) {
                    colour = SpaceMix(colour, (Color){220, 235, 255, 255}, 0.55f);
                }
                if (inRing && ringY > 0.0f) {
                    colour = SpaceMix(colour, (Color){200, 190, 170, 255}, 0.75f * ringDensity);
                }
            } else if (inRing) {
                /* The ring on the far side of the planet is in its shadow
                   where the planet stands between it and the light. */
                float shadow = (dx > 0.0f && fabsf(dy) < 0.9f) ? 0.55f : 1.0f;

                colour = (Color){(unsigned char)(196.0f * shadow), (unsigned char)(184.0f * shadow),
                                 (unsigned char)(162.0f * shadow),
                                 (unsigned char)(230.0f * ringDensity)};
            } else if (distance < 1.08f) {
                /* A thin haze of atmosphere beyond the limb. */
                colour = (Color){160, 200, 255,
                                 (unsigned char)(120.0f * (1.08f - distance) / 0.08f)};
            }
            pixels[y * SPACE_PLANET_SIZE + x] = colour;
        }
    }
    return image;
}

static void SpaceUnloadTextures(SpaceRenderer *space)
{
    if (space->nebula.id != 0u) UnloadTexture(space->nebula);
    if (space->starsFar.id != 0u) UnloadTexture(space->starsFar);
    if (space->starsNear.id != 0u) UnloadTexture(space->starsNear);
    if (space->planet.id != 0u) UnloadTexture(space->planet);
    space->nebula = (Texture2D){0};
    space->starsFar = (Texture2D){0};
    space->starsNear = (Texture2D){0};
    space->planet = (Texture2D){0};
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
    int index;

    SpaceUnloadTextures(space);
    space->seed = seed;
    space->nebula = SpaceUpload(SpaceBuildNebula(seed));
    space->starsFar = SpaceUpload(SpaceBuildStars(seed, 5200, 0.55f, 1));
    space->starsNear = SpaceUpload(SpaceBuildStars(seed, 900, 1.0f, 2));
    space->planet = SpaceUpload(SpaceBuildPlanet(seed));
    space->ready = space->nebula.id != 0u && space->starsFar.id != 0u &&
                   space->starsNear.id != 0u && space->planet.id != 0u;
    for (index = 0; index < SPACE_ASTEROID_COUNT; ++index) {
        space->asteroids[index] = (SpaceAsteroid){
            .x = SpaceUnit(seed, index, 11, 5),
            .y = 0.08f + SpaceUnit(seed, index, 12, 5) * 0.84f,
            .radius = 2.0f + SpaceUnit(seed, index, 13, 5) * 6.0f,
            .parallax = 0.012f + SpaceUnit(seed, index, 14, 5) * 0.03f,
            .drift = (SpaceUnit(seed, index, 15, 5) - 0.5f) * 2.0f,
            .salt = index * 37 + 3,
        };
    }
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

/* Tiles `texture` across the target at `scale` screen pixels per texel,
   scrolled by `shiftX`, placed at `top`. */
static void SpaceTile(Texture2D texture, float shiftX, float top, float scale, int width,
                      Color tint)
{
    float tileWidth = (float)texture.width * scale;
    float start = -fmodf(shiftX, tileWidth);
    float x;

    if (start > 0.0f) start -= tileWidth;
    for (x = start; x < (float)width; x += tileWidth) {
        DrawTexturePro(texture,
                       (Rectangle){0.0f, 0.0f, (float)texture.width, (float)texture.height},
                       (Rectangle){floorf(x), floorf(top), tileWidth,
                                   (float)texture.height * scale},
                       (Vector2){0.0f, 0.0f}, 0.0f, tint);
    }
}

static float SpaceShift(Camera2D camera, float travel, float parallax)
{
    return (camera.target.x + travel) * camera.zoom * parallax;
}

/* An asteroid: a lumpy rock in blocks, lit from the upper left, turning
   slowly. */
static void SpaceDrawAsteroid(const SpaceAsteroid *asteroid, float cx, float cy,
                              float block, float time, float alpha)
{
    int radius = (int)asteroid->radius;
    float spin = time * (0.05f + 0.02f * (float)(asteroid->salt % 5));
    int row;

    for (row = -radius; row <= radius; ++row) {
        int column;

        for (column = -radius - 1; column <= radius + 1; ++column) {
            float angle = atan2f((float)row, (float)column) + spin;
            float edge = (float)radius *
                         (0.78f + 0.22f * sinf(angle * 3.0f + (float)asteroid->salt) *
                                      cosf(angle * 2.0f));
            float distance = sqrtf((float)(row * row + column * column));
            float light;
            unsigned char level;

            if (distance > edge) continue;
            light = 0.45f + 0.55f * ((float)(-column - row) / (2.0f * (float)radius) + 0.5f);
            if (light > 1.0f) light = 1.0f;
            level = (unsigned char)(120.0f * light);
            DrawRectangle((int)(cx + (float)column * block), (int)(cy + (float)row * block),
                          (int)block, (int)block,
                          (Color){(unsigned char)(level + 12), level,
                                  (unsigned char)(level * 0.82f),
                                  (unsigned char)(255.0f * alpha)});
        }
    }
}

void SpaceRendererDraw(const SpaceRenderer *space, Camera2D camera, float travel,
                       int width, int height, float amount, float time, bool full)
{
    float block;
    float lift;
    int index;

    if (space == NULL || !space->ready || amount <= 0.004f || width <= 0 || height <= 0) {
        return;
    }
    if (amount > 1.0f) amount = 1.0f;
    block = SpaceBlock(height);
    /* The whole picture lifts a little as the camera climbs, the deepest
       layer least. */
    lift = -camera.target.y * camera.zoom * 0.002f;
    if (full) {
        DrawRectangleGradientV(0, 0, width, height,
                               (Color){2, 2, 8, (unsigned char)(255.0f * amount)},
                               (Color){6, 6, 18, (unsigned char)(255.0f * amount)});
    }
    SpaceTile(space->nebula, SpaceShift(camera, travel, 0.0015f),
              (float)height * 0.5f - (float)SPACE_NEBULA_HEIGHT * block * 1.0f + lift * 0.5f,
              block * 2.0f, width, (Color){255, 255, 255, (unsigned char)(255.0f * amount)});
    SpaceTile(space->starsFar, SpaceShift(camera, travel, 0.003f),
              (float)height * 0.5f - (float)SPACE_STARS_HEIGHT * block * 0.5f + lift,
              block, width, (Color){255, 255, 255, (unsigned char)(255.0f * amount)});
    SpaceTile(space->starsNear, SpaceShift(camera, travel, 0.006f),
              (float)height * 0.5f - (float)SPACE_STARS_HEIGHT * block * 0.5f + lift * 1.5f,
              block, width, (Color){255, 255, 255, (unsigned char)(255.0f * amount)});
    if (!full) {
        return;
    }
    /* The giant, low on one side, drifting only a little. */
    {
        float scale = block * 0.9f;
        float size = (float)SPACE_PLANET_SIZE * scale;
        float period = (float)width + size;
        float x = fmodf(-SpaceShift(camera, travel, 0.004f) + (float)width * 0.72f, period);

        if (x < 0.0f) x += period;
        DrawTexturePro(space->planet,
                       (Rectangle){0.0f, 0.0f, (float)SPACE_PLANET_SIZE,
                                   (float)SPACE_PLANET_SIZE},
                       (Rectangle){floorf(x - size * 0.5f),
                                   floorf((float)height * 0.68f - size * 0.5f + lift * 2.0f),
                                   size, size},
                       (Vector2){0.0f, 0.0f}, 0.0f,
                       (Color){255, 255, 255, (unsigned char)(255.0f * amount)});
    }
    for (index = 0; index < SPACE_ASTEROID_COUNT; ++index) {
        const SpaceAsteroid *asteroid = &space->asteroids[index];
        float period = (float)width + 40.0f * block;
        float x = fmodf(asteroid->x * period - SpaceShift(camera, travel, asteroid->parallax) +
                            time * asteroid->drift * block,
                        period);

        if (x < 0.0f) x += period;
        SpaceDrawAsteroid(asteroid, x - 20.0f * block,
                          asteroid->y * (float)height + lift * 3.0f, block, time, amount);
    }
}

void SpaceRendererDrawEmissive(const SpaceRenderer *space, Camera2D camera,
                               float travel, int width, int height, float amount,
                               float time)
{
    float block;
    float lift;

    (void)time;
    if (space == NULL || !space->ready || amount <= 0.004f) {
        return;
    }
    if (amount > 1.0f) amount = 1.0f;
    block = SpaceBlock(height);
    lift = -camera.target.y * camera.zoom * 0.002f;
    SpaceTile(space->starsNear, SpaceShift(camera, travel, 0.006f),
              (float)height * 0.5f - (float)SPACE_STARS_HEIGHT * block * 0.5f + lift * 1.5f,
              block, width, (Color){255, 255, 255, (unsigned char)(160.0f * amount)});
}
