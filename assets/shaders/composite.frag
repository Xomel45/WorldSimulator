#version 460

layout(set = 0, binding = 0) uniform sampler2D inputTex;

layout(location = 0) in  vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    // Offscreen хранит перцептивные (gamma 2.2) значения — декодируем в linear,
    // HW снова закодирует при записи в sRGB swapchain.
    vec3 c = texture(inputTex, fragUV).rgb;
    outColor = vec4(pow(c, vec3(2.2)), 1.0);
}
