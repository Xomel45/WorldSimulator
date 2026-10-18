#version 460

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 camPos;  // xyz = позиция камеры, w = не используется
    mat4 prevViewProj;  // прошлый кадр, без джиттера
    vec4 jitter;        // xy = субпиксельный сдвиг в NDC
};

layout(push_constant) uniform PC {
    mat4 model;
    mat3 normalMatrix;  // transpose(inverse(mat3(model))), считается CPU-side (3 столбца по 16 байт)
    vec4 prevDelta;     // xyz = смещение объекта в мире за прошлый кадр (для векторов движения)
};

layout(location = 0) in vec4 inPos;     // xyz = позиция, w = baselineY (для deform compute)
layout(location = 1) in vec4 inNormal;  // xyz = нормаль, w = зарезервировано
layout(location = 2) in vec4 inColor;   // rgb = albedo (из MTL Kd или процедурный), a = 1

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec4 fragColor;
layout(location = 3) out vec4 curClip;   // позиция без джиттера: текущий кадр
layout(location = 4) out vec4 prevClip;  // та же точка в прошлом кадре

void main() {
    vec4 worldPos = model * vec4(inPos.xyz, 1.0);
    curClip       = proj * view * worldPos;
    prevClip      = prevViewProj * vec4(worldPos.xyz - prevDelta.xyz, 1.0);
    gl_Position   = curClip;
    gl_Position.xy += jitter.xy * curClip.w;  // субпиксельный сдвиг растра
    fragPos       = worldPos.xyz;
    fragNormal    = normalize(normalMatrix * inNormal.xyz);
    fragColor     = inColor;
}
