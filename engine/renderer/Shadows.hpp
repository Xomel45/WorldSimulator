#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace wsim {

inline constexpr int kShadowCascades = 4;  // = WSIM_SHADOW_CASCADES в lighting.glsl

// Каскадные карты теней (CSM) для направленного света. Чистая математика без
// Vulkan: считает для каждого каскада ортографическую матрицу света, которая
// покрывает свой кусок усечённой пирамиды камеры. Каскад 0 -- ближний и самый
// детальный, дальше шире и грубее.
struct CascadeSet {
    std::array<glm::mat4, kShadowCascades> viewProj{};    // мировые координаты -> клип света
    std::array<float, kShadowCascades>     splitFar{};    // дальняя граница каскада по глубине вида (юниты)
    std::array<float, kShadowCascades>     texelWorld{};  // размер текселя карты в мировых юнитах (для смещения)
    std::array<glm::vec3, kShadowCascades> center{};      // центр описанной сферы куска (для кэша далёких каскадов)
    std::array<float, kShadowCascades>     radius{};      // её радиус (с учётом inflate)
};

// Границы каскадов по глубине вида подобраны под god-камеру: дистанция от
// камеры до земли 4..200, обзор у горизонта далёкий, а тени дальше kShadowFar
// уже не нужны (плавно гаснут в шейдере).
inline constexpr std::array<float, kShadowCascades> kCascadeFar = {25.0f, 80.0f, 240.0f, 700.0f};

// view -- матрица вида камеры; proj -- её проекция (Vulkan: proj[1][1] < 0,
// из неё берём угол обзора и аспект); nearPlane -- ближняя плоскость камеры;
// toSun -- нормализованное направление К солнцу; mapSize -- сторона карты в пикселях.
[[nodiscard]] inline CascadeSet computeCascades(const glm::mat4& view, const glm::mat4& proj, float nearPlane,
                                                const glm::vec3& toSun, int mapSize,
                                                const std::array<float, kShadowCascades>& inflate = {1.0f, 1.0f, 1.0f, 1.0f}) {
    CascadeSet out;
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 camPos  = glm::vec3(invView[3]);
    const glm::vec3 right   = glm::normalize(glm::vec3(invView[0]));
    const glm::vec3 up      = glm::normalize(glm::vec3(invView[1]));
    const glm::vec3 fwd     = -glm::normalize(glm::vec3(invView[2]));
    const float tanHalfY    = 1.0f / std::abs(proj[1][1]);
    const float tanHalfX    = 1.0f / std::abs(proj[0][0]);

    // Дальность, на которую простираются тени от тел за пределами видимого куска:
    // горы, отбрасывающие тень в кадр, могут быть дальше к солнцу.
    constexpr float kCasterRange = 400.0f;

    float sliceNear = nearPlane;
    for (int i = 0; i < kShadowCascades; ++i) {
        const float sliceFar = kCascadeFar[static_cast<std::size_t>(i)];

        // Восемь углов куска пирамиды и описанная сфера: сфера не зависит от
        // поворота камеры, поэтому тени не "плывут" при её вращении.
        glm::vec3 corners[8];
        int       k = 0;
        for (float d : {sliceNear, sliceFar})
            for (float sy : {-1.0f, 1.0f})
                for (float sx : {-1.0f, 1.0f})
                    corners[k++] = camPos + fwd * d + right * (sx * tanHalfX * d) + up * (sy * tanHalfY * d);
        glm::vec3 center(0.0f);
        for (const auto& c : corners) center += c;
        center /= 8.0f;
        float radius = 0.0f;
        for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
        radius = std::ceil(radius * inflate[static_cast<std::size_t>(i)] * 16.0f) / 16.0f;   // inflate > 1: запас для кэшированных каскадов

        // Свет смотрит из-за пределов сферы в сторону центра; ортобокс = сфера + дальность теней.
        const glm::vec3 lightUp = std::abs(toSun.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 eye     = center + toSun * (radius + kCasterRange);
        glm::mat4       lightView = glm::lookAt(eye, center, lightUp);

        // Привязка центра к сетке текселей: без неё границы теней шевелятся при движении камеры.
        const float texel = 2.0f * radius / static_cast<float>(mapSize);
        glm::vec4   c     = lightView * glm::vec4(center, 1.0f);
        const glm::vec2 snapped(std::round(c.x / texel) * texel, std::round(c.y / texel) * texel);
        lightView = glm::translate(glm::mat4(1.0f), glm::vec3(snapped.x - c.x, snapped.y - c.y, 0.0f)) * lightView;

        const glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + kCasterRange);
        out.viewProj[static_cast<std::size_t>(i)]   = lightProj * lightView;
        out.splitFar[static_cast<std::size_t>(i)]   = sliceFar;
        out.texelWorld[static_cast<std::size_t>(i)] = texel;
        out.center[static_cast<std::size_t>(i)]     = center;
        out.radius[static_cast<std::size_t>(i)]     = radius;
        sliceNear = sliceFar;
    }
    return out;
}

}  // namespace wsim
