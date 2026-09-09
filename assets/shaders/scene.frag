#version 460
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 camPos;
};

#include "lighting.glsl"

layout(location = 0) out vec4  outColor;
layout(location = 1) out float outDist;   // расстояние камера→фрагмент (для defocus)

// Растеризационный вариант: теней нет, всё освещено
float shadowFactor(vec3 origin, vec3 dir, float maxDist) { return 1.0; }

void main() {
    vec3 N   = normalize(fragNormal);
    vec3 V   = normalize(camPos.xyz - fragPos);
    vec3 albedo = fragColor.rgb;
    if (fragColor.a > 0.5) albedo *= concreteSurface(fragPos, N);  // бетон: деталь + бамп
    vec3 lit = shade(fragPos, N, V, albedo, 1.0);
    lit = applyFog(lit, fragPos, camPos.xyz);
    outColor = vec4(tonemapEncode(lit), 1.0);
    outDist  = length(camPos.xyz - fragPos);
}
