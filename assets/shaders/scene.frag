#version 460
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec4 fragColor;
layout(location = 3) in vec4 curClip;
layout(location = 4) in vec4 prevClip;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec4 camPos;
    mat4 prevViewProj;
    vec4 jitter;
};

#define WSIM_RASTER_SHADOWS
#include "lighting.glsl"
#include "materials.glsl"

layout(location = 0) out vec4  outColor;
layout(location = 1) out float outDist;   // расстояние камера→фрагмент (для defocus)
layout(location = 2) out vec2  outMotion; // сдвиг в UV: текущая позиция - прошлая (без джиттера)

// Точечные и фонарик в растеризации без теней; тень солнца -- карта теней (sunShadow в lighting.glsl)
float shadowFactor(vec3 origin, vec3 dir, float maxDist) { return 1.0; }

void main() {
    vec3  N      = normalize(fragNormal);
    vec3  V      = normalize(camPos.xyz - fragPos);
    float dist   = length(camPos.xyz - fragPos);
    vec3  albedo = fragColor.rgb;
    float fresnel = 0.0;
    bool  water   = matIs(fragColor.a, MAT_WATER);

    if (matIs(fragColor.a, MAT_TERRAIN))        albedo = terrainMaterial(fragPos, N, dist, fragColor.rg, lights.fogParams.z, lights.fogParams.w);
    else if (water)                              albedo = waterMaterial(fragPos, N, V, dist, fresnel);
    else if (matIs(fragColor.a, MAT_CREATURE))   albedo = creatureMaterial(albedo, fragPos);
    else if (matIs(fragColor.a, MAT_LEAF)) {
        float bare;
        albedo = leafMaterial(albedo, fragPos, lights.shadowParams.w, bare);
        if (bare > 0.0 && wsimVNoise(fragPos * 2.6 + 3.0) < bare * 0.8) discard;   // зимой листва редеет
    }

    vec3 lit = shade(fragPos, N, V, albedo, 1.0);

    if (water) {
        // Вода отражает небо (цвет тумана = цвет горизонта) и солнечный блик.
        vec3 sky = lights.fogColorDensity.w > 0.0 ? lights.fogColorDensity.rgb : vec3(0.5, 0.65, 0.85);
        lit = mix(lit, sky, clamp(fresnel * 0.75 + 0.05, 0.0, 1.0));
        if (lights.sunColorIntensity.w > 0.0) {
            vec3  L    = normalize(lights.sunDir.xyz);
            float glint = pow(max(dot(reflect(-L, N), V), 0.0), 220.0);
            lit += lights.sunColorIntensity.rgb * lights.sunColorIntensity.w * glint * 1.4 * sunShadow(fragPos, vec3(0.0, 1.0, 0.0));
        }
    }

    lit = applyFog(lit, fragPos, camPos.xyz);
    outColor = vec4(tonemapEncode(lit), 1.0);
    outDist  = dist;
    outMotion = (curClip.xy / curClip.w - prevClip.xy / prevClip.w) * 0.5;
}
