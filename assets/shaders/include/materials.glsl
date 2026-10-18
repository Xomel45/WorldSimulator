// materials.glsl -- процедурные материалы поверхностей (без текстур-файлов).
// Подключать после lighting.glsl (нужны wsimVNoise, lights). Материал выбирает
// fragColor.a: 0.25 -- рельеф, 0.5 -- вода, 0.75 -- существа; любое другое
// (1.0) -- цвет вершины как есть. Всё считается в мировых координатах, поэтому
// чанки разных уровней детализации стыкуются без швов и без UV-развёрток.

const float MAT_TERRAIN  = 0.25;
const float MAT_WATER    = 0.50;
const float MAT_CREATURE = 0.75;

bool matIs(float a, float code) { return abs(a - code) < 0.05; }

// Фрактальный шум 0..1 (4 октавы).
float wsimFbm(vec3 p) {
    float a = 0.5, s = 0.0;
    for (int i = 0; i < 4; ++i) {
        s += a * wsimVNoise(p);
        p = p * 2.03 + 17.0;
        a *= 0.5;
    }
    return s / 0.9375;
}

// Поправка нормали по градиенту поля высоты h(P) (surface gradient, как у бамп-мапы).
vec3 bumpNormal(vec3 N, vec3 grad, float strength) {
    return normalize(N - strength * (grad - N * dot(grad, N)));
}

// Рельеф: биомы по высоте и уклону, пятна травы/сухой травы/проплешин, пляж,
// камень на склонах, снег на вершинах (высокие горы от силы бога белеют сами).
vec3 terrainMaterial(vec3 P, inout vec3 N, float dist) {
    float h     = P.y;
    float large = wsimFbm(P * 0.045 + 5.0);
    float mid   = wsimVNoise(P * 0.8 + 11.0);
    float fine  = wsimVNoise(P * 3.7 + 3.0);
    float grain = wsimVNoise(P * 11.0);
    float far   = smoothstep(80.0, 320.0, dist);  // вдали мелочь сливается, иначе мерцает
    float slope = 1.0 - clamp(N.y, 0.0, 1.0);

    vec3 grass = mix(vec3(0.17, 0.31, 0.09), vec3(0.33, 0.45, 0.14), smoothstep(0.3, 0.7, large));
    vec3 dry   = vec3(0.50, 0.44, 0.20);
    vec3 dirt  = vec3(0.33, 0.25, 0.16);
    // Камень: пятна оттенка, слоистость по высоте (напластования) и тёмные трещины.
    float strata = 0.5 + 0.5 * sin(P.y * 1.6 + large * 7.0 + mid * 2.0);
    vec3 rock  = mix(vec3(0.27, 0.26, 0.26), vec3(0.55, 0.51, 0.46), wsimFbm(P * 0.22 + 31.0));
    rock *= 0.82 + 0.28 * strata;
    rock *= 0.9 + 0.2 * smoothstep(0.35, 0.7, fine);
    vec3 sand  = vec3(0.78, 0.69, 0.47);
    vec3 snow  = vec3(0.93, 0.95, 0.98);

    vec3  c     = mix(grass, dry, smoothstep(0.55, 0.85, large) * 0.5);
    c           = mix(c, dirt, smoothstep(0.7, 0.85, mid + (large - 0.5) * 0.4) * 0.22);
    float beach = 1.0 - smoothstep(0.15, 0.9 + mid * 0.5, h);
    c           = mix(c, sand, beach);
    float rockW = smoothstep(0.28, 0.5, slope + (mid - 0.5) * 0.12 + max(h - 6.0, 0.0) * 0.02);
    c           = mix(c, rock, rockW);
    float snowW = smoothstep(10.0, 13.0, h + (large - 0.5) * 3.0) * (1.0 - rockW * 0.6);
    c           = mix(c, snow, snowW);

    // Зернистость и микрорельеф: слабее вдали и на снегу.
    float near = 1.0 - far;
    c *= 1.0 + ((fine - 0.5) * 0.12 + (grain - 0.5) * 0.06) * near;
    const float e = 0.15;
    vec3 grad = vec3(wsimVNoise((P + vec3(e, 0, 0)) * 3.7 + 3.0) - fine, 0.0,
                     wsimVNoise((P + vec3(0, 0, e)) * 3.7 + 3.0) - fine) / e;
    N = bumpNormal(N, grad, 0.035 * near * (1.0 - snowW * 0.6));
    return c;
}

// Волны: два слоя шума, бегущие в разные стороны.
float waterWave(vec2 p, float t) {
    return wsimVNoise(vec3(p * 0.9 + vec2(t * 0.35, t * 0.20), t * 0.15)) * 0.6 +
           wsimVNoise(vec3(p * 2.3 - vec2(t * 0.50, -t * 0.30), t * 0.25)) * 0.4;
}

// Вода: бегущая рябь нормали, цвет мутнее к краю карты пятен глубины, френель
// возвращается наружу -- смесь с цветом неба считает вызывающий.
vec3 waterMaterial(vec3 P, inout vec3 N, vec3 V, float dist, out float fresnel) {
    float t = lights.fogParams.y;
    const float e = 0.08;
    float w0 = waterWave(P.xz, t);
    vec3 grad = vec3((waterWave(P.xz + vec2(e, 0.0), t) - w0) / e, 0.0,
                     (waterWave(P.xz + vec2(0.0, e), t) - w0) / e);
    N = bumpNormal(N, grad, 0.30 * (1.0 - smoothstep(60.0, 420.0, dist)));
    fresnel = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);
    float depthPatch = wsimFbm(vec3(P.xz * 0.03, 1.7));
    return mix(vec3(0.03, 0.17, 0.27), vec3(0.09, 0.40, 0.47), depthPatch);
}

// Существа: цвет вершины с пятнистой "шкурой" и лёгким градиентом по высоте тела.
vec3 creatureMaterial(vec3 base, vec3 P) {
    float spots = wsimVNoise(P * 5.0 + 9.0);
    return base * (0.88 + 0.24 * spots);
}
