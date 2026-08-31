#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 sourceTexelSize;
uniform float threshold;

out vec4 finalColor;

void main()
{
    vec2 offset = sourceTexelSize * 0.5;
    vec3 color =
        (texture(texture0, fragTexCoord + vec2(-offset.x, -offset.y)).rgb +
         texture(texture0, fragTexCoord + vec2( offset.x, -offset.y)).rgb +
         texture(texture0, fragTexCoord + vec2(-offset.x,  offset.y)).rgb +
         texture(texture0, fragTexCoord + vec2( offset.x,  offset.y)).rgb) * 0.25;
    float peak = max(color.r, max(color.g, color.b));
    float contribution = clamp((peak - threshold) /
                               max(1.0 - threshold, 0.001), 0.0, 1.0);

    finalColor = vec4(color * contribution * fragColor.rgb, 1.0);
}
