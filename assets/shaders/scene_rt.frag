#version 460
#extension GL_GOOGLE_include_directive       : require
#extension GL_EXT_ray_query                  : require
#extension GL_EXT_buffer_reference2          : require

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

#include "lighting.glsl"
#include "materials.glsl"

layout(set = 0, binding = 2) uniform accelerationStructureEXT topLevelAS;

// ─── Геометрия для hit-shading (GI / отражения) ─────────────────────────────
// Зеркалит wsim::Vertex (3× vec4 = 48 байт) и uint32 индексы.
struct RtVertex { vec4 pos; vec4 nrm; vec4 col; };
layout(buffer_reference, std430) readonly buffer Vertices { RtVertex v[]; };
layout(buffer_reference, std430) readonly buffer Indices  { uint     i[]; };
struct Geo { Vertices verts; Indices inds; };
layout(set = 0, binding = 3, std430) readonly buffer GeoTable { Geo geos[]; };

layout(location = 0) out vec4  outColor;
layout(location = 1) out float outDist;   // расстояние камера→фрагмент (для defocus)
layout(location = 2) out vec2  outMotion; // сдвиг в UV: текущая позиция - прошлая (без джиттера)

// ─── RNG (PCG hash, per-pixel) + низкодискрепантная выборка ──────────────────
uint gSeed;
uint pcgHash(uint v) {
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (v >> 22u) ^ v;
}
float rnd() { gSeed = pcgHash(gSeed); return float(gSeed) * (1.0 / 4294967296.0); }

// Van der Corput (радикальный обратный код, основание 2) → Hammersley.
// Стратифицированная выборка резко снижает дисперсию при том же числе лучей —
// меньше зерна в тенях/AO/GI, чем при чистом белом шуме.
float radicalInverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
// i-я точка Hammersley из n + поворот Крэнли–Паттерсона (jitter на пиксель):
// набор одинаково стратифицирован, но у каждого пикселя свой сдвиг → нет
// видимого межпиксельного паттерна при низкой дисперсии внутри пикселя.
vec2 hammersley(uint i, uint n, vec2 jitter) {
    return fract(vec2(float(i) / float(n), radicalInverse(i)) + jitter);
}

// Число лучей на мягкую тень — снижается на вторичных отскоках (GI/отражения)
int gShadowSamples;

// Ортонормированный базис вокруг n (Duff et al. 2017)
void basis(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float d = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * d, -s * n.x);
    b = vec3(d, s + n.y * n.y * a, -n.y);
}

// Косинус-взвешенное направление в полусфере вокруг N по стратифицированной паре u
vec3 cosineDir(vec3 N, vec2 u) {
    vec3 t, b;
    basis(N, t, b);
    float r = sqrt(u.x);
    float a = 6.2831853 * u.y;
    return normalize(t * (r * cos(a)) + b * (r * sin(a)) + N * sqrt(max(0.0, 1.0 - r * r)));
}

// Один непрозрачный occlusion-луч (первое пересечение = попадание)
bool occluded(vec3 origin, vec3 dir, float maxDist) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
        0xFF, origin, 0.015, dir, maxDist);
    rayQueryProceedEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT;
}

// ─── Мягкая тень: пучок лучей в конусе (аппроксимация площадного источника) ──
const float kLightAngle = 0.03;  // угловой радиус источника (рад) → ширина полутени

float shadowFactor(vec3 origin, vec3 dir, float maxDist) {
    if (maxDist <= 0.0) return 1.0;
    vec3 t, b;
    basis(dir, t, b);
    vec2  jit = vec2(rnd(), rnd());              // поворот стратифицированного диска
    float occ = 0.0;
    for (int i = 0; i < gShadowSamples; ++i) {
        vec2  u  = hammersley(uint(i), uint(gShadowSamples), jit);
        float r  = kLightAngle * sqrt(u.x);
        float a  = 6.2831853 * u.y;
        vec3  jd = normalize(dir + (t * cos(a) + b * sin(a)) * r);
        if (occluded(origin, jd, maxDist)) occ += 1.0;
    }
    return 1.0 - occ / float(gShadowSamples);
}

// ─── Ambient occlusion: косинус-взвешенная полусфера, короткая дистанция ─────
#define WSIM_AO_SAMPLES 8
const float kAoRadius = 1.2;

float computeAO(vec3 P, vec3 N) {
    vec2  jit = vec2(rnd(), rnd());
    float occ = 0.0;
    for (int i = 0; i < WSIM_AO_SAMPLES; ++i) {
        vec2 u = hammersley(uint(i), uint(WSIM_AO_SAMPLES), jit);
        if (occluded(P + N * 0.02, cosineDir(N, u), kAoRadius)) occ += 1.0;
    }
    return 1.0 - occ / float(WSIM_AO_SAMPLES);
}

