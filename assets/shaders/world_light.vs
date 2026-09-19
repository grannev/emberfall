#version 330

// raylib's default vertex shader, plus the one thing the light shader needs:
// where in the world the fragment is. In 2D mode every vertex the batch
// receives is already in world cells (the camera lives in mvp), so the
// position needs no transform of its own — a rotated terrain body and a
// static page both arrive with the coordinate the light field is indexed by.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;

uniform mat4 mvp;

out vec2 fragTexCoord;
out vec4 fragColor;
out vec2 fragWorld;

void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragWorld = vertexPosition.xy;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
