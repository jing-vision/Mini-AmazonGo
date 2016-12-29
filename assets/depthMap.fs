#version 150

uniform usampler2D uDepthTexture;
uniform float uMinDepthForVizMM;
uniform float uMaxDepthForVizMM;

in vec2 TexCoord;

out vec4 oColor;

void main(void)
{ 
#if 1
    float d = texture(uDepthTexture, TexCoord).r;
    d = (d - uMinDepthForVizMM) / uMaxDepthForVizMM;
    oColor = vec4(d);
#else
    int d = int(texture(uDepthTexture, TexCoord).r);
    int r = ((d>>8)&0xff)<<2;
    int g = ((d>>4)&0x3f)<<2;
    int b = (d&0xff)<<3;
    oColor = vec4(r / 255.0, g / 255.0, b / 255.0, 1);
#endif
}