// Освещение точки попадания вторичного луча: достаём вершины меша по
// customIndex/primitive, интерполируем нормаль+albedo, считаем прямой свет.
vec3 shadeHit(rayQueryEXT rq, vec3 origin, vec3 dir) {
    int   inst = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
    int   prim = rayQueryGetIntersectionPrimitiveIndexEXT(rq, true);
    vec2  bc   = rayQueryGetIntersectionBarycentricsEXT(rq, true);
    float t    = rayQueryGetIntersectionTEXT(rq, true);

    Geo g = geos[inst];
    uint i0 = g.inds.i[prim * 3 + 0];
    uint i1 = g.inds.i[prim * 3 + 1];
    uint i2 = g.inds.i[prim * 3 + 2];
    RtVertex v0 = g.verts.v[i0];
    RtVertex v1 = g.verts.v[i1];
    RtVertex v2 = g.verts.v[i2];

    vec3 w      = vec3(1.0 - bc.x - bc.y, bc.x, bc.y);
    vec3 localN = v0.nrm.xyz * w.x + v1.nrm.xyz * w.y + v2.nrm.xyz * w.z;
    vec3 albedo = v0.col.rgb * w.x + v1.col.rgb * w.y + v2.col.rgb * w.z;

    mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
    vec3 N = normalize(mat3(o2w) * localN);
    if (dot(N, -dir) < 0.0) N = -N;              // двусторонняя нормаль
    vec3 P = origin + dir * t;
    return shade(P, N, -dir, albedo, 1.0);
}

// ─── Однократный диффузный отскок (GI) ──────────────────────────────────────
#define WSIM_GI_SAMPLES 3
const float kGiDist = 8.0;

vec3 computeGI(vec3 P, vec3 N) {
    vec3 sum = vec3(0.0);
    vec2 jit = vec2(rnd(), rnd());
    gShadowSamples = 1;                          // на отскоке тень дешёвая
    for (int i = 0; i < WSIM_GI_SAMPLES; ++i) {
        vec3 dir = cosineDir(N, hammersley(uint(i), uint(WSIM_GI_SAMPLES), jit));
        vec3 o   = P + N * 0.02;
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, topLevelAS, gl_RayFlagsOpaqueEXT,
            0xFF, o, 0.015, dir, kGiDist);
        while (rayQueryProceedEXT(rq)) {}
        if (rayQueryGetIntersectionTypeEXT(rq, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT)
            sum += shadeHit(rq, o, dir);         // косинус-семплинг → без доп. cos
    }
    return sum / float(WSIM_GI_SAMPLES);
}

// ─── Глянцевое отражение (одно, Френель-взвешенное) ─────────────────────────
const float kReflDist = 12.0;

vec3 computeReflection(vec3 P, vec3 N, vec3 V) {
    vec3 R = reflect(-V, N);
    vec3 o = P + N * 0.02;
    gShadowSamples = 1;
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, topLevelAS, gl_RayFlagsOpaqueEXT,
        0xFF, o, 0.015, R, kReflDist);
    while (rayQueryProceedEXT(rq)) {}
    if (rayQueryGetIntersectionTypeEXT(rq, true) !=
        gl_RayQueryCommittedIntersectionNoneEXT)
        return shadeHit(rq, o, R);
    return lights.ambient.rgb * lights.ambient.w;  // «небо» = ambient
}

void main() {
    gSeed = pcgHash(uint(gl_FragCoord.x) + pcgHash(uint(gl_FragCoord.y)));

    vec3  N  = normalize(fragNormal);
    vec3  V  = normalize(camPos.xyz - fragPos);

    vec3 albedo = fragColor.rgb;
    if (matIs(fragColor.a, MAT_TERRAIN)) albedo = terrainMaterial(fragPos, N, length(camPos.xyz - fragPos));
    else if (matIs(fragColor.a, MAT_CREATURE)) albedo = creatureMaterial(albedo, fragPos);

    gShadowSamples = 4;                           // первичные тени — мягкие
    float ao  = computeAO(fragPos, N);
    vec3  lit = shade(fragPos, N, V, albedo, ao);

    // Однократный диффузный отскок
    lit += albedo * computeGI(fragPos, N);

    // Тонкое глянцевое отражение (Шлик-Френель, слабое для матовой краски)
    float fres = 0.04 + 0.20 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
    lit += computeReflection(fragPos, N, V) * fres;

    lit = applyFog(lit, fragPos, camPos.xyz);
    outColor = vec4(tonemapEncode(lit), 1.0);
    outDist  = length(camPos.xyz - fragPos);
    outMotion = (curClip.xy / curClip.w - prevClip.xy / prevClip.w) * 0.5;
}
