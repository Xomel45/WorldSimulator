#version 460

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 camPos;  // xyz = позиция камеры, w = не используется
};

layout(push_constant) uniform PC {
    mat4 model;
    mat4 normalMatrix;  // transpose(inverse(model)), считается CPU-side
};

layout(location = 0) in vec4 inPos;     // xyz = позиция, w = baselineY (для deform compute)
layout(location = 1) in vec4 inNormal;  // xyz = нормаль, w = зарезервировано
layout(location = 2) in vec4 inColor;   // rgb = albedo (из MTL Kd или процедурный), a = 1

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragColor;

void main() {
    vec4 worldPos = model * vec4(inPos.xyz, 1.0);
    gl_Position   = proj * view * worldPos;
    fragPos       = worldPos.xyz;
    fragNormal    = normalize(mat3(normalMatrix) * inNormal.xyz);
    fragColor     = inColor;
}
