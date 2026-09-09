#version 460

layout(set = 0, binding = 0) uniform sampler2D inputTex;

layout(location = 0) in  vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec2 direction;  // (1,0) = horizontal, (0,1) = vertical
    int  radius;     // blur radius in pixels
} pc;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(inputTex, 0));
    vec4 acc   = vec4(0.0);
    float wsum = 0.0;

    for (int i = -pc.radius; i <= pc.radius; ++i) {
        float w  = exp(-0.5 * float(i * i) / float(pc.radius * pc.radius));
        acc     += texture(inputTex, fragUV + float(i) * texel * pc.direction) * w;
        wsum    += w;
    }

    outColor = acc / wsum;
}
