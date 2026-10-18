// lighting.glsl — общая модель освещения (Blinn-Phong) для scene.frag / scene_rt.frag.
// Подключающий шейдер обязан:
//   1. объявить CameraUBO (set=0, binding=0) ДО include
//   2. определить float shadowFactor(vec3 origin, vec3 dir, float maxDist)
//      (1.0 = полностью освещено, 0.0 = полная тень) -- для точечных/фонарика
//   3. тени от солнца: растровый шейдер сцены делает #define WSIM_RASTER_SHADOWS до
//      include и получает каскадную карту теней (sunShadow ниже, binding 4); остальные
//      (RT-варианты, path tracer) -- солнечный луч через их же shadowFactor

#define WSIM_MAX_POINT_LIGHTS 8
#define WSIM_SHADOW_CASCADES 4

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
    mat4  cascadeVP[WSIM_SHADOW_CASCADES];         // мир -> клип света по каскадам
    vec4  cascadeSplit;                            // дальние границы каскадов по глубине вида
    vec4  cascadeTexel;                            // размер текселя каскада в юнитах мира
    vec4  shadowParams;                            // x = тени включены, y = 1/размер карты, z = смещение глубины
} lights;

#ifdef WSIM_RASTER_SHADOWS
layout(set = 0, binding = 4) uniform sampler2DArrayShadow shadowMap;
#endif

float shadowFactor(vec3 origin, vec3 dir, float maxDist);
float sunShadow(vec3 P, vec3 N);

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
            float sh = sunShadow(P, N);
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

// ─── Тени от солнца: каскадная карта ─────────────────────────────────────────
#ifdef WSIM_RASTER_SHADOWS
float wsimIgn(vec2 p) {  // interleaved gradient noise: дешёвый шум на пиксель, чтобы PCF не давал полос
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float sunShadow(vec3 P, vec3 N) {
    if (lights.shadowParams.x < 0.5) return 1.0;
    float depth = -(view * vec4(P, 1.0)).z;
    if (depth >= lights.cascadeSplit.w) return 1.0;
    int c = depth < lights.cascadeSplit.x ? 0 : depth < lights.cascadeSplit.y ? 1 : depth < lights.cascadeSplit.z ? 2 : 3;

    // Нормальное смещение: сдвигаем точку вдоль нормали на пару текселей, сильнее на скользящем свету.
    vec3  L   = normalize(lights.sunDir.xyz);
    float ndl = clamp(dot(N, L), 0.0, 1.0);
    vec3  Pn  = P + N * lights.cascadeTexel[c] * (1.0 + 2.5 * (1.0 - ndl));
    vec4  sc  = lights.cascadeVP[c] * vec4(Pn, 1.0);
    vec3  p   = sc.xyz / sc.w;
    vec2  uv  = p.xy * 0.5 + 0.5;
    // Смещение глубины в единицах карты: глубина каскада D = 1/|ряд z матрицы|; берём
    // ~1.5 текселя плюс добавка на скользящем свету (наклон поверхности к солнцу).
    float invD = length(vec3(lights.cascadeVP[c][0][2], lights.cascadeVP[c][1][2], lights.cascadeVP[c][2][2]));
    float tanT = sqrt(max(1.0 - ndl * ndl, 0.0)) / max(ndl, 0.15);
    float ref  = p.z - invD * lights.cascadeTexel[c] * lights.shadowParams.z * (1.5 + 0.5 * min(tanT, 4.0));

    // PCF по диску Вогеля с поворотом на пиксель (12 точек, каждая -- ещё и аппаратное 2x2).
    const int   kTaps   = 12;
    float       rot     = 6.2831853 * wsimIgn(gl_FragCoord.xy);
    float       radius  = (1.5 + 0.35 * float(c)) * lights.shadowParams.y;  // дальние каскады мягче
    float       lit     = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float r = sqrt((float(i) + 0.5) / float(kTaps));
        float a = float(i) * 2.39996323 + rot;
        lit += texture(shadowMap, vec4(uv + vec2(cos(a), sin(a)) * r * radius, float(c), ref));
    }
    lit /= float(kTaps);

    float fade = smoothstep(lights.cascadeSplit.w * 0.8, lights.cascadeSplit.w, depth);
    return mix(lit, 1.0, fade);
}
#else
float sunShadow(vec3 P, vec3 N) {
    return shadowFactor(P + N * 0.02, normalize(lights.sunDir.xyz), 1000.0);
}
#endif

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
