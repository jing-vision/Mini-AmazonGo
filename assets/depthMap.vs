#version 150

uniform mat4    ciModelViewProjection;
uniform mat3    ciNormalMatrix;
uniform bool uFlipX;
uniform bool uFlipY;

in vec4     ciPosition;
in vec2     ciTexCoord0;

out vec2 TexCoord;

void main() 
{ 
    gl_Position = ciModelViewProjection * ciPosition;
    TexCoord = ciTexCoord0;
    TexCoord.x = uFlipX ? 1.0 - TexCoord.x : TexCoord.x;
    TexCoord.y = uFlipY ? 1.0 - TexCoord.y : TexCoord.y;
}
