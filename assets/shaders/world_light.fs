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

out vec4 finalColor;

void main()
{
    vec4 texel = texture(texture0, fragTexCoord);

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

    float closing = clamp((veil.x - sky) / (veil.x - veil.y), 0.0, 1.0);
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
