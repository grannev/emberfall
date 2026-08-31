#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform sampler2D texture0;
uniform vec2 texelSize;
uniform vec2 direction;
uniform float radius;

out vec4 finalColor;

void main()
{
    vec2 stepOffset = texelSize * direction * radius;
    vec3 color = texture(texture0, fragTexCoord).rgb * 0.227027;

    color += texture(texture0, fragTexCoord + stepOffset * 1.384615).rgb * 0.316216;
    color += texture(texture0, fragTexCoord - stepOffset * 1.384615).rgb * 0.316216;
    color += texture(texture0, fragTexCoord + stepOffset * 3.230769).rgb * 0.070270;
    color += texture(texture0, fragTexCoord - stepOffset * 3.230769).rgb * 0.070270;

    finalColor = vec4(color * fragColor.rgb, 1.0);
}
