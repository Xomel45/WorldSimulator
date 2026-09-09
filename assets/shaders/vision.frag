#version 460

// ─── Симуляция зрения: depth-based defocus ──────────────────────────────────
// Один gather-проход по диску: радиус размытия ∝ дефокусу в диоптриях вне
// «зоны резкости» [nearSharp, farSharp]. Близорукость размывает даль
// (> farSharp), дальнозоркость — близь (< nearSharp). Расстояние берётся из
// viewDist-таргета (length(camPos - fragPos)), записанного scene-шейдером.

layout(set = 0, binding = 0) uniform sampler2D colorTex;  // tonemap-encoded сцена
layout(set = 0, binding = 1) uniform sampler2D distTex;   // R16F, метры

layout(location = 0) in  vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    float nearSharp;   // ближняя граница резкости, м (0 = дальнозоркости нет)
    float farSharp;    // дальняя граница резкости, м (0 = близорукости нет)
    float strength;    // диоптрии → пиксели радиуса (0 = проход-passthrough)
    float maxRadius;   // кламп радиуса CoC, пиксели
} pc;

// Радиус круга размытия (CoC) в пикселях для фрагмента на расстоянии dist.
float cocPixels(float dist) {
    float invd    = 1.0 / max(dist, 0.05);
    float nearInv = pc.nearSharp > 0.0 ? 1.0 / pc.nearSharp : 1e9;  // близь: > nearInv резко
    float farInv  = pc.farSharp  > 0.0 ? 1.0 / pc.farSharp  : 0.0;  // даль:  < farInv  резко
    // Вне зоны — дефокус в диоптриях (одно из слагаемых всегда 0)
    float diopters = max(0.0, invd - nearInv) + max(0.0, farInv - invd);
    return clamp(diopters * pc.strength, 0.0, pc.maxRadius);
}

void main() {
    float centerCoc = cocPixels(texture(distTex, fragUV).r);

    // strength=0 → passthrough (copy-back проход); резкая точка → без сбора
    if (pc.strength <= 0.0 || centerCoc < 0.75) {
        outColor = vec4(texture(colorTex, fragUV).rgb, 1.0);
        return;
    }

    const int   N  = 24;
    const float GA = 2.39996323;  // золотой угол, рад

    vec2 texel = 1.0 / vec2(textureSize(colorTex, 0));
    vec3  acc  = texture(colorTex, fragUV).rgb;  // центр всегда учитываем
    float wsum = 1.0;

    for (int i = 0; i < N; ++i) {
        float t = (float(i) + 0.5) / float(N);
        float r = sqrt(t) * centerCoc;           // равномерное покрытие диска
        float a = float(i) * GA;
        vec2  uv = fragUV + vec2(cos(a), sin(a)) * r * texel;

        // scatter-as-gather: сэмпл влияет тем сильнее, чем дальше «дотягивается»
        // его собственный CoC — гасит подтекание резкого фона в размытую область.
        float sampCoc = cocPixels(texture(distTex, uv).r);
        float w = clamp(sampCoc - r + 1.0, 0.0, 1.0);

        acc  += texture(colorTex, uv).rgb * w;
        wsum += w;
    }

    outColor = vec4(acc / wsum, 1.0);
}
