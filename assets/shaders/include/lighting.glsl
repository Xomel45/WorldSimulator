// lighting.glsl — общая модель освещения (Blinn-Phong) для scene.frag / scene_rt.frag.
// Подключающий шейдер обязан:
//   1. объявить CameraUBO (set=0, binding=0) ДО include
//   2. определить float shadowFactor(vec3 origin, vec3 dir, float maxDist)
//      (1.0 = полностью освещено, 0.0 = полная тень)

#define WSIM_MAX_POINT_LIGHTS 8

layout(set = 0, binding = 1) uniform LightsUBO {
    vec4  ambient;                                // rgb, w = интенсивность
    vec4  sunDir;                                 // xyz = направление К солнцу (уже L, не позиция), нормализовано
    vec4  sunColorIntensity;                       // rgb, w = интенсивность (0 = выкл)
    vec4  spotPosCosIn;                           // xyz = позиция, w = cos(inner)
    vec4  spotDirCosOut;                          // xyz = направление, w = cos(outer)
    vec4  spotColor;                              // rgb, w = интенсивность (0 = выкл)
    vec4  pointPosRadius[WSIM_MAX_POINT_LIGHTS];   // xyz = позиция, w = радиус
    vec4  pointColorInt[WSIM_MAX_POINT_LIGHTS];    // rgb, w = интенсивность
    ivec4 counts;                                 // x = число точечных источников
    vec4  fogColorDensity;                        // rgb = цвет тумана, w = базовая плотность
    vec4  fogParams;                              // x = height falloff, y = время (с), zw резерв
} lights;

float shadowFactor(vec3 origin, vec3 dir, float maxDist);

// Физически-правдоподобное затухание с жёсткой границей радиуса (UE-style)
float distAtten(float d, float radius) {
    float x = clamp(1.0 - pow(d / radius, 4.0), 0.0, 1.0);
    return x * x / (d * d + 1.0);
}

vec3 blinnPhong(vec3 albedo, vec3 N, vec3 V, vec3 L, float specPower, float specStrength) {
    float ndl  = max(dot(N, L), 0.0);
    vec3  H    = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), specPower) * specStrength;
    return albedo * ndl + vec3(spec) * ndl;
}

// ao — фактор затенения ambient (1.0 = открыто, 0.0 = в щели); для растеризации 1.0.
vec3 shade(vec3 P, vec3 N, vec3 V, vec3 albedo, float ao) {
    vec3 lit = albedo * lights.ambient.rgb * lights.ambient.w * ao;

    // Солнце: направленный свет, без позиции/затухания. shadowFactor те же
    // хуки, что у точечных/фонарика — бесплатные RT-тени в scene_rt.frag,
    // no-op (всегда освещено) в растеризации scene.frag, тот же контракт.
    if (lights.sunColorIntensity.w > 0.0) {
        vec3 L = normalize(lights.sunDir.xyz);
        if (dot(N, L) > 0.0) {
            float sh = shadowFactor(P + N * 0.02, L, 1000.0);
            if (sh > 0.0) {
                vec3 c = lights.sunColorIntensity.rgb * lights.sunColorIntensity.w;
                lit += blinnPhong(albedo, N, V, L, 16.0, 0.05) * c * sh;
            }
        }
    }

    // Точечные источники (лампочки)
    for (int i = 0; i < lights.counts.x; ++i) {
        vec3  Lv = lights.pointPosRadius[i].xyz - P;
        float d  = length(Lv);
        float r  = lights.pointPosRadius[i].w;
        if (d >= r) continue;
        vec3 L = Lv / d;
        if (dot(N, L) <= 0.0) continue;

        float sh = shadowFactor(P + N * 0.02, L, d - 0.04);
        if (sh <= 0.0) continue;

        vec3 c = lights.pointColorInt[i].rgb * lights.pointColorInt[i].w;
        lit += blinnPhong(albedo, N, V, L, 32.0, 0.25) * c * distAtten(d, r) * sh;
    }

    // Фонарик (spot от камеры)
    if (lights.spotColor.w > 0.0) {
        vec3  Lv = lights.spotPosCosIn.xyz - P;
        float d  = length(Lv);
        vec3  L  = Lv / d;
        float cd   = dot(-L, normalize(lights.spotDirCosOut.xyz));
        float cone = smoothstep(lights.spotDirCosOut.w, lights.spotPosCosIn.w, cd);
        if (cone > 0.0 && dot(N, L) > 0.0) {
            float sh = shadowFactor(P + N * 0.02, L, d - 0.04);
            if (sh > 0.0) {
                float att = 1.0 / (d * d * 0.05 + 1.0);
                vec3  c   = lights.spotColor.rgb * lights.spotColor.w;
                lit += blinnPhong(albedo, N, V, L, 48.0, 0.35) * c * cone * att * sh;
            }
        }
    }
    return lit;
}

