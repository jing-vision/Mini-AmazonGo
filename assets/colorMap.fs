#version 150

uniform sampler2D uColorTexture;
uniform sampler2D uDepthToColorTableTexture;

in vec2 TexCoord;

out vec4 oColor;

void main(void)
{ 
    vec2 uv = texture(uDepthToColorTableTexture, TexCoord).xy;
    uv.y = 1.0 - uv.y;
    oColor = texture(uColorTexture, uv);
}
