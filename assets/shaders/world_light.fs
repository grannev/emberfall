#version 330

// Lights the unlit world pixels — static pages and detached terrain bodies —
// from the coarse light field. This is the GPU half of world_lighting.h: the
// tint and the air veil are the same formulas, on the same constants, and the
// two must change together.
//
// One shader serves both planes. In the scene pass a pixel is its material
// colour under the light; in the emissive pass it is its own glow, unlit, and
// the light only decides how much of the sky the air lets through.

in vec2 fragTexCoord;
in vec4 fragColor;
in vec2 fragWorld;

uniform sampler2D texture0;
uniform vec4 colDiffuse;

// R is the fraction of full daylight reaching a sample, G is ember. Sampled
// bilinearly by the hardware, which is the same interpolation the CPU used
// to do per cell.
uniform sampler2D lightMap;
uniform vec2 lightTexel;
uniform float lightScale;
uniform float daylight;
uniform float minimumLight;
uniform vec3 warmth;
// x: sky light at and above which air is a window; y: at and below which it is
// ground.
uniform vec2 veil;
// x: alpha of open air; y: how much more sealed air has.
uniform vec2 veilAlpha;
// Alpha that marks an air texel. Zero is "nothing here": a body's empty raster
// cell, which must stay invisible.
uniform float airAlpha;
uniform int emissivePass;

// The plants' layer (swayPass == 1): its alpha is how freely a pixel sways,
// from floraAlpha (still) to 1 (the most), and every pixel is displaced by
// the wind, the breeze, the character brushing past and any recent blast.
uniform int swayPass;
uniform float floraAlpha;
uniform float swayTime;
// Wind along the ground, cells per second, signed.
uniform float swayWind;
// The character: position and velocity.
uniform vec4 swayPlayer;
// Up to four recent blasts: position, radius, strength (cells of push).
uniform vec4 swayBlasts[4];
// One texel of the page texture, in texture coordinates.
uniform vec2 pageTexel;

out vec4 finalColor;

float SwayWeight(vec4 texel)
{
    return texel.a >= floraAlpha - 0.5 / 255.0
               ? (texel.a - floraAlpha) / (1.0 - floraAlpha)
               : 0.0;
}

// How far a plant pixel at `world` with sway `weight` is carried, in cells.
vec2 Sway(vec2 world, float weight)
{
    float wind = swayWind / 45.0;
    float breeze = sin(swayTime * 1.7 + world.x * 0.09 + world.y * 0.03) * 0.6 +
                   sin(swayTime * 0.9 + world.x * 0.023) * 0.4;
    float gust = sin(swayTime * 3.1 + world.x * 0.05) * abs(wind);
    float dx = (wind * 2.4 + gust * 1.3 + breeze * (0.45 + abs(wind))) * weight;

    // Parted by the character: away from them, harder the faster they go.
    vec2 rel = world - swayPlayer.xy;
    if (abs(rel.y) < 14.0 && abs(rel.x) < 16.0) {
        float near = (1.0 - abs(rel.x) / 16.0) * (1.0 - abs(rel.y) / 14.0);
        dx += sign(rel.x) * near * weight *
              (2.2 + min(abs(swayPlayer.z) / 40.0, 3.0));
    }
    // Flattened by a blast, outward from it.
    for (int i = 0; i < 4; ++i) {
        vec4 blast = swayBlasts[i];
        if (blast.z <= 0.0) continue;
        vec2 away = world - blast.xy;
        float distance = length(away);
        if (distance < blast.z) {
            dx += sign(away.x) * (1.0 - distance / blast.z) * blast.w * weight;
        }
    }
    dx = clamp(dx, -9.0, 9.0);
    // A blade bent over is a little shorter.
    return vec2(dx, abs(dx) * 0.25 * weight);
}

void main()
{
    vec4 texel;

    if (swayPass == 1) {
        // Where the pixel now over this fragment came from: guessed with the
        // largest sway, then corrected with the sway of what is there.
        vec2 guess = Sway(fragWorld, 1.0);
        vec4 first = texture(texture0, fragTexCoord - guess * pageTexel);
        vec2 carried = Sway(fragWorld, SwayWeight(first));

        texel = texture(texture0, fragTexCoord - carried * pageTexel);
        if (texel.a < floraAlpha - 0.5 / 255.0) {
            discard;
        }
        texel.a = 1.0;
        if (emissivePass == 1) {
            // A plant glows no more than the page would have let it: it is
            // an occluder here, as solid as the scene draws it.
            finalColor = vec4(0.0, 0.0, 0.0, 1.0) * colDiffuse * fragColor;
            return;
        }
    } else {
        texel = texture(texture0, fragTexCoord);
    }

    if (texel.a <= 0.0) {
        discard;
    }

    // Sampled at the centre of the cell the fragment lies in, so the light is
    // constant across a cell exactly as it was when it was baked per cell.
    vec2 cell = floor(fragWorld) + 0.5;
    vec2 light = texture(lightMap, cell / lightScale * lightTexel).rg;
    float sky = light.r * daylight;
    float ember = light.g;
    bool air = texel.a <= airAlpha + 0.5 / 255.0;

    // The veil follows how open a cell is to the sky, not how bright the sky
    // is: closing it on the daylight-scaled light drew a wall of dark air
    // over the whole night sky, and the stars and the moon behind it went
    // out at dusk. Night is the backdrop's to draw; the veil only says
    // whether there is ground between the viewer and it.
    float closing = clamp((veil.x - light.r) / (veil.x - veil.y), 0.0, 1.0);
    closing = closing * closing * (3.0 - 2.0 * closing);
    float veiled = veilAlpha.x + veilAlpha.y * closing;

    if (emissivePass == 1) {
        finalColor = air ? vec4(0.0, 0.0, 0.0, veiled) : vec4(texel.rgb, 1.0);
        finalColor *= colDiffuse * fragColor;
        return;
    }

    float brightest = max(sky, ember);
    float level = minimumLight + (1.0 - minimumLight) * brightest;
    float warm = max(ember - sky, 0.0);
    vec3 tint = level * vec3(1.0 + warmth.r * warm, 1.0 - warmth.g * warm,
                             1.0 - warmth.b * warm);

    finalColor = vec4(min(texel.rgb * tint, vec3(1.0)), air ? veiled : texel.a) *
                 colDiffuse * fragColor;
}