// ─── Динамический туман ─────────────────────────────────────────────────────
// Дешёвое value-noise для дрейфа плотности.
float wsimHash13(vec3 p) {
    p  = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}
float wsimVNoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(wsimHash13(i + vec3(0,0,0)), wsimHash13(i + vec3(1,0,0)), f.x),
                   mix(wsimHash13(i + vec3(0,1,0)), wsimHash13(i + vec3(1,1,0)), f.x), f.y),
               mix(mix(wsimHash13(i + vec3(0,0,1)), wsimHash13(i + vec3(1,0,1)), f.x),
                   mix(wsimHash13(i + vec3(0,1,1)), wsimHash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

// ─── Процедурный бетон (3D value-noise в world space, без UV) ────────────────
// 3D-шум бесшовен на любой ориентации грани → не нужен triplanar/развёртка.
// concreteH — поле «высоты» поверхности (2 октавы fbm).
float concreteH(vec3 p) {
    return wsimVNoise(p * 3.5) * 0.65 + wsimVNoise(p * 9.45) * 0.30;
}
// Пятнистое потемнение albedo + бамп нормали из градиента высоты (surface
// gradient: убираем компоненту вдоль N, сдвигаем нормаль по тангенциальной).
// Применять только к бетону (маска — fragColor.a у вызывающего).
vec3 concreteSurface(vec3 P, inout vec3 N) {
    const float e = 0.06;
    float h  = concreteH(P);
    vec3  grad = (vec3(concreteH(P + vec3(e, 0, 0)),
                       concreteH(P + vec3(0, e, 0)),
                       concreteH(P + vec3(0, 0, e))) - h) / e;
    N = normalize(N - 0.05 * (grad - N * dot(grad, N)));

    // Вертикальные потёки грязи (шум растянут по Y) + каверна: в углублениях
    // (низкий h) темнее, подчёркивая микрорельеф под скользящим фонариком.
    float grime  = wsimVNoise(vec3(P.x * 7.0, P.y * 0.6, P.z * 7.0));
    float cavity = mix(0.72, 1.0, smoothstep(0.05, 0.35, h));

    // Детерминированная вариация по этажу: floorIndex = worldY/H, хэш с сидом
    // (fogParams.z = высота этажа, .w = сид). Функция от (этаж, сид) → одинакова
    // на всех клиентах (кооп-детерминизм), но этажи не клоны.
    vec3  tint = vec3(1.0);
    float fvar = 1.0;
    float H = lights.fogParams.z;
    if (H > 0.0) {
        float fi = floor(P.y / H + 0.001);
        float fh = fract(sin(fi * 12.9898 + lights.fogParams.w * 7.13) * 43758.5453);
        fvar = 0.85 + 0.30 * fh;                                  // светлее/темнее
        tint = mix(vec3(1.0), vec3(1.05, 0.98, 0.92), fh);       // тёплый↔холодный разброс
    }
    return vec3((0.80 + 0.35 * h) * cavity * (0.86 + 0.14 * grime) * fvar) * tint;
}

// Экспоненциальный туман: высотный спад (гуще у пола) + дрейф плотности во времени.
vec3 applyFog(vec3 color, vec3 P, vec3 camPos) {
    float density = lights.fogColorDensity.w;
    if (density <= 0.0) return color;
    float dist    = length(P - camPos);
    float t       = lights.fogParams.y;
    float heightF = exp(-max(P.y, 0.0) * lights.fogParams.x);
    float swirl   = wsimVNoise(P * 0.25 + vec3(t * 0.06, t * 0.02, t * 0.04));
    float d       = density * heightF * (0.55 + 0.9 * swirl);
    float f       = clamp(1.0 - exp(-dist * d), 0.0, 1.0);
    return mix(color, lights.fogColorDensity.rgb, f);
}

// Reinhard tonemap + гамма-кодирование: offscreen (UNORM8) хранит
// перцептивные значения, composite.frag декодирует перед sRGB-выводом.
vec3 tonemapEncode(vec3 hdr) {
    vec3 x = hdr / (hdr + vec3(1.0));
    return pow(x, vec3(1.0 / 2.2));
}
